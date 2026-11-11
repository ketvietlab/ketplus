#include "app/ApplicationAttributes.h"
#include "app/MainWindow.h"
#include "app/RepositoryRouter.h"
#include "ui/InterfaceFont.h"
#include "ui/Theme.h"
#include "update/AppUpdater.h"

#include "KetPlusProductVersion.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
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
    // The name keeps the settings location of existing installs; users see KétPlus Pro.
    QCoreApplication::setApplicationName(QStringLiteral("KetPlus"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("KétPlus Pro"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KETPLUS_PRODUCT_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("KetPlus"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/ketplus-app-icon.png")));

    ketplus::ThemeManager theme(app);
    ketplus::InterfaceUiFont::apply(app, theme.interfaceTextPixels());
    QObject::connect(&theme, &ketplus::ThemeManager::themeChanged, &app, [&app, &theme] {
        ketplus::InterfaceUiFont::apply(app, theme.interfaceTextPixels());
    });
    ketplus::RepositoryRouter repositories;
    std::function<ketplus::MainWindow*()> createWindow;
    createWindow = [&app, &theme, &repositories, &createWindow] {
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
