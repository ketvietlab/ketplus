#include "agent/TranscriptFileLinks.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace ketplus {

namespace {

constexpr qint64 cacheLifetimeMs = 30'000;
constexpr int maximumTrackedFiles = 200'000;

} // namespace

void TranscriptFileLinks::setRoot(const QString& root) {
    if (root == root_)
        return;
    root_ = root;
    cache_.clear();
    files_.clear();
    filesListed_ = false;
    age_.restart();
}

QString TranscriptFileLinks::href(const QString& text) {
    // A path, then an optional line (a range or column keeps its first line).
    static const QRegularExpression reference(
        QStringLiteral(R"(^(?:(?:\.\.\.|…)/)?([^\s:*?"<>|`]+?)(?::(\d+)(?:[-–]\d+)?(?::\d+)?)?$)"));
    static const QRegularExpression extension(QStringLiteral(R"(\.[A-Za-z0-9]{1,10}$)"));
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 512 || trimmed.contains(QStringLiteral("://")))
        return {};
    const auto match = reference.match(trimmed);
    if (!match.hasMatch())
        return {};
    const QString path = match.captured(1);
    // A bare word is an identifier, not a file; it needs a folder or an extension.
    if (!path.contains(QLatin1Char('/')) && !extension.match(path).hasMatch())
        return {};
    if (!age_.isValid() || age_.elapsed() > cacheLifetimeMs) {
        cache_.clear();
        files_.clear();
        filesListed_ = false;
        age_.restart();
    }
    const auto cached = cache_.constFind(path);
    const QString resolved = cached != cache_.cend() ? *cached : resolve(path);
    if (cached == cache_.cend())
        cache_.insert(path, resolved);
    if (resolved.isEmpty())
        return {};
    const QString line = match.captured(2);
    return line.isEmpty() ? resolved : resolved + QLatin1Char(':') + line;
}

QString TranscriptFileLinks::resolve(const QString& path) {
    if (path.startsWith(QStringLiteral("~/"))) {
        const QString absolute = QDir::home().filePath(path.mid(2));
        return QFileInfo(absolute).isFile() ? absolute : QString{};
    }
    if (QDir::isAbsolutePath(path))
        return QFileInfo(path).isFile() ? path : QString{};
    if (root_.isEmpty())
        return {};
    QString relative = path;
    while (relative.startsWith(QStringLiteral("./")))
        relative = relative.mid(2);
    if (relative.startsWith(QStringLiteral("../")))
        return {};
    if (QFileInfo(QDir(root_).filePath(relative)).isFile())
        return relative;
    // A path given from inside the tree opens when exactly one tracked file ends with it.
    const QString suffix = QLatin1Char('/') + relative;
    QString found;
    for (const QString& file : trackedFiles()) {
        if (!file.endsWith(suffix))
            continue;
        if (!found.isEmpty())
            return {};
        found = file;
    }
    return found;
}

const QStringList& TranscriptFileLinks::trackedFiles() {
    if (filesListed_)
        return files_;
    filesListed_ = true;
    QProcess git;
    git.setWorkingDirectory(root_);
    git.start(QStringLiteral("git"),
              {QStringLiteral("ls-files"), QStringLiteral("--cached"), QStringLiteral("--others"),
               QStringLiteral("--exclude-standard")});
    if (!git.waitForFinished(2'000) || git.exitStatus() != QProcess::NormalExit ||
        git.exitCode() != 0) {
        git.kill();
        git.waitForFinished(500);
        return files_;
    }
    files_ =
        QString::fromUtf8(git.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (files_.size() > maximumTrackedFiles)
        files_.clear();
    return files_;
}

} // namespace ketplus
