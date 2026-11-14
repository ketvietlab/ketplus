#include "app/ApplicationAttributes.h"
#include "app/MainWindow.h"
#include "app/RepositoryRouter.h"
#include "remote/LinuxMachines.h"
#include "ui/InterfaceFont.h"
#include "ui/Theme.h"
#include "update/AppUpdater.h"

#include "KetPlusProductVersion.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QtWebView/QtWebView>

#include <cstdio>
#include <functional>

int main(int argc, char* argv[]) {
    QElapsedTimer startupTimer;
    startupTimer.start();

#ifdef Q_OS_MACOS
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    QtWebView::initialize();
    ketplus::applyApplicationAttributes();
    QApplication app(argc, argv);
#ifdef Q_OS_LINUX
    // Installed-package diagnostic: inspect a real child, never the app's own
    // environment, and do not print unrelated variables (tokens, credentials).
    if (app.arguments().contains(QStringLiteral("--check-child-environment"))) {
        QProcess child;
        child.start(QStringLiteral("/usr/bin/env"), {QStringLiteral("-0")});
        if (!child.waitForFinished(5000) || child.exitStatus() != QProcess::NormalExit ||
            child.exitCode() != 0) {
            std::fprintf(stderr, "Child environment probe failed\n");
            return 1;
        }
        QJsonObject environment;
        for (const auto& entry : child.readAllStandardOutput().split('\0')) {
            const auto separator = entry.indexOf('=');
            if (separator < 0)
                continue;
            const auto name = entry.left(separator);
            if (name == "LD_LIBRARY_PATH" || name == "QT_PLUGIN_PATH" || name.startsWith("QML") ||
                name.startsWith("QTWEBENGINE")) {
                environment.insert(QString::fromUtf8(name),
                                   QString::fromUtf8(entry.mid(separator + 1)));
            }
        }
        std::puts(QJsonDocument(environment).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
#endif
    // The name keeps the settings location of existing installs; users see ketPlus Pro.
    QCoreApplication::setApplicationName(QStringLiteral("KetPlus"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("ketPlus Pro"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KETPLUS_PRODUCT_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("KetPlus"));
    // Matches packaging/linux/ketplus-pro.desktop, so Wayland compositors use its name and icon.
    QGuiApplication::setDesktopFileName(QStringLiteral("ketplus-pro"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/ketplus-app-icon.png")));

    ketplus::ThemeManager theme(app);
    ketplus::InterfaceUiFont::apply(app, theme.interfaceTextPixels());
    QObject::connect(&theme, &ketplus::ThemeManager::themeChanged, &app, [&app, &theme] {
        ketplus::InterfaceUiFont::apply(app, theme.interfaceTextPixels());
    });
    ketplus::RepositoryRouter repositories;
    // The app's one set of Linux machines (ADR 0119): every window shows it, and it connects
    // once, at launch, so each window's picker lists the machines before Connections opens.
    ketplus::LinuxMachines linuxMachines;
    linuxMachines.attachSshBackend();
    linuxMachines.attachRelayBackend();
    linuxMachines.start();
    std::function<ketplus::MainWindow*()> createWindow;
    createWindow = [&app, &theme, &repositories, &createWindow, &linuxMachines] {
        auto* window = new ketplus::MainWindow(theme);
        window->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(window, &ketplus::MainWindow::newWindowRequested, &app,
                         [&createWindow] { createWindow()->show(); });
        QObject::connect(window, &ketplus::MainWindow::repositoryOpened, &repositories,
                         [&repositories, window](const QString& path) {
                             repositories.registerWindowLater(path, window);
                         });
        QObject::connect(window, &ketplus::MainWindow::repositoryRequested, &repositories,
                         &ketplus::RepositoryRouter::activate);
        QObject::connect(window, &ketplus::MainWindow::repositoryFileRequested, &repositories,
                         &ketplus::RepositoryRouter::openFile);
        QObject::connect(window, &ketplus::MainWindow::repositoryFileDiffRequested, &repositories,
                         &ketplus::RepositoryRouter::openFileDiff);
        window->useLinuxMachines(&linuxMachines);
        return window;
    };
    QObject::connect(&repositories, &ketplus::RepositoryRouter::worktreeRequested, &app,
                     [](QWidget* window, const QString& path) {
                         if (auto* target = qobject_cast<ketplus::MainWindow*>(window))
                             target->switchToWorktree(path);
                     });
    QObject::connect(&repositories, &ketplus::RepositoryRouter::openRequested, &app,
                     [&createWindow](const QString& path) {
                         auto* target = createWindow();
                         target->openFolder(path);
                         target->show();
                         target->raise();
                         target->activateWindow();
                     });
    QObject::connect(&repositories, &ketplus::RepositoryRouter::fileRequested, &app,
                     [](QWidget* window, const QString& filePath) {
                         if (auto* target = qobject_cast<ketplus::MainWindow*>(window))
                             target->openFile(filePath);
                     });
    QObject::connect(&repositories, &ketplus::RepositoryRouter::fileDiffRequested, &app,
                     [](QWidget* window, const QString& filePath) {
                         if (auto* target = qobject_cast<ketplus::MainWindow*>(window))
                             target->openFileDiff(filePath);
                     });
    QObject::connect(&repositories, &ketplus::RepositoryRouter::openFileRequested, &app,
                     [&createWindow](const QString& repositoryPath, const QString& filePath) {
                         auto* target = createWindow();
                         target->openFolder(repositoryPath);
                         target->openFile(filePath);
                         target->show();
                         target->raise();
                         target->activateWindow();
                     });
    QObject::connect(&repositories, &ketplus::RepositoryRouter::openFileDiffRequested, &app,
                     [&createWindow](const QString& repositoryPath, const QString& filePath) {
                         auto* target = createWindow();
                         target->openFolder(repositoryPath);
                         target->openFileDiff(filePath);
                         target->show();
                         target->raise();
                         target->activateWindow();
                     });
    QObject::connect(&repositories, &ketplus::RepositoryRouter::activationFailed, &app,
                     [](const QString& message) {
                         QMessageBox::warning(QApplication::activeWindow(),
                                              QStringLiteral("KetPlus Spotlight"), message);
                     });

    auto* window = createWindow();
    window->show();

    const auto arguments = QCoreApplication::arguments();
    bool openedArgument = false;
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QFileInfo file(arguments.at(index));
        if (file.isFile()) {
            window->openFile(file.absoluteFilePath());
            openedArgument = true;
        } else if (file.isDir()) {
            window->openFolder(file.absoluteFilePath());
            openedArgument = true;
        }
    }
    if (!openedArgument) {
        window->restoreLastWorkspace();
    }
    // Scheduled update checks start once a window is up, so they never delay launch.
    QTimer::singleShot(0, &app, [] { ketplus::AppUpdater::instance().start(); });

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
