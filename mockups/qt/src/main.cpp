// SPDX-License-Identifier: MPL-2.0
// KetPlus Qt mock. Options make review evidence repeatable:
//   --screen=<id> --state=<state> --dark|--light --zoom=1.25   open one screen
//   --open=model|mode|limits                                    open a composer panel
//   --screenshot=<file>                                         save the window and exit
//   --smoke                                  open every screen; exit non-zero on QML warnings
#include "KetAppearance.h"
#include "MockController.h"

#include <QCommandLineParser>
#include <QDirIterator>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
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
    QGuiApplication application(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("KetPlus Mock"));
    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption screen(QStringLiteral("screen"), QStringLiteral("Open <id>."),
                                    QStringLiteral("id"));
    const QCommandLineOption state(QStringLiteral("state"),
                                   QStringLiteral("Screen state, e.g. running or welcome."),
                                   QStringLiteral("state"));
    const QCommandLineOption open(QStringLiteral("open"),
                                  QStringLiteral("Open a composer panel: model, mode or limits."),
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
    parser.addOptions({screen, state, open, dark, light, zoom, screenshot, smoke});
    parser.process(application);

    previousHandler = qInstallMessageHandler(countQmlProblems);

    // Register KÉT Sans and KÉT Sans Mono before KetTheme reads its font families.
    QDirIterator fonts(QStringLiteral(":/fonts"), {QStringLiteral("*.ttf")}, QDir::Files);
    while (fonts.hasNext())
        if (QFontDatabase::addApplicationFont(fonts.next()) < 0)
            std::fprintf(stderr, "Could not load %s\n", qPrintable(fonts.filePath()));

    if (parser.isSet(dark))
        ket::Appearance::instance().setMode(ket::Appearance::Mode::Dark);
    else if (parser.isSet(light))
        ket::Appearance::instance().setMode(ket::Appearance::Mode::Light);
    ket::Appearance::instance().setZoom(parser.value(zoom).toDouble());

    MockController controller;
    if (parser.isSet(screen))
        controller.initialScreen = parser.value(screen);
    controller.initialState = parser.value(state);
    controller.initialPanel = parser.value(open);
    qmlRegisterSingletonInstance("KetPlusMock.Native", 1, 0, "Mock", &controller);

    QQmlApplicationEngine engine;
    engine.loadFromModule("KetPlusMock", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());

    if (parser.isSet(smoke)) {
        const QStringList ids = window->property("screenIds").toStringList();
        for (const QString& id : ids) {
            QMetaObject::invokeMethod(window, "showScreen", Q_ARG(QVariant, id));
            QTimer::singleShot(150, &application, &QCoreApplication::quit);
            QGuiApplication::exec();
        }
        QMetaObject::invokeMethod(window, "openEveryOverlay");
        QTimer::singleShot(300, &application, &QCoreApplication::quit);
        QGuiApplication::exec();
        std::fprintf(stderr, "%lld screens, %d QML problems\n", qlonglong(ids.size()), qmlProblems);
        return ids.isEmpty() || qmlProblems > 0 ? 1 : 0;
    }
    if (const QString file = parser.value(screenshot); !file.isEmpty()) {
        QTimer::singleShot(700, window, [window, file] {
            QGuiApplication::exit(window->grabWindow().save(file) ? 0 : 1);
        });
    }
    return QGuiApplication::exec();
}
