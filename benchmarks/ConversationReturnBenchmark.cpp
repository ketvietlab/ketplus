// Conversation return benchmark: A -> B -> A through the real session manager,
// Agent Workspace view and an in-process daemon serving a synthetic journal.
// It measures what a reader sees when coming back to a conversation that was
// just open: how many times the transcript is replaced, how long the GUI thread
// is busy or stalled, and frame spacing until the transcript settles.
//
// The daemon runs on its own thread, as it runs in its own process in the app,
// so its journal reads are not charged to the GUI thread.
//
// Usage: ketplus_conversation_return_benchmark [clean|dirty] [rounds] [answer_kib]
// KETPLUS_BENCHMARK_SCREEN=<name substring> places the window on that display.
//   clean: nothing happens to A while B is shown.
//   dirty: A's window is invalidated while B is shown, as a live event would.
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"
#include "ui/Theme.h"

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QQuickView>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <functional>

using namespace ketplus;
using namespace ketplus::daemon;

namespace {

QElapsedTimer gClock;
qint64 nowNs() { return gClock.nsecsElapsed(); }

double threadCpuMs() {
    timespec value{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value);
    return value.tv_sec * 1000.0 + value.tv_nsec / 1e6;
}

// Waits in a real event loop so the GUI thread blocks while idle: its busy time
// and CPU then measure the transcript, not the wait.
bool waitUntil(const std::function<bool()>& ready, int timeoutMs) {
    if (ready())
        return true;
    QEventLoop loop;
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ready())
            loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return ready();
}

void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QString body(int kib) {
    QString text;
    for (int n = 0; text.size() < kib * 1024; ++n)
        text += QString("## Section %1\n\nThis is a **synthetic conversation** about application "
                        "architecture, performance, and correctness. A [local "
                        "link](https://example.invalid/) stays within the text.\n\n- The first "
                        "item describes the source.\n- The second item describes the result.\n\n"
                        "```cpp\nint value = compute(%1);\n```\n\n")
                    .arg(n);
    return text;
}

double percentile(QList<double> values, double p) {
    if (values.isEmpty())
        return 0;
    std::sort(values.begin(), values.end());
    return values.at(qsizetype(p * (values.size() - 1) + 0.5));
}

QJsonObject summary(const QList<double>& values) {
    return {{"p50", percentile(values, .5)},
            {"p90", percentile(values, .9)},
            {"max", percentile(values, 1)}};
}

} // namespace

