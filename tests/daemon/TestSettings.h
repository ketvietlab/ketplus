#pragma once
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

// QSettings() needs an organization name to persist anything on Windows (the registry key is
// built from it). Gives the test process one and keeps its settings in a temporary directory
// instead of the user's. Call it from initTestCase(). Only Windows needs it; elsewhere it does
// nothing so the other platforms keep their behavior.
inline void isolateTestSettings([[maybe_unused]] const QString& applicationName) {
#if defined(Q_OS_WIN)
    static QTemporaryDir directory;
    static bool done = false;
    if (done || !directory.isValid())
        return;
    done = true;
    QCoreApplication::setOrganizationName(QStringLiteral("KetPlusTests"));
    QCoreApplication::setApplicationName(applicationName);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
#endif
}
