#include "app/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QIcon>
#include <QTimer>

#include <cstdio>

#ifndef KETPLUS_CM_VERSION
#define KETPLUS_CM_VERSION "0.1.3"
#endif

int main(int argc, char* argv[]) {
    QElapsedTimer startupTimer;
    startupTimer.start();

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("KetPlus CM"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KETPLUS_CM_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("Ketsuite"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/ketplus-app-icon.png")));

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