int main(int argc, char** argv) {
    gClock.start();
#ifdef Q_OS_MACOS
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("KetPlusDiagnostic");
    QCoreApplication::setApplicationName("ConversationReturn");
    const QString scenario = argc > 1 ? argv[1] : "clean";
    const int rounds = argc > 2 ? QString(argv[2]).toInt() : 10;
    const int kib = argc > 3 ? QString(argv[3]).toInt() : 8;
    if ((scenario != "clean" && scenario != "dirty") || rounds < 1 || kib < 1) {
        std::fprintf(stderr, "usage: %s [clean|dirty] [rounds] [answer_kib]\n", argv[0]);
        return 2;
    }

    QTemporaryDir tmp;
    const QString path = QFileInfo(tmp.path()).canonicalFilePath();
    const QStringList ids{QStringLiteral("return-a"), QStringLiteral("return-b")};
    {
        LocalJournal journal(tmp.filePath("journal.sqlite3"));
        bool seeded = true;
        for (const auto& id : ids) {
            StoredSession session;
            session.id = id;
            session.worktreePath = path;
            session.adapter = QStringLiteral("codex");
            session.state = QStringLiteral("ready");
            session.title = id;
            seeded = seeded && journal.upsertSession(session);
            for (int turn = 0; turn < 90; ++turn) {
                seeded =
                    seeded && journal.appendEvent(id, 1, "user.message",
                                                  {{"text", QString("prompt-%1").arg(turn)}}) > 0;
                seeded = seeded &&
                         journal.appendEvent(id, 1, "item.completed",
                                             {{"item", QJsonObject{{"id", QString::number(turn)},
                                                                   {"type", "agentMessage"},
                                                                   {"phase", "final_answer"},
                                                                   {"text", body(kib)}}}}) > 0;
                seeded = seeded && journal.appendEvent(id, 1, "turn.completed",
                                                       {{"status", "completed"}}) > 0;
            }
        }
        if (!seeded) {
            std::fprintf(stderr, "could not seed the journal\n");
            return 3;
        }
    }
    QSettings().setValue(
        agent_message::persistedKey(path),
        QVariantMap{{"path", path},
                    {"activeId", ids.first()},
                    {"conversations",
                     QVariantList{QVariantMap{
                         {"id", ids.first()}, {"threadId", ids.first()}, {"title", "Return A"}}}}});
    const QString socket = QString("kpcr-%1").arg(QCoreApplication::applicationPid());
    qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
    qputenv("KETPLUS_DAEMON_STATE", tmp.filePath("journal.sqlite3").toUtf8());
    qputenv("KETPLUS_DAEMON_TOKEN_FILE", tmp.filePath("local.token").toUtf8());
    qputenv("KETPLUS_DAEMON_EXECUTABLE", QCoreApplication::applicationFilePath().toUtf8());
    qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");

    QThread daemonThread;
    daemonThread.start();
    QObject daemonContext;
    daemonContext.moveToThread(&daemonThread);
    DaemonService* service = nullptr;
    bool started = false;
    QMetaObject::invokeMethod(
        &daemonContext,
        [&] {
            service = new DaemonService({tmp.filePath("journal.sqlite3"),
                                         tmp.filePath("local.token"),
                                         socket,
                                         "/nonexistent/ketplus-return-codex",
                                         {},
                                         {}});
            started = service->start();
        },
        Qt::BlockingQueuedConnection);
    const auto stopDaemon = [&] {
        QMetaObject::invokeMethod(
            &daemonContext, [&] { delete service; }, Qt::BlockingQueuedConnection);
        daemonThread.quit();
        daemonThread.wait();
    };
    if (!started) {
        stopDaemon();
        return 3;
    }

    CodexAppServer server;
    AgentSessionManager desktop(&server);
    AgentWorkspaceView view;
    ThemeManager theme(app);
    view.applyTheme(theme.palette());
    view.resize(1100, 900);
    // KETPLUS_BENCHMARK_SCREEN picks a display by name, so the window that has to
    // stay on top can run on a secondary screen.
    const QString screenName = qEnvironmentVariable("KETPLUS_BENCHMARK_SCREEN");
    for (QScreen* screen : QGuiApplication::screens()) {
        if (!screenName.isEmpty() && screen->name().contains(screenName, Qt::CaseInsensitive)) {
            const QRect area = screen->availableGeometry();
            view.setGeometry(area.x(), area.y(), std::min(1100, area.width()),
                             std::min(900, area.height()));
            break;
        }
    }
    view.setActivePath(path);
    view.setTranscriptPayloadSource(
        [&](const QString& scope, const QString& key, const QString& kind) {
            return desktop.transcriptPayload(path, scope, key, kind);
        });

    int resets = 0, prepends = 0, windows = 0, loads = 0;
    qint64 lastMutation = 0;
    QObject::connect(&desktop, &AgentSessionManager::messagesReset, &view,
                     [&](const QString&, const QVariantList& rows) {
                         ++resets;
                         lastMutation = nowNs();
                         view.resetMessages(rows, path + ":" + desktop.activeConversationId(path));
                     });
    QObject::connect(&desktop, &AgentSessionManager::messagesPrepended, &view,
                     [&](const QString&, const QVariantList& rows) {
                         ++prepends;
                         lastMutation = nowNs();
                         view.prependMessages(rows);
                     });
    QObject::connect(&desktop, &AgentSessionManager::messagesWindowReset, &view,
                     [&](const QString&, const QVariantList& rows) {
                         ++windows;
                         lastMutation = nowNs();
                         view.replaceMessageWindow(rows);
                     });
    bool wasLoading = false;
    QObject::connect(&desktop, &AgentSessionManager::historyStateChanged, &view,
                     [&](const QString&, const QVariantMap& state) {
                         const bool loading = state.value("loading").toBool();
                         loads += loading && !wasLoading;
                         wasLoading = loading;
                         view.setHistoryState(state);
                     });
    QObject::connect(&desktop, &AgentSessionManager::messageAppended, &view,
                     [&](const QString&, int, const QVariantMap& m) { view.appendMessage(m); });
    QObject::connect(
        &desktop, &AgentSessionManager::messageUpdated, &view,
        [&](const QString&, int i, const QVariantMap& m) { view.updateMessage(i, m); });
    QObject::connect(
        &desktop, &AgentSessionManager::conversationsChanged, &view,
        [&](const QString&, const QVariantList& rows) { view.setConversations(rows); });
    QObject::connect(&view, &AgentWorkspaceView::transcriptInputActiveChanged, &desktop,
                     &AgentSessionManager::setTranscriptInputActive);
    QObject::connect(&view, &AgentWorkspaceView::transcriptScrollingChanged, &desktop,
                     &AgentSessionManager::setTranscriptScrolling);
    QObject::connect(&view, &AgentWorkspaceView::transcriptReadingKeyChanged, &desktop,
                     &AgentSessionManager::setTranscriptReadingKey);
    QObject::connect(&view, &AgentWorkspaceView::transcriptExtentReported, &desktop,
                     &AgentSessionManager::setTranscriptExtent);
    QObject::connect(
        &view, &AgentWorkspaceView::olderMessagesRequested, &desktop,
        [&](double h, double r, qint64 tok) { desktop.loadOlderMessages(path, h, r, tok); });
    QObject::connect(
        &view, &AgentWorkspaceView::newerMessagesRequested, &desktop,
        [&](double h, double r, qint64 tok) { desktop.loadNewerMessages(path, h, r, tok); });

    // GUI thread busy time, longest stall and frame spacing while measuring.
    bool measuring = false;
    qint64 busy = 0, awakeAt = -1, lastTick = 0, maxStall = 0;
    QMutex swapLock;
    QList<qint64> swaps;
    auto* dispatcher = QAbstractEventDispatcher::instance();
    QObject::connect(dispatcher, &QAbstractEventDispatcher::awake, [&] {
        if (awakeAt < 0)
            awakeAt = nowNs();
    });
    QObject::connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, [&] {
        if (awakeAt >= 0 && measuring)
            busy += nowNs() - awakeAt;
        awakeAt = -1;
    });
    QTimer probe;
    probe.setTimerType(Qt::PreciseTimer);
    probe.setInterval(1);
    QObject::connect(&probe, &QTimer::timeout, [&] {
        const qint64 t = nowNs();
        if (measuring)
            maxStall = std::max(maxStall, t - lastTick);
        lastTick = t;
    });
    probe.start();
    QObject::connect(
        view.quickView(), &QQuickWindow::frameSwapped, view.quickView(),
        [&] {
            QMutexLocker locker(&swapLock);
            if (measuring)
                swaps.append(nowNs());
        },
        Qt::DirectConnection);

    // macOS stops rendering an occluded window; keep it on top so every round
    // renders the frames it is measured by.
    view.setWindowFlag(Qt::WindowStaysOnTopHint);
    view.show();
    view.raise();
    view.activateWindow();
    spin(300);

    // Settled: no load running, nothing materializing, and no transcript
    // mutation for a while (a backfill page is scheduled after the first one).
    const auto settle = [&](qint64 since) {
        return waitUntil(
            [&] {
                return !desktop.historyState(path).value("loading").toBool() &&
                       !view.benchmarkMaterializing() &&
                       nowNs() - std::max(since, lastMutation) > 600'000'000;
            },
            20000);
    };
    const auto select = [&](const QString& id) {
        if (desktop.activeConversationId(path) == id)
            return;
        desktop.selectConversation(path, id);
    };

    desktop.activate(path);
    if (!settle(nowNs()) || desktop.messages(path).isEmpty()) {
        std::fprintf(stderr, "conversation A did not load\n");
        stopDaemon();
        return 4;
    }
    QElapsedTimer discover;
    discover.start();
    while (desktop.conversations(path).size() < 2 && discover.elapsed() < 10000)
        spin(20);
    select(ids.at(1));
    if (!settle(nowNs()) || desktop.activeConversationId(path) != ids.at(1)) {
        std::fprintf(stderr, "conversation B did not load\n");
        stopDaemon();
        return 4;
    }

    QList<double> usable, settled, busyMs, cpuMs, stall, worstFrame, mutationCounts, loadCounts;
    int keptWindows = 0;
    QJsonArray runs;
    for (int round = 0; round < rounds; ++round) {
        select(ids.at(0));
        settle(nowNs());
        const auto before = desktop.messages(path);
        select(ids.at(1));
        settle(nowNs());
        if (scenario == "dirty") {
            // What a live event of A does while B is on screen.
            emit server.daemonTranscriptInvalidated(ids.at(0));
            spin(50);
        }

        resets = prepends = windows = loads = 0;
        busy = 0;
        maxStall = 0;
        {
            QMutexLocker locker(&swapLock);
            swaps.clear();
        }
        const double cpuStart = threadCpuMs();
        const qint64 start = nowNs();
        lastTick = start;
        lastMutation = 0;
        awakeAt = start;
        measuring = true;
        select(ids.at(0));
        const double usableMs = waitUntil([&] { return view.benchmarkUsableViewport(); }, 10000)
                                    ? (nowNs() - start) / 1e6
                                    : -1;
        settle(start);
        measuring = false;
        const double settledMs = (std::max(lastMutation, start) - start) / 1e6;
        const double cpu = threadCpuMs() - cpuStart;
        double worst = 0;
        int frames = 0;
        {
            QMutexLocker locker(&swapLock);
            frames = int(swaps.size());
            qint64 previous = start;
            for (const qint64 swap : swaps) {
                worst = std::max(worst, (swap - previous) / 1e6);
                previous = swap;
            }
        }
        const auto after = desktop.messages(path);
        const bool kept = after == before;
        keptWindows += kept;
        const int mutations = resets + prepends + windows;
        usable.append(usableMs);
        settled.append(settledMs);
        busyMs.append(busy / 1e6);
        cpuMs.append(cpu);
        stall.append(maxStall / 1e6);
        worstFrame.append(worst);
        mutationCounts.append(mutations);
        loadCounts.append(loads);
        runs.append(QJsonObject{{"resets", resets},
                                {"prepends", prepends},
                                {"windows", windows},
                                {"history_loads", loads},
                                {"rows_before", before.size()},
                                {"rows_after", after.size()},
                                {"kept_window", kept},
                                {"usable_ms", usableMs},
                                {"last_mutation_ms", settledMs},
                                {"gui_busy_ms", busy / 1e6},
                                {"gui_cpu_ms", cpu},
                                {"max_gui_stall_ms", maxStall / 1e6},
                                {"frames", frames},
                                {"worst_frame_gap_ms", worst}});
    }

    const QJsonObject result{
        {"scenario", scenario},
        {"rounds", rounds},
        {"answer_kib", kib},
        {"graphics_api", int(view.quickView()->rendererInterface()->graphicsApi())},
        {"kept_window_rounds", keptWindows},
        {"transcript_mutations", summary(mutationCounts)},
        {"history_loads", summary(loadCounts)},
        {"usable_ms", summary(usable)},
        {"last_mutation_ms", summary(settled)},
        {"gui_busy_ms", summary(busyMs)},
        {"gui_cpu_ms", summary(cpuMs)},
        {"max_gui_stall_ms", summary(stall)},
        {"worst_frame_gap_ms", summary(worstFrame)},
        {"runs", runs}};
    std::puts(QJsonDocument(result).toJson(QJsonDocument::Indented).constData());
    stopDaemon();
    return 0;
}
