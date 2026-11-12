#include "app/MainWindow.h"
#include "app/ReviewCapture.h"
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
    // The name keeps the settings location of KetPlus CM installs; users see ketPlus.
    QCoreApplication::setApplicationName(QStringLiteral("KetPlus CM"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("ketPlus"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KETPLUS_CM_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("Ketsuite"));
    // Matches core/assets/linux/ketplus.desktop, so Wayland compositors use its name and icon.
    QGuiApplication::setDesktopFileName(QStringLiteral("ketplus"));
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
        ketplus::prepareReviewCapture();
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
    // saves the window once it has laid out, then quits (docs/design-system.md).
    if (const QString capture = qEnvironmentVariable("KETPLUS_CAPTURE"); !capture.isEmpty())
        ketplus::runReviewCapture(theme, window, capture);

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
