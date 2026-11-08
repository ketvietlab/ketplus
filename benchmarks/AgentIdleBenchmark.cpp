// Native, synthetic #126 workload. No provider, daemon, real journal or polling
// probe is involved during the measured interval. See run-agent-idle.py.
#include "agent/AgentWorkspaceView.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <sys/resource.h>

namespace {
double cpuSeconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1'000'000.0 + usage.ru_stime.tv_sec +
           usage.ru_stime.tv_usec / 1'000'000.0;
}

void report(QJsonObject row) {
    const auto bytes = QJsonDocument(row).toJson(QJsonDocument::Compact);
    std::printf("%s\n", bytes.constData());
    std::fflush(stdout);
}

QString stateName() {
    return qApp->applicationState() == Qt::ApplicationActive ? "active" : "inactive";
}

QQuickItem* findItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name)
        return root;
    for (auto* child : root->childItems())
        if (auto* found = findItem(child, name))
            return found;
    return nullptr;
}
} // namespace

int main(int argc, char** argv) {
    qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
    QApplication app(argc, argv);
    app.setOrganizationName("KetPlusBenchmark");
    app.setApplicationName("Agent idle CPU");
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir settings;
    if (!settings.isValid())
        return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({{"work", "running or idle", "state", "running"},
                       {"visibility", "active, inactive or hidden", "state", "active"},
                       {"seconds", "Measured interval (10–60 seconds)", "seconds", "12"},
                       {"capture", "Optional native PNG before measurement", "path"}});
    parser.process(app);
    const auto work = parser.value("work");
    const auto visibility = parser.value("visibility");
    bool validSeconds = false;
    const int seconds = parser.value("seconds").toInt(&validSeconds);
    if ((work != "running" && work != "idle") ||
        (visibility != "active" && visibility != "inactive" && visibility != "hidden") ||
        !validSeconds || seconds < 10 || seconds > 60) {
        report({{"event", "error"}, {"reason", "Invalid workload, visibility or duration"}});
        return 2;
    }
    ketplus::ThemeManager theme(app);
    ketplus::AgentWorkspaceView view;
    view.setWindowTitle("KétPlus idle CPU fixture");
    view.resize(1440, 900);
    view.setAgentAvailable(true);
    view.setActivePath(settings.path());
    QVariantList conversations;
    for (int i = 0; i < 3; ++i)
        conversations.append(QVariantMap{{"id", QString::number(i)},
                                         {"number", QString::number(100 + i)},
                                         {"title", QString("Synthetic task %1").arg(i + 1)},
                                         {"active", i == 0},
                                         {"running", work == "running"},
                                         {"started", true},
                                         {"status", work == "running" ? "Working" : "Done"}});
    view.setConversations(conversations);
    view.resetMessages({QVariantMap{{"kind", "user"},
                                    {"messageKey", "prompt"},
                                    {"body", "Inspect the repository and run the focused checks."}},
                        QVariantMap{{"kind", "assistant"},
                                    {"messageKey", "response"},
                                    {"body", "The patch is ready. Checking the remaining tasks."}}},
                       "idle-cpu-fixture");
    view.setSessionState("Idle CPU qualification", work == "running" ? "Working" : "Ready",
                         work == "running");
    view.show();
    view.raise();
    view.activateWindow();
    view.quickView()->requestActivate();
    const auto expected = [&] {
        return visibility == "hidden" ? !view.isVisible()
                                      : view.isVisible() && stateName() == visibility;
    };
    if (visibility == "hidden")
        QTimer::singleShot(1000, &view, &QWidget::hide);
    report({{"event", "ready"},
            {"pid", qint64(app.applicationPid())},
            {"work", work},
            {"expectedVisibility", visibility}});

    // This readiness timer stops BEFORE measurement. It verifies the actual OS
    // application state rather than simulating inactivity in QML.
    QTimer readiness;
    readiness.setInterval(100);
    QTimer readinessDeadline;
    readinessDeadline.setSingleShot(true);
    QElapsedTimer stable;
    bool measuring = false;
    bool stateChanged = false;
    bool fixtureValid = false;
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &app, [&] {
        if (measuring) {
            if (!expected())
                stateChanged = true;
            report({{"event", "state-change"}, {"state", stateName()}});
        }
    });
    QElapsedTimer wall;
    int frames = 0;
    double cpuStart = 0;
    QObject::connect(view.quickView(), &QQuickWindow::frameSwapped, &app, [&] {
        if (measuring)
            ++frames;
    });
    QObject::connect(&readiness, &QTimer::timeout, &app, [&] {
        if (!expected()) {
            stable.invalidate();
            return;
        }
        if (!stable.isValid())
            stable.start();
        if (stable.elapsed() < 3000)
            return;
        const auto capture = parser.value("capture");
        if (!capture.isEmpty() && !view.property("captured").toBool()) {
            if (!view.quickView()->grabWindow().save(capture)) {
                report({{"event", "error"}, {"reason", "Capture failed"}});
                app.exit(2);
                return;
            }
            view.setProperty("captured", true);
            stable.restart();
            return;
        }
        readiness.stop();
        readinessDeadline.stop();
        auto* timer =
            view.quickView()->rootObject()->findChild<QObject*>("transcriptMeasurementTimer");
        int visibleRows = 0;
        int sidebarClockHolders = 0;
        for (int i = 0; i < 3; ++i) {
            auto* row =
                findItem(view.quickView()->rootObject(), QString("conversationRow_%1").arg(i));
            visibleRows += row && row->isVisible();
            sidebarClockHolders += row && row->property("holdsClock").toBool();
        }
        auto* indicator = findItem(view.quickView()->rootObject(), "workingAnimationIcon");
        const bool clockHeld = indicator && indicator->property("holdsClock").toBool();
        const bool metal =
            view.quickView()->rendererInterface()->graphicsApi() == QSGRendererInterface::Metal;
        fixtureValid = app.platformName() == "cocoa" && metal &&
                       qFuzzyCompare(view.quickView()->devicePixelRatio(), 2.0) &&
                       view.size() == QSize(1440, 900) && visibleRows == 3 && timer &&
                       !timer->property("running").toBool() &&
                       clockHeld == (work == "running" && visibility != "hidden" &&
                                     view.workingMotionEnabled());
        report({{"event", "begin"},
                {"pid", qint64(app.applicationPid())},
                {"platform", app.platformName()},
                {"renderer", metal ? "metal" : "other"},
                {"fixtureValid", fixtureValid},
                {"state", stateName()},
                {"visible", view.isVisible()},
                {"dpr", view.quickView()->devicePixelRatio()},
                {"width", view.width()},
                {"height", view.height()},
                {"visibleSidebarRows", visibleRows},
                {"sidebarClockHolders", sidebarClockHolders},
                {"transcriptClockHeld", clockHeld},
                {"measurementTimerRunning", timer ? timer->property("running").toBool() : true}});
        measuring = true;
        cpuStart = cpuSeconds();
        wall.start();
        QTimer::singleShot(seconds * 1000, Qt::PreciseTimer, &app, [&] {
            const auto elapsed = wall.nsecsElapsed() / 1e9;
            const auto cpu = (cpuSeconds() - cpuStart) / elapsed * 100;
            measuring = false;
            const bool valid = fixtureValid && expected() && !stateChanged;
            report({{"event", "result"},
                    {"valid", valid},
                    {"work", work},
                    {"visibility", visibility},
                    {"seconds", elapsed},
                    {"cpuPercent", cpu},
                    {"frames", frames},
                    {"framesPerSecond", frames / elapsed}});
            app.exit(valid ? 0 : 2);
        });
    });
    readiness.start();
    QObject::connect(&readinessDeadline, &QTimer::timeout, &app, [&] {
        report({{"event", "error"}, {"reason", "Timed out waiting for stable window state"}});
        app.exit(2);
    });
    readinessDeadline.start(60000);
    return app.exec();
}
