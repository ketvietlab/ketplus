// Scroll benchmark: drives the real transcript with synthesized wheel/flick input
// over a daemon journal supplied on the command line, and records frame timing.
// With KETPLUS_CHAT_LAYOUT_TRACE set it also produces the trace that
// benchmarks/transcript-scroll-metrics.py reduces to the M1/M2 scroll metrics.
//
// The journal is the operator's own data: it is copied into a temporary state
// directory, never written back, and nothing from it enters the repository.
//
// Usage: ketplus_transcript_scroll_benchmark <journal.sqlite3> <scenario> <outdir>
//                                            [--title-substr S]
// Scenarios: prefetch-up, notch-down, notch-up, pad-down, pad-up, flick-down,
//            flick-up, full, all
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"
#include "ui/Theme.h"

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QQuickItem>
#include <QQuickView>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cstdio>
#include <functional>

using namespace ketplus;
using namespace ketplus::daemon;

namespace {
QElapsedTimer gClock;
qint64 nowNs() { return gClock.nsecsElapsed(); }

struct FrameRec {
    qint64 animNs{0};      // afterAnimating (GUI)
    qint64 syncBeginNs{0}; // beforeSynchronizing
    qint64 syncEndNs{0};
    qint64 renderBeginNs{0};
    qint64 swapNs{0};
    qint64 busyNs{0}; // GUI busy since previous afterAnimating
};

struct Recorder {
    QMutex lock;
    QList<qint64> swaps;
    QList<qint64> renderDur;
    QList<FrameRec> gui;
    qint64 busyAccum{0};
    qint64 awakeAt{-1};
    bool active{false};
    qint64 rb{0};
};

void wheel(QQuickView* w, QQuickItem* t, QPoint pixel, QPoint angle, Qt::ScrollPhase phase,
           Qt::MouseEventSource src) {
    const auto pos = t->mapToScene(QPointF(t->width() / 2, t->height() / 2));
    QWheelEvent ev(pos, w->mapToGlobal(pos.toPoint()), pixel, angle, Qt::NoButton, Qt::NoModifier,
                   phase, false, src);
    QCoreApplication::sendEvent(w, &ev);
}

void spin(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

double pct(QList<double> v, double p) {
    if (v.isEmpty())
        return 0;
    std::sort(v.begin(), v.end());
    return v.at(std::min<qsizetype>(v.size() - 1, qsizetype(p * (v.size() - 1) + 0.5)));
}

QJsonObject dist(const QList<double>& v) {
    int over16 = 0, over33 = 0, over8 = 0;
    double total = 0;
    for (double x : v) {
        over8 += x > 8.4;
        over16 += x > 16.7;
        over33 += x > 33.4;
        total += x;
    }
    return {{"n", v.size()},      {"p50", pct(v, .5)}, {"p90", pct(v, .9)},
            {"p99", pct(v, .99)}, {"max", pct(v, 1)},  {"gt8_3", over8},
            {"gt16_7", over16},   {"gt33", over33},    {"total_ms", total}};
}

} // namespace

int main(int argc, char** argv) {
    gClock.start();
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("KetPlusScrollBench");
    QCoreApplication::setApplicationName("ScrollBench");
    // Rendered Mermaid diagrams are cached on disk. A warm cache makes every
    // diagram arrive already finished, so the loading-to-rendered transition the
    // benchmark is meant to include would silently never happen.
    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/mermaid")
        .removeRecursively();
    const QStringList args = app.arguments();
    if (args.size() < 4) {
        std::fprintf(stderr, "usage: %s journal scenario outdir\n", argv[0]);
        return 2;
    }
    const QString sourceDb = args.at(1);
    const QString scenario = args.at(2);
    const QString outDir = args.at(3);
    const QString title =
        args.size() > 5 && args.at(4) == "--title-substr" ? args.at(5) : "origin/staging";
    const bool dumpRows = qEnvironmentVariableIsSet("SCROLL_BENCH_DUMP_ROWS");
    QDir().mkpath(outDir);
    QSettings().clear();

    QTemporaryDir state(outDir + "/state-XXXXXX");
    for (const char* suffix : {"", "-wal", "-shm"})
        QFile::copy(sourceDb + suffix, state.filePath(QString("journal.sqlite3") + suffix));
    QString sessionId, worktree;
    {
        LocalJournal journal(state.filePath("journal.sqlite3"));
        for (const auto& s : journal.sessions())
            if (s.title.contains(title)) {
                sessionId = s.id;
                worktree = journal.conversationPath(s);
                break;
            }
    }
    if (sessionId.isEmpty())
        return 3;
    const QString path = QFileInfo(worktree).canonicalFilePath();
    QSettings().setValue(
        agent_message::persistedKey(path),
        QVariantMap{{"path", path},
                    {"activeId", sessionId},
                    {"conversations",
                     QVariantList{QVariantMap{
                         {"id", sessionId}, {"threadId", sessionId}, {"title", "bench"}}}}});
    qputenv("KETPLUS_DAEMON_SOCKET",
            QString("kpsb-%1").arg(QCoreApplication::applicationPid()).toUtf8());
    qputenv("KETPLUS_DAEMON_STATE", state.filePath("journal.sqlite3").toUtf8());
    qputenv("KETPLUS_DAEMON_TOKEN_FILE", state.filePath("local.token").toUtf8());
    qputenv("KETPLUS_DAEMON_EXECUTABLE", QCoreApplication::applicationFilePath().toUtf8());
    qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
    DaemonService service({state.filePath("journal.sqlite3"), state.filePath("local.token"),
                           QString("kpsb-%1").arg(QCoreApplication::applicationPid()),
                           "/nonexistent/ketplus-bench-codex"});
    if (!service.start()) {
        std::fprintf(stderr, "daemon start failed: %s\n", qPrintable(service.errorString()));
        return 4;
    }

    CodexAppServer server;
    AgentSessionManager desktop(&server);
    AgentWorkspaceView view;
    view.setTranscriptPayloadSource(
        [&](const QString& scope, const QString& key, const QString& kind) {
            return desktop.transcriptPayload(path, scope, key, kind);
        });
    ketplus::ThemeManager theme(app);
    view.applyTheme(theme.palette());
    view.resize(1100, 900);

    QJsonArray rowLog;
    QJsonObject summary{
        {"scenario", scenario}, {"platform", QGuiApplication::platformName()}, {"dpr", 0.0}};
    qint64 openStart = 0, firstReset = -1;
    int resetRows = 0;
    const auto logRows = [&](const char* ev, const QVariantList& rows) {
        if (dumpRows)
            rowLog.append(QJsonObject{{"event", ev}, {"rows", QJsonArray::fromVariantList(rows)}});
    };
    QObject::connect(&desktop, &AgentSessionManager::messagesReset, &view,
                     [&](const QString&, const QVariantList& rows) {
                         if (firstReset < 0 && !rows.isEmpty()) {
                             firstReset = nowNs();
                             resetRows = rows.size();
                         }
                         logRows("reset", rows);
                         view.resetMessages(rows, path + ":" + desktop.activeConversationId(path));
                     });
    QObject::connect(&desktop, &AgentSessionManager::messagesPrepended, &view,
                     [&](const QString&, const QVariantList& rows) {
                         logRows("prepend", rows);
                         view.prependMessages(rows);
                     });
    QObject::connect(&desktop, &AgentSessionManager::messagesWindowReset, &view,
                     [&](const QString&, const QVariantList& rows) {
                         logRows("window", rows);
                         view.replaceMessageWindow(rows);
                     });
    QObject::connect(&desktop, &AgentSessionManager::historyStateChanged, &view,
                     [&](const QString&, const QVariantMap& s) { view.setHistoryState(s); });
    QObject::connect(&desktop, &AgentSessionManager::messageAppended, &view,
                     [&](const QString&, int, const QVariantMap& m) { view.appendMessage(m); });
    QObject::connect(
        &desktop, &AgentSessionManager::messageUpdated, &view,
        [&](const QString&, int i, const QVariantMap& m) { view.updateMessage(i, m); });
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
    QObject::connect(&view, &AgentWorkspaceView::latestMessagesRequested, &desktop,
                     [&] { desktop.loadLatestMessages(path); });

    auto* qv = view.quickView();
    Recorder rec;
    qint64 firstSwapAfterReset = -1;
    QObject::connect(
        qv, &QQuickWindow::beforeRendering, qv,
        [&] {
            QMutexLocker l(&rec.lock);
            rec.rb = nowNs();
        },
        Qt::DirectConnection);
    QObject::connect(
        qv, &QQuickWindow::frameSwapped, qv,
        [&] {
            const qint64 t = nowNs();
            QMutexLocker l(&rec.lock);
            if (firstReset >= 0 && firstSwapAfterReset < 0)
                firstSwapAfterReset = t;
            if (rec.active) {
                rec.swaps.append(t);
                rec.renderDur.append(t - rec.rb);
            }
        },
        Qt::DirectConnection);
    auto* dispatcher = QAbstractEventDispatcher::instance();
    QObject::connect(dispatcher, &QAbstractEventDispatcher::awake, [&] {
        if (rec.awakeAt < 0)
            rec.awakeAt = nowNs();
    });
    QObject::connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, [&] {
        if (rec.awakeAt >= 0)
            rec.busyAccum += nowNs() - rec.awakeAt;
        rec.awakeAt = -1;
    });
    QObject::connect(qv, &QQuickWindow::afterAnimating, qv, [&] {
        if (!rec.active)
            return;
        const qint64 t = nowNs();
        if (rec.awakeAt >= 0) { // fold current busy slice
            rec.busyAccum += t - rec.awakeAt;
            rec.awakeAt = t;
        }
        FrameRec f;
        f.animNs = t;
        f.busyNs = rec.busyAccum;
        rec.busyAccum = 0;
        rec.gui.append(f);
    });

