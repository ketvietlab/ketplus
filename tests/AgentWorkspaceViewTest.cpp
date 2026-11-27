#include "AgentWorkspaceViewTest.h"

#include "KetAppearance.h"
#include <QApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

void AgentWorkspaceViewTest::init() {
    ket::Appearance::instance().setMode(ket::Appearance::Mode::Dark);
    ket::Appearance::instance().setZoom(1.0);
}

int main(int argc, char** argv) {
    // Transcript QA must never read or overwrite the installed application's preferences.
    QTemporaryDir settings;
    if (!settings.isValid())
        return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("KetPlusQA"));
    QCoreApplication::setApplicationName(QStringLiteral("TranscriptCards"));
    AgentWorkspaceViewTest test;
    return QTest::qExec(&test, argc, argv);
}
