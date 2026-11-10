// SPDX-License-Identifier: MPL-2.0
// KetPlus Qt mock. Options make review evidence repeatable:
//   --screen=<id> --state=<state> --dark|--light --zoom=1.25   open one screen
//   --open=model|mode|limits|reviewer|composer|images|mermaid   a composer panel, the image viewer
//   or diagrams; plan presses the Plan ring, atlas-toggle presses Atlas twice
//   --scroll=0.5                        scroll the transcript, the Connections intro or a team
//   (0 top, 1 end)
//   --screenshot=<file>                                         save the window and exit
//   --smoke                                  open every screen; exit non-zero on QML warnings
//   --edition=lite --chrome=windows|linux       KétPlus instead of Pro, with that platform's chrome
//   --display=secondary|<index>      open on another display, leaving the main one free
//   --keys                     with --smoke and --edition=lite: drive the menu bar by keyboard
#include "KetAppearance.h"
#include "KetProfile.h"
#include "MockController.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDirIterator>
#include <QFontDatabase>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QScreen>
#include <QTest>
#include <QTimer>
#include <QtQml/qqmlextensionplugin.h>

#include <cstdio>

Q_IMPORT_QML_PLUGIN(KetPlugin)

namespace {

int qmlProblems = 0;
QtMessageHandler previousHandler = nullptr;

void countQmlProblems(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (type != QtDebugMsg && type != QtInfoMsg &&
        (message.contains(QLatin1String(".qml")) || message.contains(QLatin1String("QML"))))
        ++qmlProblems;
    previousHandler(type, context, message);
}

} // namespace