    view.show();
    view.raise();
    view.activateWindow();
    spin(300);
    summary["dpr"] = qv->effectiveDevicePixelRatio();
    openStart = nowNs();
    desktop.activate(path);
    auto* root = qv->rootObject();
    QQuickItem* transcript = nullptr;
    QElapsedTimer guard;
    guard.start();
    while (guard.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (desktop.activeConversationId(path) != sessionId) {
            for (const auto& row : desktop.conversations(path)) {
                if (row.toMap().value("id").toString() == sessionId) {
                    desktop.selectConversation(path, sessionId);
                    break;
                }
            }
        }
        transcript = root->findChild<QQuickItem*>("agentTranscript");
        if (firstReset >= 0 && transcript && !view.benchmarkMaterializing() &&
            transcript->opacity() == 1.0 && transcript->property("count").toInt() > 0)
            break;
    }
    const qint64 materialized = nowNs();
    spin(400);
    QJsonObject open{{"first_reset_ms", (firstReset - openStart) / 1e6},
                     {"reset_rows", resetRows},
                     {"first_swap_after_reset_ms",
                      firstSwapAfterReset > 0 ? (firstSwapAfterReset - openStart) / 1e6 : -1},
                     {"materialized_visible_ms", (materialized - openStart) / 1e6},
                     {"rendered_rows", transcript ? transcript->property("count").toInt() : -1}};
    summary["open"] = open;
    if (!transcript || transcript->property("count").toInt() == 0) {
        std::fprintf(stderr,
                     "conversation did not hydrate: window=%d conversations=%lld selected=%d; no "
                     "scroll measurement is valid\n",
                     int(server.hasDaemonTranscriptWindow()),
                     qint64(desktop.conversations(path).size()),
                     int(desktop.activeConversationId(path) == sessionId));
        return 5;
    }
    transcript->setProperty("idlePrefetchDone", true);

