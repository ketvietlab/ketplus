#include "snapshot/ExcludeRules.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace ketplus::runner::snapshot {
namespace {

QByteArray readBounded(const QString& path) {
    QFile file(path);
    if (path.isEmpty() || !QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly) ||
        file.size() > maxExcludeFileBytes)
        return {};
    return file.read(maxExcludeFileBytes);
}

// What git's --path expansion does for core.excludesFile: `~/` is the home directory and a
// relative path is taken from the worktree root. `~user/` is not supported.
QString expandPath(const QString& value, const QString& home, const QString& workTree) {
    if (value.isEmpty())
        return {};
    if (value == QStringLiteral("~"))
        return home;
    if (value.startsWith(QStringLiteral("~/")))
        return home.isEmpty() ? QString() : QDir(home).filePath(value.mid(2));
    if (value.startsWith(QLatin1Char('~')))
        return {};
    return QDir::isAbsolutePath(value) ? value : QDir(workTree).filePath(value);
}

} // namespace

ExcludeSources excludeSourcesFromEnvironment() {
    return {QDir::homePath(), qEnvironmentVariable("XDG_CONFIG_HOME"),
            qEnvironmentVariable("GIT_CONFIG_GLOBAL")};
}

QByteArray collectExcludeRules(const SealedGit& git, const RepositoryLayout& layout,
                               const ExcludeSources& sources) {
    const QString xdg = !sources.xdgConfigHome.isEmpty() ? sources.xdgConfigHome
                        : !sources.home.isEmpty()
                            ? QDir(sources.home).filePath(QStringLiteral(".config"))
                            : QString();
    // Lowest precedence first; the last file that sets the key wins, as in git.
    QStringList configs;
    if (!sources.globalConfig.isEmpty()) {
        configs << sources.globalConfig;
    } else {
        if (!xdg.isEmpty())
            configs << QDir(xdg).filePath(QStringLiteral("git/config"));
        if (!sources.home.isEmpty())
            configs << QDir(sources.home).filePath(QStringLiteral(".gitconfig"));
    }
    configs << QDir(layout.commonDir).filePath(QStringLiteral("config"))
            << QDir(layout.gitDir).filePath(QStringLiteral("config.worktree"));
    QString configured;
    for (const QString& config : configs) {
        const QString value = git.readConfigValue(config, QStringLiteral("core.excludesFile"));
        if (!value.isEmpty())
            configured = value;
    }
    const QString globalExcludes =
        !configured.isEmpty()
            ? expandPath(configured, sources.home, layout.workTree)
            : (xdg.isEmpty() ? QString() : QDir(xdg).filePath(QStringLiteral("git/ignore")));

    QByteArray rules = readBounded(globalExcludes);
    if (!rules.isEmpty() && !rules.endsWith('\n'))
        rules += '\n';
    rules += readBounded(layout.excludeFile);
    return rules;
}

} // namespace ketplus::runner::snapshot
