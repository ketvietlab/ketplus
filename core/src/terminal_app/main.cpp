#include "terminal_app/TerminalApplication.h"
#include "terminal_app/TerminalWindow.h"
#include "ui/KetFonts.h"

#include "KetProfile.h"
#include "KetStyle.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QIcon>
#include <QPointer>
#include <QVector>

#include <algorithm>
#include <functional>

#ifndef KETPLUS_CM_VERSION
#define KETPLUS_CM_VERSION "0.3.5"
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
    // The application name also names the settings domain, so it stays ASCII; users see the
    // display name (ADR 0029).
    QCoreApplication::setApplicationName(QStringLiteral("KetPlus Terminal"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("KétPlus Terminal"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KETPLUS_CM_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("Ketsuite"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/ketplus-terminal-icon.png")));

    // The kit draws the window: the KÉT fonts (its font-sans and the profile's monospace), the
    // KétPlus metrics, the saved appearance, then the style.
    ketplus::registerKetFonts();
    ket::Profile::setCurrent(ket::Profile::ketPlus());
    ketplus::restoreTerminalAppearance();
    ket::Style::install(app);

    QVector<QPointer<ketplus::TerminalWindow>> windows;
    bool openedWindow = false;
    std::function<void(const LaunchTarget&)> openWindow;
    openWindow = [&app, &windows, &openedWindow, &openWindow](const LaunchTarget& target) {
        auto* window = new ketplus::TerminalWindow(target.workingDirectory, target.commandPath);
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
