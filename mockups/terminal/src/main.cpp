// SPDX-License-Identifier: MPL-2.0
// KetPlus Terminal mock. Options make review evidence repeatable:
//   --dark|--light --zoom=1.25      theme and interface zoom
//   --cwd=<dir>                     the first tab's folder (default: home)
//   --demo                          run sample output in the first tab and open a second tab
//   --find=<text>                   open Find in terminal with <text>
//   --screenshot=<file>             save the window and exit
#include "KetAppearance.h"
#include "KetStyle.h"
#include "TerminalMockWindow.h"
#include "terminal/TerminalView.h"
#include "terminal_app/TerminalTab.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFontDatabase>
#include <QIcon>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>

namespace {

// The demo script lives in resources; the shell needs it on disk.
QString extractDemoScript() {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString path = QDir(directory).filePath(QStringLiteral("ketplus-terminal-mock-demo.sh"));
    QFile::remove(path);
    if (!QFile::copy(QStringLiteral(":/demo/demo.sh"), path))
        return {};
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("KetPlus Terminal Mock"));
    QApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/ketplus-terminal.svg")));

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption dark(QStringLiteral("dark"), QStringLiteral("Start in dark mode."));
    const QCommandLineOption light(QStringLiteral("light"), QStringLiteral("Start in light mode."));
    const QCommandLineOption zoom(QStringLiteral("zoom"), QStringLiteral("Interface zoom."),
                                  QStringLiteral("factor"), QStringLiteral("1"));
    const QCommandLineOption cwd(QStringLiteral("cwd"), QStringLiteral("The first tab's folder."),
                                 QStringLiteral("dir"));
    const QCommandLineOption demo(QStringLiteral("demo"),
                                  QStringLiteral("Run sample output and open a second tab."));
    const QCommandLineOption find(QStringLiteral("find"),
                                  QStringLiteral("Open Find in terminal with <text>."),
                                  QStringLiteral("text"));
    const QCommandLineOption screenshot(QStringLiteral("screenshot"),
                                        QStringLiteral("Save the window to <file> and exit."),
                                        QStringLiteral("file"));
    parser.addOptions({dark, light, zoom, cwd, demo, find, screenshot});
    parser.process(application);

    // Register KÉT Sans and KÉT Sans Mono before the kit reads its font families.
    QDirIterator fonts(QStringLiteral(":/fonts"), {QStringLiteral("*.ttf")}, QDir::Files);
    while (fonts.hasNext())
        if (QFontDatabase::addApplicationFont(fonts.next()) < 0)
            std::fprintf(stderr, "Could not load %s\n", qPrintable(fonts.filePath()));

    if (parser.isSet(dark))
        ket::Appearance::instance().setMode(ket::Appearance::Mode::Dark);
    else if (parser.isSet(light))
        ket::Appearance::instance().setMode(ket::Appearance::Mode::Light);
    ket::Appearance::instance().setZoom(parser.value(zoom).toDouble());
    ket::Style::install(application);

    const auto openWindow = [](const QString& directory) {
        auto* window = new TerminalMockWindow(directory);
        QObject::connect(window, &TerminalMockWindow::newWindowRequested, window,
                         [](const QString& next) {
                             auto* another = new TerminalMockWindow(next);
                             another->addTab(next);
                             another->show();
                         });
        return window;
    };
    TerminalMockWindow* window = openWindow(parser.value(cwd));
    const QString first = parser.value(cwd).isEmpty() ? QDir::homePath() : parser.value(cwd);
    if (parser.isSet(demo)) {
        window->addTab(QDir::homePath());
        window->addTab(first, extractDemoScript());
    } else {
        window->addTab(first);
    }
    window->show();

    if (const QString text = parser.value(find); !text.isEmpty()) {
        QTimer::singleShot(3000, window, [window, text] {
            if (ketplus::TerminalTab* tab = window->currentTab()) {
                tab->view()->openSearch();
                if (auto* edit =
                        tab->view()->findChild<QLineEdit*>(QStringLiteral("terminalSearchEdit")))
                    edit->setText(text);
            }
        });
    }
    if (const QString file = parser.value(screenshot); !file.isEmpty()) {
        QTimer::singleShot(4000, window, [window, file] {
            QApplication::exit(window->grab().save(file) ? 0 : 1);
        });
    }
    return QApplication::exec();
}
