#include "app/QaSettingsIsolation.h"
#include "update/AppUpdater.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryFile>

namespace ketplus {
namespace {

constexpr auto kDirectoryVariable = "KETPLUS_QA_SETTINGS_DIR";

bool prepareDirectory(const QString& path) {
    if (QFileInfo(path).isSymLink() || !QDir().mkpath(path))
        return false;
    QTemporaryFile probe(QDir(path).filePath(QStringLiteral(".ketplus-qa-XXXXXX")));
    return probe.open();
}

bool canUseSettingsFile(const QString& path) {
    const QFileInfo info(path);
    if (info.isSymLink())
        return false;
    if (!info.exists())
        return true;
    QFile file(path);
    return info.isFile() && file.open(QIODevice::ReadWrite);
}

QString prepareScope(const QString& path, const QString& organization, const QString& application) {
    const QDir scope(path);
    const auto appDirectory = scope.filePath(organization);
    const auto orgFile = scope.filePath(organization + QStringLiteral(".ini"));
    const auto appFile = QDir(appDirectory).filePath(application + QStringLiteral(".ini"));
    if (!prepareDirectory(path))
        return path;
    if (!prepareDirectory(appDirectory))
        return appDirectory;
    if (!canUseSettingsFile(orgFile))
        return orgFile;
    if (!canUseSettingsFile(appFile))
        return appFile;
    return {};
}

} // namespace

QString configureQaSettingsIsolation(const QString& organization, const QString& application) {
    if (!qEnvironmentVariableIsSet(kDirectoryVariable))
        return {};

    const QString requestedPath = qEnvironmentVariable(kDirectoryVariable);
    if (requestedPath.isEmpty() || !QDir::isAbsolutePath(requestedPath)) {
        return QStringLiteral("KETPLUS_QA_SETTINGS_DIR must be an absolute directory path.");
    }
    if (!QDir().mkpath(requestedPath)) {
        return QStringLiteral("KETPLUS_QA_SETTINGS_DIR: cannot create directory '%1'.")
            .arg(requestedPath);
    }
    const QString canonicalPath = QFileInfo(requestedPath).canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        return QStringLiteral("KETPLUS_QA_SETTINGS_DIR: cannot resolve directory '%1'.")
            .arg(requestedPath);
    }
    const QDir root(canonicalPath);
    const QString userPath = root.filePath(QStringLiteral("user"));
    const QString systemPath = root.filePath(QStringLiteral("system"));
    for (const auto& path : {userPath, systemPath}) {
        const auto unusablePath = prepareScope(path, organization, application);
        if (!unusablePath.isEmpty()) {
            return QStringLiteral("KETPLUS_QA_SETTINGS_DIR: cannot use settings path '%1'.")
                .arg(unusablePath);
        }
    }

    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, userPath);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, systemPath);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    AppUpdater::enableQaSettingsIsolation();
    return {};
}

} // namespace ketplus
