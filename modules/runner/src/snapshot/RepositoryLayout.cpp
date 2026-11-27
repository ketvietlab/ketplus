#include "snapshot/RepositoryLayout.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace ketplus::runner::snapshot {
namespace {

constexpr qint64 smallFileLimit = 64 * 1024;

std::optional<QByteArray> readSmallFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > smallFileLimit)
        return std::nullopt;
    return file.readAll();
}

QString resolve(const QString& base, const QString& path) {
    const QString absolute = QDir::isAbsolutePath(path) ? path : QDir(base).filePath(path);
    return QFileInfo(absolute).canonicalFilePath();
}

// extensions.objectFormat from the repository config. Only that one key is read; a config
// value elsewhere never changes how the store runs git.
QString objectFormat(const QString& commonDir) {
    const auto config = readSmallFile(QDir(commonDir).filePath(QStringLiteral("config")));
    if (!config)
        return QStringLiteral("sha1");
    static const QRegularExpression section(QStringLiteral("^\\s*\\[\\s*([A-Za-z0-9.-]+)"));
    static const QRegularExpression key(
        QStringLiteral("^\\s*objectformat\\s*=\\s*\"?([A-Za-z0-9]+)\"?\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    QString current;
    QString format = QStringLiteral("sha1");
    for (const QString& line : QString::fromUtf8(*config).split(QLatin1Char('\n'))) {
        if (const auto match = section.match(line); match.hasMatch()) {
            current = match.captured(1).toLower();
            continue;
        }
        if (current == QStringLiteral("extensions")) {
            if (const auto match = key.match(line); match.hasMatch())
                format = match.captured(1).toLower();
        }
    }
    return format;
}

} // namespace

Result<RepositoryLayout> readRepositoryLayout(const QString& worktreePath) {
    const QFileInfo root(worktreePath);
    if (!root.isAbsolute() || !root.isDir()) {
        return Error{ErrorCode::NotAWorktree,
                     QStringLiteral("not an absolute directory: %1").arg(worktreePath)};
    }
    RepositoryLayout layout;
    layout.workTree = root.canonicalFilePath();
    const QString dotGit = QDir(layout.workTree).filePath(QStringLiteral(".git"));
    const QFileInfo dotGitInfo(dotGit);
    if (dotGitInfo.isDir() && !dotGitInfo.isSymLink()) {
        layout.gitDir = dotGitInfo.canonicalFilePath();
    } else if (dotGitInfo.isFile() && !dotGitInfo.isSymLink()) {
        const auto content = readSmallFile(dotGit);
        static const QRegularExpression gitdir(QStringLiteral("^gitdir: (.+)$"));
        const auto match = content ? gitdir.match(QString::fromUtf8(*content).trimmed())
                                   : QRegularExpressionMatch();
        if (!match.hasMatch())
            return Error{ErrorCode::NotAWorktree, QStringLiteral("unreadable %1").arg(dotGit)};
        layout.gitDir = resolve(layout.workTree, match.captured(1));
    }
    if (layout.gitDir.isEmpty() || !QFileInfo(layout.gitDir).isDir())
        return Error{ErrorCode::NotAWorktree,
                     QStringLiteral("no git directory in %1").arg(worktreePath)};

    layout.commonDir = layout.gitDir;
    if (const auto common =
            readSmallFile(QDir(layout.gitDir).filePath(QStringLiteral("commondir")))) {
        layout.commonDir = resolve(layout.gitDir, QString::fromUtf8(*common).trimmed());
        if (layout.commonDir.isEmpty() || !QFileInfo(layout.commonDir).isDir()) {
            return Error{ErrorCode::NotAWorktree,
                         QStringLiteral("missing common directory for %1").arg(layout.gitDir)};
        }
    }
    layout.objectsDir = QDir(layout.commonDir).filePath(QStringLiteral("objects"));
    if (!QFileInfo(layout.objectsDir).isDir())
        return Error{ErrorCode::NotAWorktree,
                     QStringLiteral("no objects in %1").arg(layout.commonDir)};
    layout.indexFile = QDir(layout.gitDir).filePath(QStringLiteral("index"));
    layout.objectFormat = objectFormat(layout.commonDir);
    if (layout.objectFormat != QStringLiteral("sha1") &&
        layout.objectFormat != QStringLiteral("sha256")) {
        return Error{ErrorCode::UnsupportedRepository,
                     QStringLiteral("unknown object format %1").arg(layout.objectFormat)};
    }
    return layout;
}

} // namespace ketplus::runner::snapshot