    const auto history = [&] { return desktop.historyState(path); };
    const auto waitLoad = [&] {
        QElapsedTimer t;
        t.start();
        do
            spin(20);
        while ((history().value("loading").toBool() || view.benchmarkMaterializing()) &&
               t.elapsed() < 10000);
    };
    // Large conversations never run out of history: cap the warm-up paging.
    const int loadPages = qEnvironmentVariableIntegerValue("BENCH_LOAD_PAGES").value_or(8);
    const int scenarioMaxMs = qEnvironmentVariableIntegerValue("BENCH_MAX_MS").value_or(45000);
    const auto loadAll = [&] {
        for (int i = 0; history().value("hasOlder").toBool() && i < loadPages; ++i) {
            desktop.loadOlderMessages(path, transcript->height(), 120, 0);
            waitLoad();
        }
        spin(500);
    };
    const auto at = [&](double p) {
        view.setBenchmarkScrollProgress(p);
        spin(600);
    };
    const auto contentAbove = [&] {
        return transcript->property("contentY").toDouble() -
               transcript->property("originY").toDouble();
    };
    const auto contentBelow = [&] {
        return transcript->property("originY").toDouble() +
               transcript->property("contentHeight").toDouble() -
               transcript->property("contentY").toDouble() - transcript->height();
    };

