// Native, synthetic #126 workload. No provider, daemon, real journal or polling
// probe is involved during the measured interval. See run-agent-idle.py.
#include "KetAppearance.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include "ui/Theme.h"

#include "AgentTranscriptFixture.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>
#include <cstdio>
#include <functional>
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

// Reports window changes that would make a sample describe a different fixture,
// without adding a polling timer to the measured interval.
class FixtureWatcher final : public QObject {
  public:
    explicit FixtureWatcher(std::function<void(const char*)> changed)
        : changed_(std::move(changed)) {}

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
        case QEvent::Resize:
            changed_("resize");
            break;
        case QEvent::WindowStateChange:
            changed_("window-state");
            break;
        case QEvent::DevicePixelRatioChange:
            changed_("device-pixel-ratio");
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

  private:
    std::function<void(const char*)> changed_;
};
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
    parser.addOptions(
        {{"work", "running or idle", "state", "running"},
         {"visibility", "active, inactive, hidden or minimized", "state", "active"},
         {"seconds", "Measured interval (10–60 seconds)", "seconds", "12"},
         {"capture", "Optional native PNG before measurement", "path"},
         {"theme", "system, light or dark", "theme", "system"},
         {"zoom", "Interface zoom (1–1.5)", "zoom", "1"},
         {"open",
          "mode, model or limits (a composer panel), welcome, viewer or tasks, for captures",
          "panel"},
         {"transcript", "claude, codex, cursor or error: the mock's workspace/<state> messages",
          "provider"},
         {"scroll", "With --transcript: 0 shows the top, 1 the end, like the mock's --scroll",
          "fraction"},
         {"expand-groups",
          "With --transcript: open every activity group and the first file diff, like the mock's"},
         {"display", "secondary (the first that is not the main one) or a screen index", "screen"},
         {"hold", "Keep the window open for review instead of measuring"}});
    parser.process(app);
    const auto work = parser.value("work");
    const auto visibility = parser.value("visibility");
    bool validSeconds = false;
    const int seconds = parser.value("seconds").toInt(&validSeconds);
    if ((work != "running" && work != "idle") ||
        (visibility != "active" && visibility != "inactive" && visibility != "hidden" &&
         visibility != "minimized") ||
        !validSeconds || seconds < 10 || seconds > 60) {
        report({{"event", "error"}, {"reason", "Invalid workload, visibility or duration"}});
        return 2;
    }
    const auto themeName = parser.value("theme");
    bool validZoom = false;
    const qreal zoom = parser.value("zoom").toDouble(&validZoom);
    if ((themeName != "system" && themeName != "light" && themeName != "dark") || !validZoom ||
        zoom < 1 || zoom > 1.5) {
        report({{"event", "error"}, {"reason", "Invalid theme or zoom"}});
        return 2;
    }
    // The composer button that opens each panel, as in the mock's --open.
    const QHash<QString, QString> panelButtons{
        {"mode", "permissionButton"}, {"model", "modelButton"}, {"limits", "usageLimitsButton"}};
    const auto panel = parser.value("open");
    // `welcome` opens no panel: the conversation has no messages, like the mock's welcome state.
    const bool welcome = panel == "welcome";
    if (!panel.isEmpty() && !welcome && panel != "viewer" && panel != "tasks" &&
        !panelButtons.contains(panel)) {
        report({{"event", "error"}, {"reason", "Invalid panel"}});
        return 2;
    }
    const auto transcript = parser.value("transcript");
    if (!transcript.isEmpty() && transcript != "claude" && transcript != "codex" &&
        transcript != "cursor" && transcript != "error") {
        report({{"event", "error"}, {"reason", "Invalid transcript"}});
        return 2;
    }
    // Captures show a mock conversation; CPU samples keep the default workload.
    const bool capture = !panel.isEmpty() || !transcript.isEmpty();
    // workspace/error is the mock's Claude conversation whose provider stopped answering.
    const QString captureProvider =
        transcript.isEmpty() || transcript == "error" ? QStringLiteral("claude") : transcript;
    ketplus::ThemeManager theme(app);
    // Applied before the view exists, like a saved appearance, for app/mock captures.
    auto& appearance = ket::Appearance::instance();
    if (themeName != "system")
        appearance.setMode(themeName == "dark" ? ket::Appearance::Mode::Dark
                                               : ket::Appearance::Mode::Light);
    appearance.setZoom(zoom);
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
    // Captures show the mock's conversation and its usage; CPU samples keep the default.
    if (capture) {
        auto active = conversations.first().toMap();
        active.insert("provider", captureProvider);
        conversations[0] = active;
        const auto resetsAt = [](int day, int hour, int minute) {
            return QDateTime(QDate(2026, 11, day), QTime(hour, minute)).toSecsSinceEpoch();
        };
        view.setUsageSnapshot(
            {{"provider", "claude"},
             {"context",
              QVariantMap{{"available", true}, {"usedTokens", 80000}, {"limitTokens", 500000}}},
             {"session", QVariantMap{{"mode", "runtime"},
                                     {"estimatedCostUsd", 139.6424},
                                     {"inputTokens", 22383},
                                     {"outputTokens", 1561880},
                                     {"cacheReadInputTokens", 400672151},
                                     {"cacheCreationInputTokens", 3522600}}},
             {"limits", QVariantList{QVariantMap{{"id", "five-hour"},
                                                 {"label", "Session · 5 hours"},
                                                 {"available", true},
                                                 {"usedPercent", 2},
                                                 {"resetsAt", resetsAt(6, 12, 10)}},
                                     QVariantMap{{"id", "weekly"},
                                                 {"label", "Weekly · all models"},
                                                 {"available", true},
                                                 {"usedPercent", 85},
                                                 {"resetsAt", resetsAt(8, 7, 0)}},
                                     QVariantMap{{"id", "weekly-fable"},
                                                 {"label", "Weekly · Fable"},
                                                 {"available", true},
                                                 {"usedPercent", 0},
                                                 {"resetsAt", resetsAt(8, 7, 0)}}}}});
        view.setClaudeUsageRequester([](ketplus::AgentWorkspaceView::UsageReply reply) {
            reply({{"configured", false}}, {});
        });
    }
    view.setConversations(conversations);
    // A fixed catalog, so the model panel opens without a provider. Re-sent on request.
    QVariantList efforts;
    for (const char* effort : {"low", "medium", "high", "xhigh", "max"})
        efforts.append(QVariantMap{{"reasoningEffort", effort}, {"description", effort}});
    const auto model = [&efforts](const QString& provider, const QString& id, const QString& name,
                                  const QString& description, bool isDefault) {
        return QVariantMap{
            {"provider", provider},
            {"id", id},
            {"model", id},
            {"displayName", name},
            {"description", description},
            {"isDefault", isDefault},
            {"modelContextWindow", 500000},
            {"defaultReasoningEffort", "medium"},
            {"supportedReasoningEfforts", efforts},
            {"permissions", ketplus::CodexAppServer::permissionProfilesFor(provider)},
            {"defaultPermission", "workspace-write"}};
    };
    const QVariantList catalog{
        model("claude", "default", "Default (recommended)",
              "Sonnet 5.5 · Efficient for routine tasks", true),
        model("claude", "opus", "Opus 5.5", "For complex work and everyday tasks", false),
        model("claude", "fable", "Fable 5.1", "For your toughest challenges", false),
        model("codex", "astra", "GPT-6-Astra", "Available through Codex", true),
        model("codex", "luna", "GPT-5.6-Luna", "Available through Codex", false)};
    view.setModels(catalog);
    QObject::connect(&view, &ketplus::AgentWorkspaceView::modelsRequested, &view,
                     [&view, catalog] { view.setModels(catalog); });
    if (welcome)
        view.resetMessages({}, "idle-cpu-fixture");
    else if (!transcript.isEmpty())
        view.resetMessages(ketplus::benchmark::transcriptFixture(transcript, KETPLUS_MOCK_ASSETS),
                           "idle-cpu-fixture");
    else
        view.resetMessages(
            {QVariantMap{{"kind", "user"},
                         {"messageKey", "prompt"},
                         {"body", "Inspect the repository and run the focused checks."}},
             QVariantMap{{"kind", "assistant"},
                         {"messageKey", "response"},
                         {"body", "The patch is ready. Checking the remaining tasks."}}},
            "idle-cpu-fixture");
    view.setSessionState("Idle CPU qualification", work == "running" ? "Working" : "Ready",
                         work == "running");
    view.show();
    // Like the mock's --display: review windows stay off the main screen.
    if (parser.isSet("display")) {
        const QList<QScreen*> screens = QGuiApplication::screens();
        QScreen* target = nullptr;
        if (parser.value("display") == "secondary") {
            for (QScreen* candidate : screens)
                if (candidate != QGuiApplication::primaryScreen() && !target)
                    target = candidate;
        } else {
            bool ok = false;
            const int index = parser.value("display").toInt(&ok);
            if (ok && index >= 0 && index < screens.size())
                target = screens.at(index);
        }
        if (target) {
            view.windowHandle()->setScreen(target);
            view.move(target->availableGeometry().topLeft() + QPoint(40, 40));
        }
        report({{"event", "display"}, {"screen", target ? target->name() : QString()}});
    }
    view.raise();
    view.activateWindow();
    view.quickView()->requestActivate();
    // Offscreen (QT_QPA_PLATFORM=offscreen) only captures. It has no application state or CPU
    // fixture to qualify, so a locked or headless Mac can still compare the window with the mock.
    const bool offscreenCapture = app.platformName() == "offscreen" && parser.isSet("capture");
    const auto expected = [&] {
        if (offscreenCapture)
            return view.isVisible();
        if (visibility == "hidden")
            return !view.isVisible();
        if (visibility == "minimized")
            return view.isMinimized();
        return view.isVisible() && !view.isMinimized() && stateName() == visibility;
    };
    if (visibility == "hidden")
        QTimer::singleShot(1000, &view, &QWidget::hide);
    if (visibility == "minimized")
        QTimer::singleShot(1000, &view, &QWidget::showMinimized);
    if (capture)
        view.setConversationConfiguration({{"provider", captureProvider},
                                           {"model", captureProvider == "claude"  ? "opus"
                                                     : captureProvider == "codex" ? "astra"
                                                                                  : "auto"},
                                           {"effort", "medium"},
                                           {"permission", "workspace-write"},
                                           {"compactWindowSupported", true}});
    // Before the scroll: an opened group changes the transcript's height.
    if (!transcript.isEmpty() && parser.isSet("expand-groups"))
        QTimer::singleShot(600, &view, [&view] {
            const auto open = [](const auto& self, QQuickItem* item) -> void {
                if (item->objectName() == "transcriptGroupHeader")
                    QMetaObject::invokeMethod(item, "clicked");
                for (auto* child : item->childItems())
                    self(self, child);
            };
            open(open, view.quickView()->rootObject());
            // The mock opens the first file of a multi-file edit on its diff.
            QTimer::singleShot(200, &view, [&view] {
                const auto first = [](const auto& self, QQuickItem* item) -> QQuickItem* {
                    if (item->objectName() == "transcriptFileStep" &&
                        item->property("expandable").toBool())
                        return item;
                    for (auto* child : item->childItems())
                        if (auto* found = self(self, child))
                            return found;
                    return nullptr;
                };
                if (auto* step = first(first, view.quickView()->rootObject()))
                    QMetaObject::invokeMethod(step, "clicked");
            });
        });
    if (!transcript.isEmpty() && parser.isSet("scroll"))
        QTimer::singleShot(1000, &view, [&view, fraction = parser.value("scroll").toDouble()] {
            auto* list = findItem(view.quickView()->rootObject(), "agentTranscript");
            if (!list)
                return;
            list->setProperty("followTail", false);
            const qreal range = list->property("contentHeight").toDouble() - list->height();
            list->setProperty("contentY", list->property("originY").toDouble() +
                                              qBound(0.0, fraction, 1.0) * qMax(0.0, range));
        });
    // The mock's background tasks (Fixtures.backgroundTasks), as the daemon reports them.
    if (panel == "tasks") {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        view.setBackgroundTasks({QVariantMap{{"task_id", "dev"},
                                             {"kind", "shell"},
                                             {"title", "npm run dev"},
                                             {"command", "npm run dev"},
                                             {"status", "running"},
                                             {"started_at", now - 134'000}},
                                 QVariantMap{{"task_id", "review"},
                                             {"kind", "agent"},
                                             {"title", "Review the diff"},
                                             {"status", "completed"},
                                             {"started_at", now - 60'000},
                                             {"ended_at", now - 12'000},
                                             {"duration_ms", 48'000}},
                                 QVariantMap{{"task_id", "ctest"},
                                             {"kind", "shell"},
                                             {"title", "ctest --test-dir build"},
                                             {"command", "ctest --test-dir build"},
                                             {"status", "failed"},
                                             {"error", "2 tests failed"},
                                             {"started_at", now - 30'000},
                                             {"ended_at", now - 20'000},
                                             {"duration_ms", 9'800}}});
    }
    // The mock's image viewer on the Claude prompt's capture.
    if (panel == "viewer")
        QTimer::singleShot(1000, &view, [&view] {
            auto* viewer = view.quickView()->rootObject()->findChild<QObject*>("imagePreview");
            const QString shot =
                QUrl::fromLocalFile(QStringLiteral(KETPLUS_MOCK_ASSETS "/screenshot-sidebar.png"))
                    .toString();
            if (!viewer ||
                !QMetaObject::invokeMethod(viewer, "showImages",
                                           Q_ARG(QVariant, QVariantList{shot}), Q_ARG(QVariant, 0)))
                report({{"event", "error"}, {"reason", "Image viewer not found"}});
        });
    else if (!panel.isEmpty() && !welcome && panel != "tasks")
        QTimer::singleShot(1000, &view, [&] {
            auto* button = findItem(view.quickView()->rootObject(), panelButtons.value(panel));
            if (!button || !QMetaObject::invokeMethod(button, "clicked"))
                report({{"event", "error"}, {"reason", "Panel button not found"}});
        });
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
    const auto changedDuringSample = [&](const char* reason) {
        if (!measuring)
            return;
        stateChanged = true;
        report({{"event", "fixture-change"}, {"reason", reason}});
    };
    FixtureWatcher watcher(changedDuringSample);
    view.installEventFilter(&watcher);
    view.quickView()->installEventFilter(&watcher);
    QObject::connect(view.windowHandle(), &QWindow::screenChanged, &app,
                     [&] { changedDuringSample("screen"); });
    QElapsedTimer wall;
    int frames = 0;
    double cpuStart = 0;
    QObject::connect(view.quickView(), &QQuickWindow::frameSwapped, &app, [&] {
        if (measuring)
            ++frames;
    });
    // Conditions that make a sample describe the documented fixture.
    const auto fixture = [&] {
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
        const bool timerRunning = !timer || timer->property("running").toBool();
        const bool valid = app.platformName() == "cocoa" && metal &&
                           qFuzzyCompare(view.quickView()->devicePixelRatio(), 2.0) &&
                           view.size() == QSize(1440, 900) && visibleRows == 3 && !timerRunning &&
                           clockHeld == (work == "running" && visibility != "hidden" &&
                                         visibility != "minimized" && view.workingMotionEnabled());
        return QJsonObject{{"fixtureValid", valid},
                           {"platform", app.platformName()},
                           {"renderer", metal ? "metal" : "other"},
                           {"state", stateName()},
                           {"visible", view.isVisible()},
                           {"minimized", view.isMinimized()},
                           {"dpr", view.quickView()->devicePixelRatio()},
                           {"width", view.width()},
                           {"height", view.height()},
                           {"visibleSidebarRows", visibleRows},
                           {"sidebarClockHolders", sidebarClockHolders},
                           {"transcriptClockHeld", clockHeld},
                           {"measurementTimerRunning", timerRunning}};
    };
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
            if (offscreenCapture) {
                report({{"event", "captured"}, {"path", capture}});
                app.exit(0);
                return;
            }
            stable.restart();
            return;
        }
        readiness.stop();
        readinessDeadline.stop();
        auto begin = fixture();
        fixtureValid = begin.value("fixtureValid").toBool();
        begin.insert("event", "begin");
        begin.insert("pid", qint64(app.applicationPid()));
        report(begin);
        measuring = true;
        cpuStart = cpuSeconds();
        wall.start();
        QTimer::singleShot(seconds * 1000, Qt::PreciseTimer, &app, [&] {
            const auto elapsed = wall.nsecsElapsed() / 1e9;
            const auto cpu = (cpuSeconds() - cpuStart) / elapsed * 100;
            measuring = false;
            // The fixture must still be the one measured at the start.
            auto end = fixture();
            const bool fixtureHeld = end.value("fixtureValid").toBool();
            end.insert("event", "end");
            report(end);
            const bool valid = fixtureValid && fixtureHeld && expected() && !stateChanged;
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
    // A review window: no qualification, no measurement, open until it is closed.
    if (parser.isSet("hold"))
        return app.exec();
    readiness.start();
    QObject::connect(&readinessDeadline, &QTimer::timeout, &app, [&] {
        report({{"event", "error"},
                {"reason", "Timed out waiting for stable window state"},
                {"fixture", fixture()}});
        app.exit(2);
    });
    readinessDeadline.start(60000);
    return app.exec();
}
