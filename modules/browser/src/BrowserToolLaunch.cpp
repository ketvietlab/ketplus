#include "BrowserToolLaunch.h"

#include "BundledBrowserRuntime.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace ketplus::browser_tools {

std::optional<ToolLaunch> configuredBrowserLaunch(const Settings& settings,
                                                  const QString& searchStart,
                                                  const QString& artifactsDirectory) {
    if (!settings.enabled || artifactsDirectory.isEmpty())
        return std::nullopt;
    QString error;
    const QString origins = normalizeAllowedOrigins(settings.allowedOrigins, &error);
    if (!error.isEmpty())
        return std::nullopt;
    auto launch = resolveBrowserLaunch(settings, searchStart);
    if (!launch)
        return std::nullopt;
    for (const auto* name : {"PLAYWRIGHT_BROWSERS_PATH", "KETPLUS_BROWSER_ENGINE",
                             "KETPLUS_BROWSER_DOWNLOAD", "KETPLUS_BROWSER_CACHE"}) {
        const QString value = qEnvironmentVariable(name);
        if (!value.isEmpty())
            launch->env.insert(QLatin1String(name), value);
    }
    launch->env.insert(QStringLiteral("KETPLUS_BROWSER_ALLOWED_ORIGINS"), origins);
    launch->env.insert(QStringLiteral("KETPLUS_BROWSER_ARTIFACTS"), artifactsDirectory);
    return launch;
}

std::optional<ToolLaunch> browserLaunchFromUserSettings(const QString& searchStart) {
    Settings settings = loadSettings();
    if (!settings.enabled)
        return std::nullopt;
    settings.nodePath = effectiveNodePath(settings);
    const QString artifacts = defaultArtifactsDirectory();
    if (!QDir().mkpath(artifacts) ||
        !QFile::setPermissions(artifacts, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                              QFileDevice::ExeOwner))
        return std::nullopt;
    return configuredBrowserLaunch(settings, searchStart, artifacts);
}

QJsonObject claudeMcpServers(const std::optional<ToolLaunch>& launch) {
    if (!launch)
        return {};
    QJsonObject env;
    for (auto it = launch->env.cbegin(); it != launch->env.cend(); ++it)
        env.insert(it.key(), it.value());
    return {{QLatin1String(serverName),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("stdio")},
                         {QStringLiteral("command"), launch->command},
                         {QStringLiteral("args"), QJsonArray::fromStringList(launch->args)},
                         {QStringLiteral("env"), env}}}};
}

QJsonArray acpMcpServers(const std::optional<ToolLaunch>& launch) {
    if (!launch)
        return {};
    QJsonArray env;
    for (auto it = launch->env.cbegin(); it != launch->env.cend(); ++it)
        env.append(
            QJsonObject{{QStringLiteral("name"), it.key()}, {QStringLiteral("value"), it.value()}});
    // ACP stdio and Claude SDK have no MCP cwd field. Both entries are absolute;
    // the worker resolves assets relative to import.meta.url, never the worktree.
    return {QJsonObject{{QStringLiteral("name"), QLatin1String(serverName)},
                        {QStringLiteral("command"), launch->command},
                        {QStringLiteral("args"), QJsonArray::fromStringList(launch->args)},
                        {QStringLiteral("env"), env}}};
}

QString turnEndSignalFile(const QString& artifactsDirectory, const QString& conversationId) {
    if (artifactsDirectory.isEmpty() || conversationId.isEmpty())
        return {};
    const QByteArray digest =
        QCryptographicHash::hash(conversationId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(artifactsDirectory)
        .filePath(QStringLiteral("turn-ends/") + QString::fromLatin1(digest.left(32)));
}

bool signalTurnEnd(const QString& path) {
    if (path.isEmpty())
        return false;
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory) ||
        !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                              QFileDevice::ExeOwner))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QByteArray::number(QDateTime::currentMSecsSinceEpoch()) + '\n');
    return file.commit();
}

QString browserSkillFile(const Settings& settings, const QString& activePath,
                         const QString& applicationDir) {
    if (!settings.enabled)
        return {};
    if (const auto runtime = findBundledBrowserRuntime(applicationDir))
        return runtime->skillFile;
    QString appPath = settings.browserAppPath;
    if (appPath.isEmpty() && !activePath.isEmpty())
        appPath = detectBrowserAppPath(activePath);
    if (appPath.isEmpty())
        appPath = detectBrowserAppPath(applicationDir);
    if (appPath.isEmpty())
        return {};
    const QString skill =
        QDir(appPath).filePath(QStringLiteral(".claude/skills/ketplus-browser/SKILL.md"));
    return QFileInfo(skill).isFile() ? skill : QString();
}

} // namespace ketplus::browser_tools