int main(int argc, char* argv[]) {
    // A QApplication, not QGuiApplication: the image viewer prints through the system print
    // dialog, which is a widget.
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("KetPlus Mock"));
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption screen(QStringLiteral("screen"), QStringLiteral("Open <id>."),
                                    QStringLiteral("id"));
    const QCommandLineOption state(QStringLiteral("state"),
                                   QStringLiteral("Screen state, e.g. running or welcome."),
                                   QStringLiteral("state"));
    const QCommandLineOption open(
        QStringLiteral("open"),
        QStringLiteral(
            "Open a panel: model, mode, limits, reviewer, picker, toast, or activity, device "
            "device-focus, pair, linux and signin on connections."),
        QStringLiteral("panel"));
    const QCommandLineOption dark(QStringLiteral("dark"), QStringLiteral("Start in dark mode."));
    const QCommandLineOption light(QStringLiteral("light"), QStringLiteral("Start in light mode."));
    const QCommandLineOption zoom(QStringLiteral("zoom"), QStringLiteral("Interface zoom."),
                                  QStringLiteral("factor"), QStringLiteral("1"));
    const QCommandLineOption screenshot(QStringLiteral("screenshot"),
                                        QStringLiteral("Save the window to <file> and exit."),
                                        QStringLiteral("file"));
    const QCommandLineOption smoke(QStringLiteral("smoke"),
                                   QStringLiteral("Open every screen; fail on QML warnings."));
    const QCommandLineOption scroll(QStringLiteral("scroll"),
                                    QStringLiteral("Scroll the transcript, 0 (top) to 1 (end)."),
                                    QStringLiteral("fraction"));
    const QCommandLineOption edition(QStringLiteral("edition"),
                                     QStringLiteral("pro (KétPlus Pro) or lite (KétPlus)."),
                                     QStringLiteral("edition"), QStringLiteral("pro"));
    const QCommandLineOption chrome(QStringLiteral("chrome"),
                                    QStringLiteral("Lite window chrome: macos, windows or linux."),
                                    QStringLiteral("platform"), QStringLiteral("macos"));
    const QCommandLineOption display(
        QStringLiteral("display"),
        QStringLiteral("Display to open on: secondary (the first that is not the main one) or "
                       "an index."),
        QStringLiteral("display"));
    const QCommandLineOption keys(
        QStringLiteral("keys"),
        QStringLiteral("With --smoke: choose a menu item by keyboard. It activates the window."));
    parser.addOptions({screen, state, open, dark, light, zoom, scroll, screenshot, smoke, edition,
                       chrome, display, keys});
    parser.process(application);

    previousHandler = qInstallMessageHandler(countQmlProblems);

    // Register KÉT Sans and KÉT Sans Mono before KetTheme reads its font families.
    QDirIterator fonts(QStringLiteral(":/fonts"), {QStringLiteral("*.ttf")}, QDir::Files);
    while (fonts.hasNext())
        if (QFontDatabase::addApplicationFont(fonts.next()) < 0)
            std::fprintf(stderr, "Could not load %s\n", qPrintable(fonts.filePath()));

    // Mock overrides of the KetPlus profile. Monospace text (code, diffs, inline code,
    // conversation numbers) reads at 14 px.
    ket::Profile profile = ket::Profile::ketPlus();
    profile.codePixels = 14;
    profile.conversationNumberPixels = 14;
    // The composer's context ring reads lighter at 15 px with a 2 px stroke (kit: 19 and 3).
    profile.usageRingSize = 15;
    profile.usageRingStroke = 2;
    // Sidebar text in Medium: conversation titles (selected or not) and their status line.
    profile.conversationTitleWeight = 500;
    profile.conversationSelectedTitleWeight = 500;
    profile.conversationMetaWeight = 500;
    profile.navItemActiveWeight = 500;
    ket::Profile::setCurrent(profile);

    if (parser.isSet(dark))
        ket::Appearance::instance().setMode(ket::Appearance::Mode::Dark);
    else if (parser.isSet(light))
        ket::Appearance::instance().setMode(ket::Appearance::Mode::Light);
    ket::Appearance::instance().setZoom(parser.value(zoom).toDouble());

    MockController controller;
    controller.edition = parser.value(edition) == QLatin1String("lite") ? QStringLiteral("lite")
                                                                        : QStringLiteral("pro");
    if (const QString platform = parser.value(chrome);
        platform == QLatin1String("windows") || platform == QLatin1String("linux"))
        controller.chrome = platform;
    if (controller.edition == QLatin1String("lite"))
        controller.initialScreen = QStringLiteral("editor");
    if (parser.isSet(screen))
        controller.initialScreen = parser.value(screen);
    controller.initialState = parser.value(state);
    controller.initialPanel = parser.value(open);
    if (parser.isSet(scroll))
        controller.initialScroll = parser.value(scroll).toDouble();
    qmlRegisterSingletonInstance("KetPlusMock.Native", 1, 0, "Mock", &controller);

    QQmlApplicationEngine engine;
    engine.loadFromModule("KetPlusMock",
                          controller.edition == QLatin1String("lite") ? "LiteMain" : "Main");
    if (engine.rootObjects().isEmpty())
        return 1;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    // Fail reproducible captures if a child initialization resets the requested plan state.
    if (controller.edition != QLatin1String("lite") &&
        controller.initialScreen == QLatin1String("workspace") &&
        controller.initialState.startsWith(QLatin1String("plan-"))) {
        const QString expected = controller.initialState.mid(5);
        if (window->property("planningState").toString() != expected) {
            std::fprintf(stderr, "Planning startup state does not match --state\n");
            return 1;
        }
    }
    if (parser.isSet(display)) {
        const QList<QScreen*> screens = QGuiApplication::screens();
        QScreen* target = nullptr;
        if (parser.value(display) == QLatin1String("secondary")) {
            for (QScreen* candidate : screens)
                if (candidate != QGuiApplication::primaryScreen() && !target)
                    target = candidate;
        } else {
            bool ok = false;
            const int index = parser.value(display).toInt(&ok);
            if (ok && index >= 0 && index < screens.size())
                target = screens.at(index);
        }
        if (target) {
            window->setScreen(target);
            window->setPosition(target->availableGeometry().topLeft() + QPoint(40, 40));
            std::fprintf(stderr, "Opening on %s\n", qPrintable(target->name()));
        } else {
            std::fprintf(stderr, "No display %s; opening on the main one\n",
                         qPrintable(parser.value(display)));
        }
    }

    if (parser.isSet(smoke)) {
        const QStringList ids = window->property("screenIds").toStringList();
        for (const QString& id : ids) {
            QMetaObject::invokeMethod(window, "showScreen", Q_ARG(QVariant, id));
            QTimer::singleShot(150, &application, &QCoreApplication::quit);
            QApplication::exec();
        }
        QMetaObject::invokeMethod(window, "openEveryOverlay");
        QTimer::singleShot(300, &application, &QCoreApplication::quit);
        QApplication::exec();
        if (controller.edition == QLatin1String("lite") && parser.isSet(keys)) {
            // The Windows menu bar without a pointer: F10 focuses File, Right ×4 reaches
            // View, Down opens it, Down Down Enter chooses Search in Files. Keys reach Qt
            // Quick only in the active window; when another app keeps the focus, say so and
            // skip rather than report the mock.
            QMetaObject::invokeMethod(window, "showScreen",
                                      Q_ARG(QVariant, QStringLiteral("explorer/windows")));
            window->requestActivate();
            if (!QTest::qWaitForWindowActive(window)) {
                std::fprintf(stderr, "Keyboard check skipped: the window is not active\n");
            } else {
                QTest::qWait(150);
                QTest::keyClick(window, Qt::Key_F10);
                for (int i = 0; i < 4; ++i)
                    QTest::keyClick(window, Qt::Key_Right);
                for (const Qt::Key key :
                     {Qt::Key_Down, Qt::Key_Down, Qt::Key_Down, Qt::Key_Return}) {
                    QTest::qWait(50);
                    QTest::keyClick(window, key);
                }
                const bool opened = QTest::qWaitFor(
                    [window] {
                        return window->property("screen").toString() == QLatin1String("search");
                    },
                    1000);
                if (!opened) {
                    std::fprintf(stderr,
                                 "QML keyboard check: View > Search in Files did not open it\n");
                    ++qmlProblems;
                } else {
                    std::fprintf(stderr, "Keyboard check: View > Search in Files opened Search\n");
                }
            }
        }
        std::fprintf(stderr, "%lld screens, %d QML problems\n", qlonglong(ids.size()), qmlProblems);
        return ids.isEmpty() || qmlProblems > 0 ? 1 : 0;
    }
    if (const QString file = parser.value(screenshot); !file.isEmpty()) {
        QTimer::singleShot(700, window, [window, file] {
            QApplication::exit(window->grabWindow().save(file) ? 0 : 1);
        });
    }
    return QApplication::exec();
}
