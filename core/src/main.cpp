#include "app/MainWindow.h"
#include "ui/KetFonts.h"
#include "ui/Theme.h"
#include <QSettings>
#include <QTemporaryDir>
#include <memory>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QIcon>
#include <QTimer>

#include <cstdio>

#ifndef KETPLUS_CM_VERSION
#define KETPLUS_CM_VERSION "0.3.5"
#endif

int main(int argc, char* argv[]) {
    QElapsedTimer startupTimer;
    startupTimer.start();

    QApplication app(argc, argv);
    // The name keeps the settings location of KetPlus CM installs; users see KétPlus.
    QCoreApplication::setApplicationName(QStringLiteral("KetPlus CM"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("KétPlus"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KETPLUS_CM_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("Ketsuite"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/ketplus-app-icon.png")));

    // Capture/benchmark runs use a clean temporary preference store and cannot
    // overwrite the installed application's session, theme or window state.
    std::unique_ptr<QTemporaryDir> captureSettings;
    if (qEnvironmentVariableIsSet("KETPLUS_CAPTURE") ||
        qEnvironmentVariableIsSet("KETPLUS_BENCHMARK_STARTUP")) {
        captureSettings = std::make_unique<QTemporaryDir>();
        if (!captureSettings->isValid())
            return 1;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, captureSettings->path());
    }

    ketplus::registerKetFonts();
    ketplus::ThemeManager theme(app);
    ketplus::MainWindow window(theme);
    window.show();

    const auto arguments = QCoreApplication::arguments();
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QFileInfo file(arguments.at(index));
        if (file.isFile()) {
            window.openFile(file.absoluteFilePath());
        } else if (file.isDir()) {
            window.openFolder(file.absoluteFilePath());
        }
    }

    // Review evidence without a screen: with QT_QPA_PLATFORM=offscreen, KETPLUS_CAPTURE=<file>
    // saves the window once it has laid out, then quits. KETPLUS_CAPTURE_THEME=light|dark
    // previews that theme without saving it.
    if (const QString capture = qEnvironmentVariable("KETPLUS_CAPTURE"); !capture.isEmpty()) {
        const QString captureTheme = qEnvironmentVariable("KETPLUS_CAPTURE_THEME");
        if (captureTheme == QLatin1String("light")) {
            theme.previewMode(ketplus::ThemeManager::Mode::Light);
        } else if (captureTheme == QLatin1String("dark")) {
            theme.previewMode(ketplus::ThemeManager::Mode::Dark);
        }
        QTimer::singleShot(700, &app, [&app, &window, capture] {
            const bool saved = window.grab().save(capture);
            std::fprintf(stderr, "KETPLUS_CAPTURE %s: %s\n", saved ? "saved" : "failed",
                         qPrintable(capture));
            app.exit(saved ? 0 : 1);
        });
    }

    if (qEnvironmentVariableIsSet("KETPLUS_BENCHMARK_STARTUP")) {
        QTimer::singleShot(0, &app, [&app, &startupTimer] {
            const double elapsedMilliseconds =
                static_cast<double>(startupTimer.nsecsElapsed()) / 1'000'000.0;
            std::fprintf(stderr, "KETPLUS_STARTUP_MS=%.3f\n", elapsedMilliseconds);
            app.quit();
        });
    }

    return QApplication::exec();
}
