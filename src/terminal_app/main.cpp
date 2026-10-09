#include "terminal_app/TerminalApplication.h"
#include "terminal_app/TerminalWindow.h"
#include "ui/Theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QPointer>
#include <QVector>

#include <algorithm>
#include <functional>

#ifndef KETPLUS_CM_VERSION
#define KETPLUS_CM_VERSION "0.3.4"
#endif

namespace {

struct LaunchTarget final {
    QString workingDirectory;
    QString commandPath;
};

LaunchTarget targetForPath(const QString& path) {
    const QFileInfo info(path);
    if (info.isDir()) {
        return {info.absoluteFilePath(), {}};
    }
    if (info.isFile()) {
        return {info.absolutePath(), info.absoluteFilePath()};
    }
    return {QDir::homePath(), {}};
}

} // namespace

int main(int argc, char* argv[]) {
    ketplus::TerminalApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("KetPlus Terminal"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KETPLUS_CM_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("Ketsuite"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/ketplus-terminal-icon.png")));

    ketplus::ThemeManager theme(app);
    QVector<QPointer<ketplus::TerminalWindow>> windows;
    bool openedWindow = false;
    std::function<void(const LaunchTarget&)> openWindow;
    openWindow = [&app, &theme, &windows, &openedWindow, &openWindow](const LaunchTarget& target) {
        auto* window =
            new ketplus::TerminalWindow(theme, target.workingDirectory, target.commandPath);
        openedWindow = true;
        windows.append(window);
        QObject::connect(window, &QObject::destroyed, &app, [&windows] {
            windows.erase(std::remove_if(windows.begin(), windows.end(),
                                         [](const auto& item) { return item.isNull(); }),
                          windows.end());
        });
        QObject::connect(
            window, &ketplus::TerminalWindow::newWindowRequested, &app,
            [&openWindow](const QString& workingDirectory) { openWindow({workingDirectory, {}}); });
        window->show();
        window->raise();
        window->activateWindow();
    };

    app.setOpenPathHandler([&openWindow](const QString& path) { openWindow(targetForPath(path)); });

    const QStringList arguments = QCoreApplication::arguments();
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QFileInfo info(arguments.at(index));
        if (info.exists()) {
            openWindow(targetForPath(info.absoluteFilePath()));
        }
    }
    if (!openedWindow) {
        openWindow({QDir::homePath(), {}});
    }

    return QApplication::exec();
}