    QJsonObject results;
    const auto measure = [&](const QString& name, const std::function<bool()>& step, int intervalMs,
                             int maxMs) {
        {
            QMutexLocker l(&rec.lock);
            rec.swaps.clear();
            rec.renderDur.clear();
        }
        rec.gui.clear();
        rec.busyAccum = 0;
        rec.active = true;
        const int olderBefore = transcript->property("olderPageRequests").toInt();
        const int newerBefore = transcript->property("newerPageRequests").toInt();
        QElapsedTimer t;
        t.start();
        QList<double> dispatch;
        bool done = false;
        QTimer timer;
        timer.setTimerType(Qt::PreciseTimer);
        timer.setInterval(intervalMs);
        QObject::connect(&timer, &QTimer::timeout, [&] {
            const qint64 s = nowNs();
            done = !step() || t.elapsed() > maxMs;
            dispatch.append((nowNs() - s) / 1e6);
        });
        timer.start();
        while (!done)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        timer.stop();
        // Let kinetic tail finish.
        QElapsedTimer tail;
        tail.start();
        while ((transcript->property("moving").toBool() || tail.elapsed() < 200) &&
               tail.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        rec.active = false;
        const double wall = t.elapsed();
        QList<double> swapIntervals, render, guiIntervals, busy;
        {
            QMutexLocker l(&rec.lock);
            for (int i = 1; i < rec.swaps.size(); ++i)
                swapIntervals.append((rec.swaps[i] - rec.swaps[i - 1]) / 1e6);
            for (auto r : rec.renderDur)
                render.append(r / 1e6);
        }
        QFile csv(outDir + "/" + summary["platform"].toString() + "-" + name + ".csv");
        (void)csv.open(QIODevice::WriteOnly);
        csv.write("frame,gui_interval_ms,gui_busy_ms\n");
        int slowFrames = 0;
        for (int i = 1; i < rec.gui.size(); ++i) {
            const auto& a = rec.gui[i - 1];
            const auto& b = rec.gui[i];
            const double iv = (b.animNs - a.animNs) / 1e6;
            guiIntervals.append(iv);
            busy.append(b.busyNs / 1e6);
            csv.write(QString("%1,%2,%3\n")
                          .arg(i)
                          .arg(iv, 0, 'f', 2)
                          .arg(b.busyNs / 1e6, 0, 'f', 2)
                          .toUtf8());
            slowFrames += iv > 16.7;
        }
        results[name] = QJsonObject{
            {"wall_ms", wall},
            {"swap_interval", dist(swapIntervals)},
            {"gui_frame_interval", dist(guiIntervals)},
            {"gui_busy_per_frame", dist(busy)},
            {"render_thread", dist(render)},
            {"input_dispatch", dist(dispatch)},
            {"older_requests", transcript->property("olderPageRequests").toInt() - olderBefore},
            {"newer_requests", transcript->property("newerPageRequests").toInt() - newerBefore},
            {"slow_frames", slowFrames},
            {"end_rows", transcript->property("count").toInt()},
            {"end_content_height", transcript->property("contentHeight").toDouble()}};
        std::printf("%s %s done wall=%.0fms\n", qPrintable(summary["platform"].toString()),
                    qPrintable(name), wall);
        std::fflush(stdout);
    };

    const auto notch = [&](int dir) {
        return [&, dir] {
            if ((dir > 0 && contentAbove() < 1) || (dir < 0 && contentBelow() < 1))
                return !(dir > 0 && history().value("hasOlder").toBool());
            wheel(qv, transcript, {}, {0, 120 * dir}, Qt::NoScrollPhase,
                  Qt::MouseEventNotSynthesized);
            return true;
        };
    };
    // Trackpad: 40 updates @ 8ms (~22px each), end, 50 momentum events decaying.
    const auto pad = [&](int dir) {
        auto phaseIndex = std::make_shared<int>(0);
        return [&, dir, phaseIndex] {
            const bool edge =
                (dir > 0 && contentAbove() < 1 && !history().value("hasOlder").toBool()) ||
                (dir < 0 && contentBelow() < 1 && !history().value("hasNewer").toBool());
            int i = (*phaseIndex)++ % 100;
            if (edge && i == 0)
                return false;
            if (i == 0)
                wheel(qv, transcript, {0, 0}, {0, 0}, Qt::ScrollBegin,
                      Qt::MouseEventSynthesizedBySystem);
            else if (i <= 40)
                wheel(qv, transcript, {0, 22 * dir}, {0, 22 * dir}, Qt::ScrollUpdate,
                      Qt::MouseEventSynthesizedBySystem);
            else if (i == 41)
                wheel(qv, transcript, {0, 0}, {0, 0}, Qt::ScrollEnd,
                      Qt::MouseEventSynthesizedBySystem);
            else if (i < 92) {
                const int d = int(22 * std::pow(0.93, i - 41));
                wheel(qv, transcript, {0, d * dir}, {0, d * dir}, Qt::ScrollMomentum,
                      Qt::MouseEventSynthesizedBySystem);
            } else if (i == 92)
                wheel(qv, transcript, {0, 0}, {0, 0}, Qt::ScrollEnd,
                      Qt::MouseEventSynthesizedBySystem);
            return true;
        };
    };
    const auto flick = [&](int dir) {
        return [&, dir] {
            if ((dir > 0 && contentAbove() < 1) || (dir < 0 && contentBelow() < 1))
                return !(dir > 0 && history().value("hasOlder").toBool());
            if (!transcript->property("flicking").toBool()) {
                const double v = 2500.0 * dir; // default desktop max flick velocity
                QMetaObject::invokeMethod(transcript, "flick", Q_ARG(qreal, 0), Q_ARG(qreal, v));
            }
            return true;
        };
    };

    const bool all = scenario == "all";
    if (all || scenario == "prefetch-up") {
        at(1.0);
        measure("prefetch-up-pad", pad(+1), 8, scenarioMaxMs);
        measure("newer-down-pad", pad(-1), 8, scenarioMaxMs);
    }
    if (all || scenario != "prefetch-up") {
        loadAll();
        const auto run = [&](const QString& n, double start, std::function<bool()> f, int iv) {
            if (all || scenario == n || scenario == "full") {
                at(start);
                measure(n, std::move(f), iv, scenarioMaxMs);
            }
        };
        run("notch-down", 0.0, notch(-1), 40);
        run("notch-up", 1.0, notch(+1), 40);
        run("pad-down", 0.0, pad(-1), 8);
        run("pad-up", 1.0, pad(+1), 8);
        run("flick-down", 0.0, flick(-1), 16);
        run("flick-up", 1.0, flick(+1), 16);
        // Warm repeat of trackpad down (delegates/heights cached).
        run("pad-down-warm", 0.0, pad(-1), 8);
    }
    summary["results"] = results;
    summary["history_end"] = QJsonObject::fromVariantMap(history());
    QFile out(outDir + "/" + summary["platform"].toString() + "-" + scenario + ".json");
    (void)out.open(QIODevice::WriteOnly);
    out.write(QJsonDocument(summary).toJson());
    if (dumpRows) {
        QFile rows(outDir + "/fixture-rows.json");
        (void)rows.open(QIODevice::WriteOnly);
        rows.write(QJsonDocument(rowLog).toJson(QJsonDocument::Compact));
    }
    std::printf("wrote %s\n", qPrintable(out.fileName()));
    QSettings().clear();
    return 0;
}
