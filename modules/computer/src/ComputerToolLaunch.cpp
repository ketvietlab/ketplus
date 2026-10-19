#include "ComputerToolLaunch.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>

namespace ketplus::computer_tools {
namespace {

constexpr QFileDevice::Permissions ownerOnlyFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
constexpr QFileDevice::Permissions ownerOnlyDirectory = ownerOnlyFile | QFileDevice::ExeOwner;
constexpr qint64 maximumSettingsBytes = 64 * 1024;

QString dataRoot() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("Ketsuite/KetPlus"));
}

bool ensurePrivateDirectory(const QString& directory) {
    return QDir().mkpath(directory) && QFile::setPermissions(directory, ownerOnlyDirectory);
}

QJsonObject readObject(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximumSettingsBytes)
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QString frontIn(const QString& appBundle) {
    const QString front = QDir(appBundle).filePath(QLatin1String(frontExecutable));
    return QFileInfo(front).isExecutable() ? QFileInfo(front).absoluteFilePath() : QString();
}

} // namespace

QString settingsFilePath() {
    const QString overridePath = qEnvironmentVariable("KETPLUS_COMPUTER_TOOLS_CONFIG");
    return overridePath.isEmpty() ? QDir(dataRoot()).filePath(QStringLiteral("computer-tools.json"))
                                  : overridePath;
}

QString agentDirectory() {
    const QString overridePath = qEnvironmentVariable("KETPLUS_COMPUTER_DIR");
    return overridePath.isEmpty() ? QDir(dataRoot()).filePath(QStringLiteral("computer"))
                                  : overridePath;
}

Settings loadSettings(const QString& path) {
    const QJsonObject object = readObject(path);
    Settings settings;
    settings.enabled = object.value(QStringLiteral("enabled")).toBool(false);
    for (const auto& value : object.value(QStringLiteral("deniedApps")).toArray()) {
        const QString id = value.toString().trimmed();
        if (!id.isEmpty() && !settings.deniedApps.contains(id))
            settings.deniedApps.append(id);
    }
    return settings;
}

bool saveSettings(const Settings& settings, const QString& path) {
    QJsonObject object = readObject(path);
    object.insert(QStringLiteral("enabled"), settings.enabled);
    object.insert(QStringLiteral("deniedApps"), QJsonArray::fromStringList(settings.deniedApps));
    if (!ensurePrivateDirectory(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit() && QFile::setPermissions(path, ownerOnlyFile);
}

QString findFrontExecutable(const QString& applicationDir) {
    const QString overrideApp = qEnvironmentVariable("KETPLUS_COMPUTER_APP");
    if (!overrideApp.isEmpty())
        return frontIn(overrideApp);
    const QDir dir(applicationDir);
    for (const QString& candidate :
         {dir.filePath(QLatin1String(agentAppName)),
          dir.filePath(QStringLiteral("../Helpers/") + QLatin1String(agentAppName))}) {
        if (const QString front = frontIn(candidate); !front.isEmpty())
            return front;
    }
    return {};
}

QByteArray ensureHostKey(const QString& directory) {
    if (!ensurePrivateDirectory(directory))
        return {};
    const QString path = QDir(directory).filePath(QStringLiteral("host.key"));
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly)) {
        const QByteArray key = existing.read(64);
        if (key.size() == 32)
            return key;
    }
    QByteArray key(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(key.begin(), key.end());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.setPermissions(ownerOnlyFile);
    file.write(key);
    if (!file.commit() || !QFile::setPermissions(path, ownerOnlyFile))
        return {};
    return key;
}

QString mintToken(const QByteArray& key, const QString& conversationId) {
    return QString::fromLatin1(QMessageAuthenticationCode::hash(QByteArrayLiteral("computer.v0:") +
                                                                    conversationId.toUtf8(),
                                                                key, QCryptographicHash::Sha256)
                                   .toHex());
}

QString fileStem(const QString& conversationId) {
    return QString::fromLatin1(
        QCryptographicHash::hash(conversationId.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(32));
}

std::optional<Launch> launchForConversation(const Settings& settings, const QString& conversationId,
                                            const QString& applicationDir,
                                            const QString& directory) {
    if (!settings.enabled || conversationId.isEmpty())
        return std::nullopt;
    const QString front = findFrontExecutable(applicationDir);
    if (front.isEmpty())
        return std::nullopt;
    const QByteArray key = ensureHostKey(directory);
    if (key.isEmpty())
        return std::nullopt;
    Launch launch;
    launch.command = front;
    launch.env.insert(QStringLiteral("KETPLUS_COMPUTER_CONVERSATION"), conversationId);
    launch.env.insert(QStringLiteral("KETPLUS_COMPUTER_TOKEN"), mintToken(key, conversationId));
    if (qEnvironmentVariableIsSet("KETPLUS_COMPUTER_DIR"))
        launch.env.insert(QStringLiteral("KETPLUS_COMPUTER_DIR"), directory);
    return launch;
}

std::optional<Launch> launchFromUserSettings(const QString& conversationId,
                                             const QString& applicationDir) {
    return launchForConversation(loadSettings(), conversationId, applicationDir);
}

QJsonObject withClaudeMcpServer(QJsonObject servers, const std::optional<Launch>& launch) {
    if (!launch)
        return servers;
    QJsonObject env;
    for (auto it = launch->env.cbegin(); it != launch->env.cend(); ++it)
        env.insert(it.key(), it.value());
    servers.insert(QLatin1String(serverName),
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("stdio")},
                               {QStringLiteral("command"), launch->command},
                               {QStringLiteral("args"), QJsonArray::fromStringList(launch->args)},
                               {QStringLiteral("env"), env}});
    return servers;
}

QJsonArray withAcpMcpServer(QJsonArray servers, const std::optional<Launch>& launch) {
    if (!launch)
        return servers;
    QJsonArray env;
    for (auto it = launch->env.cbegin(); it != launch->env.cend(); ++it)
        env.append(
            QJsonObject{{QStringLiteral("name"), it.key()}, {QStringLiteral("value"), it.value()}});
    servers.append(QJsonObject{{QStringLiteral("name"), QLatin1String(serverName)},
                               {QStringLiteral("command"), launch->command},
                               {QStringLiteral("args"), QJsonArray::fromStringList(launch->args)},
                               {QStringLiteral("env"), env}});
    return servers;
}

bool setRemoteTurn(const QString& conversationId, const bool remote, const QString& directory) {
    if (conversationId.isEmpty())
        return false;
    const QString markers = QDir(directory).filePath(QStringLiteral("remote-turns"));
    const QString path = QDir(markers).filePath(fileStem(conversationId));
    if (!remote)
        return !QFileInfo::exists(path) || QFile::remove(path);
    if (!ensurePrivateDirectory(directory) || !ensurePrivateDirectory(markers))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(ownerOnlyFile);
}

} // namespace ketplus::computer_tools
