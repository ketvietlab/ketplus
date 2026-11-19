#pragma once
#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

// QSettings() needs an organization name to persist anything on Windows (the registry key is
// built from it). Gives the test process one and keeps its settings in a temporary directory
// instead of the user's, on every platform. Call it from initTestCase().
inline void isolateTestSettings(const QString& applicationName) {
    static QTemporaryDir directory;
    static bool done = false;
    if (done || !directory.isValid())
        return;
    done = true;
    QCoreApplication::setOrganizationName(QStringLiteral("KetPlusTests"));
    QCoreApplication::setApplicationName(applicationName);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
}
