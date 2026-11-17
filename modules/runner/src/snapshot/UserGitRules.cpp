#include "snapshot/UserGitRules.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace ketplus::runner::snapshot {
namespace {

bool isRegularFile(const QString& path) {
#ifdef Q_OS_UNIX
    struct stat info{};
    return ::stat(QFile::encodeName(path).constData(), &info) == 0 && S_ISREG(info.st_mode);
#else
    return QFileInfo(path).isFile();
#endif
}

// Missing or not a regular file: empty. Over the limit: an error, since dropping the file
// would put ignored or filtered paths into the snapshot.
Result<QByteArray> readRulesFile(const QString& path) {
    if (path.isEmpty() || !isRegularFile(path))
        return QByteArray();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    const QByteArray content = file.read(maxUserRulesFileBytes + 1);
    if (content.size() > maxUserRulesFileBytes) {
        return Error{
            ErrorCode::UnsupportedRepository,
            QStringLiteral("%1 is larger than %2 bytes").arg(path).arg(maxUserRulesFileBytes)};
    }
    return content;
}

} // namespace

UserGitSources userGitSourcesFromEnvironment() {
    return {QDir::homePath(), qEnvironmentVariable("XDG_CONFIG_HOME"),
            qEnvironmentVariable("GIT_CONFIG_GLOBAL")};
}

Result<UserGitRules> readUserGitRules(const SealedGit& git, const RepositoryLayout& layout,
                                      const UserGitSources& sources) {
    const auto query = git.queryUserConfig(
        layout.gitDir, layout.workTree, sources.home, sources.xdgConfigHome, sources.globalConfig,
        QStringLiteral("^core\\.(excludesfile|attributesfile)$"));
    // Exit status 1 is "no such key"; anything else (a malformed config) fails closed.
    if (!query && query.error().gitExitCode != 1)
        return query.error();

    // Output lines are `key value`, in the order git reads them; the last one wins.
    QString excludes;
    QString attributes;
    if (query) {
        for (const QString& line : QString::fromUtf8(query.value().out).split(QLatin1Char('\n'))) {
            const qsizetype space = line.indexOf(QLatin1Char(' '));
            if (space <= 0)
                continue;
            const QString key = line.left(space).toLower();
            const QString value = line.mid(space + 1);
            if (key == QStringLiteral("core.excludesfile"))
                excludes = value;
            else if (key == QStringLiteral("core.attributesfile"))
                attributes = value;
        }
    }
    // Defaults as in git; a relative value is taken from the worktree root.
    const QString xdg = !sources.xdgConfigHome.isEmpty() ? sources.xdgConfigHome
                        : !sources.home.isEmpty()
                            ? QDir(sources.home).filePath(QStringLiteral(".config"))
                            : QString();
    const auto resolve = [&](const QString& value, const QString& fallback) {
        if (value.isEmpty())
            return xdg.isEmpty() ? QString() : QDir(xdg).filePath(fallback);
        return QDir::isAbsolutePath(value) ? value : QDir(layout.workTree).filePath(value);
    };

    UserGitRules rules;
    const auto excludeContent = readRulesFile(resolve(excludes, QStringLiteral("git/ignore")));
    if (!excludeContent)
        return excludeContent.error();
    const auto attributeContent =
        readRulesFile(resolve(attributes, QStringLiteral("git/attributes")));
    if (!attributeContent)
        return attributeContent.error();
    rules.excludes = excludeContent.value();
    rules.attributes = attributeContent.value();
    return rules;
}

} // namespace ketplus::runner::snapshot
