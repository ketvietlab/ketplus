#include "agent/AgentWorkspaceView.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QQuickView>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QTimer>
#include <QVariantList>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <sys/resource.h>

namespace {

double cpuSeconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec) + usage.ru_utime.tv_usec / 1'000'000.0 +
           static_cast<double>(usage.ru_stime.tv_sec) + usage.ru_stime.tv_usec / 1'000'000.0;
}

bool waitForFrame(QQuickView* view, const int timeoutMs = 5000) {
    QEventLoop loop;
    bool received = false;
    const auto connection = QObject::connect(view, &QQuickWindow::frameSwapped, &loop, [&] {
        received = true;
        loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    view->update();
    loop.exec();
    QObject::disconnect(connection);
    return received;
}

bool waitUntil(const std::function<bool()>& predicate, const int timeoutMs = 5000) {
    if (predicate())
        return true;
    QEventLoop loop;
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (predicate())
            loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return predicate();
}

bool waitForUsableViewport(ketplus::AgentWorkspaceView& view, const int timeoutMs = 5000) {
    return waitUntil([&] { return view.benchmarkUsableViewport(); }, timeoutMs);
}

bool waitForMaterialization(ketplus::AgentWorkspaceView& view, const int timeoutMs = 5000) {
    return waitUntil([&] { return !view.benchmarkMaterializing(); }, timeoutMs);
}

class EventLoopGapProbe final {
  public:
    EventLoopGapProbe() {
        elapsed_.start();
        lastNanoseconds_ = elapsed_.nsecsElapsed();
        timer_.setTimerType(Qt::PreciseTimer);
        timer_.setInterval(1);
        QObject::connect(&timer_, &QTimer::timeout, [&] {
            const qint64 now = elapsed_.nsecsElapsed();
            maximumGapNanoseconds_ = std::max(maximumGapNanoseconds_, now - lastNanoseconds_);
            lastNanoseconds_ = now;
        });
        timer_.start();
    }

    void restartMeasurement() {
        maximumGapNanoseconds_ = 0;
        lastNanoseconds_ = elapsed_.nsecsElapsed();
    }
    double maximumGapMs() const { return maximumGapNanoseconds_ / 1'000'000.0; }

  private:
    QElapsedTimer elapsed_;
    QTimer timer_;
    qint64 lastNanoseconds_{0};
    qint64 maximumGapNanoseconds_{0};
};

struct TimedRun final {
    double wallSeconds{0.0};
    double cpuPercent{0.0};
    int frames{0};
};

const char* graphicsApiName(const QSGRendererInterface::GraphicsApi api) {
    switch (api) {
    case QSGRendererInterface::Metal:
        return "metal";
    case QSGRendererInterface::OpenGL:
        return "opengl";
    case QSGRendererInterface::Vulkan:
        return "vulkan";
    case QSGRendererInterface::Direct3D11:
        return "direct3d11";
    case QSGRendererInterface::Direct3D12:
        return "direct3d12";
    case QSGRendererInterface::Software:
        return "software";
    default:
        return "unknown";
    }
}

TimedRun runInterval(QQuickView* view, const int durationMs,
                     const std::function<void(double)>& tick = {}) {
    TimedRun result;
    QElapsedTimer wall;
    QEventLoop loop;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(8);
    const auto frameConnection =
        QObject::connect(view, &QQuickWindow::frameSwapped, [&] { ++result.frames; });
    if (tick) {
        QObject::connect(&timer, &QTimer::timeout, [&] {
            const double progress = std::min(1.0, wall.elapsed() / static_cast<double>(durationMs));
            tick(progress);
        });
        timer.start();
    }
    QTimer::singleShot(durationMs, &loop, &QEventLoop::quit);
    const double cpuStart = cpuSeconds();
    wall.start();
    loop.exec();
    result.wallSeconds = wall.nsecsElapsed() / 1'000'000'000.0;
    const double cpuElapsed = cpuSeconds() - cpuStart;
    result.cpuPercent = result.wallSeconds > 0.0 ? cpuElapsed / result.wallSeconds * 100.0 : 0.0;
    QObject::disconnect(frameConnection);
    return result;
}

QVariantList makeConversation(const int conversation, const int count) {
    QVariantList messages;
    messages.reserve(count);
    for (int index = 0; index < count; ++index) {
        const bool user = index % 4 == 0;
        const bool tool = index % 11 == 7;
        messages.append(QVariantMap{
            {QStringLiteral("kind"), tool   ? QStringLiteral("tool")
                                     : user ? QStringLiteral("user")
                                            : QStringLiteral("assistant")},
            {QStringLiteral("body"),
             tool ? QStringLiteral("Inspecting repository files for conversation %1")
                        .arg(conversation + 1)
             : user ? QStringLiteral("Conversation %1 message %2: inspect this worktree.")
                          .arg(conversation + 1)
                          .arg(index + 1)
                    : QStringLiteral("Synthetic streamed response retained in memory to "
                                     "model a realistic multi-conversation workspace.")},
            {QStringLiteral("meta"), tool   ? QStringLiteral("Tool · completed")
                                     : user ? QStringLiteral("You")
                                            : QStringLiteral("Codex")},
            {QStringLiteral("filePath"), QString{}},
            {QStringLiteral("itemId"), QStringLiteral("%1-%2").arg(conversation).arg(index)},
        });
    }
    return messages;
}

QVector<ketplus::GitWorktree> makeWorktrees(const QString& repositoryPath, const int count) {
    QVector<ketplus::GitWorktree> worktrees;
    worktrees.reserve(count);
    for (int index = 0; index < count; ++index) {
        worktrees.append(
            {.path =
                 QDir(repositoryPath).filePath(QStringLiteral("conversation-%1").arg(index + 1)),
             .head = QStringLiteral("benchmark%1").arg(index),
             .branch = QStringLiteral("conversation/%1").arg(index + 1),
             .current = index == 0});
    }
    return worktrees;
}

int maximumResidentSetKiB() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#ifdef Q_OS_MACOS
    return static_cast<int>(usage.ru_maxrss / 1024);
#else
    return static_cast<int>(usage.ru_maxrss);
#endif
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef Q_OS_MACOS
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption stressOption(QStringLiteral("multi-app-stress"),
                                    QStringLiteral("Run the resident conversation stress case."));
    QCommandLineOption conversationsOption(QStringLiteral("conversations"),
                                           QStringLiteral("Resident conversations per process."),
                                           QStringLiteral("count"), QStringLiteral("6"));
    QCommandLineOption messagesOption(
        QStringLiteral("messages-per-conversation"),
        QStringLiteral("Synthetic messages retained by every conversation."),
        QStringLiteral("count"), QStringLiteral("200"));
    QCommandLineOption repositoryOption(QStringLiteral("repository"),
                                        QStringLiteral("Synthetic repository root."),
                                        QStringLiteral("path"), QDir::currentPath());
    QCommandLineOption instanceOption(QStringLiteral("instance"),
                                      QStringLiteral("One-based process index."),
                                      QStringLiteral("number"), QStringLiteral("1"));
    QCommandLineOption instancesOption(QStringLiteral("instances"),
                                       QStringLiteral("Total concurrent process count."),
                                       QStringLiteral("count"), QStringLiteral("1"));
    QCommandLineOption pathologicalOption(
        QStringLiteral("pathological-assistant-mib"),
        QStringLiteral("Render one synthetic assistant response of this source size."),
        QStringLiteral("mib"), QStringLiteral("0"));
    QCommandLineOption pathologicalStreamRateOption(
        QStringLiteral("pathological-stream-hz"),
        QStringLiteral("Directly stream giant model updates at this rate."), QStringLiteral("hz"),
        QStringLiteral("0"));
    QCommandLineOption pathologicalStreamSecondsOption(
        QStringLiteral("pathological-stream-seconds"),
        QStringLiteral("Duration of the direct giant stream workload."), QStringLiteral("seconds"),
        QStringLiteral("60"));
    parser.addOptions({stressOption, conversationsOption, messagesOption, repositoryOption,
                       instanceOption, instancesOption, pathologicalOption,
                       pathologicalStreamRateOption, pathologicalStreamSecondsOption});
    parser.process(app);
    ketplus::ThemeManager theme(app);

    const bool multiAppStress = parser.isSet(stressOption);
    const int conversationCount = qBound(1, parser.value(conversationsOption).toInt(), 20);
    const int messagesPerConversation = qBound(1, parser.value(messagesOption).toInt(), 10'000);
    const int pathologicalMiB = qBound(0, parser.value(pathologicalOption).toInt(), 8);
    const int pathologicalStreamHz =
        qBound(0, parser.value(pathologicalStreamRateOption).toInt(), 100);
    const int pathologicalStreamSeconds =
        qBound(1, parser.value(pathologicalStreamSecondsOption).toInt(), 60);
    const int instance = qMax(1, parser.value(instanceOption).toInt());
    const int totalInstances = qMax(instance, parser.value(instancesOption).toInt());
    const QString repositoryPath = QDir::cleanPath(parser.value(repositoryOption));

    QElapsedTimer firstFrameTimer;
    firstFrameTimer.start();
    ketplus::AgentWorkspaceView view;
    if (multiAppStress && QGuiApplication::primaryScreen() != nullptr) {
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        const int columns = totalInstances <= 4 ? 2 : 3;
        const int rows = (totalInstances + columns - 1) / columns;
        const int gutter = 8;
        const int width = available.width() / columns - gutter;
        const int height = available.height() / rows - gutter;
        const int column = (instance - 1) % columns;
        const int row = (instance - 1) / columns;
        view.setGeometry(available.x() + column * available.width() / columns,
                         available.y() + row * available.height() / rows, width, height);
    } else {
        view.resize(900, 700);
    }
    view.applyTheme(theme.palette());
    QVector<ketplus::GitWorktree> stressWorktrees;
    if (multiAppStress) {
        stressWorktrees = makeWorktrees(repositoryPath, conversationCount);
        view.setWorktrees(stressWorktrees, stressWorktrees.constFirst().path);
    } else {
        view.setActivePath(QDir::currentPath());
    }
    if (qEnvironmentVariableIsSet("KETPLUS_BENCHMARK_ALWAYS_ON_TOP"))
        view.setWindowFlag(Qt::WindowStaysOnTopHint);
    view.show();
    // macOS throttles or suppresses scene-graph frames for fully occluded windows.
    // Keep the measured surface foregrounded so scroll FPS reflects rendering,
    // not window-occlusion policy.
    view.raise();
    view.activateWindow();
    const bool firstFrameReady = waitForFrame(view.quickView());
    const double firstFrameMs = firstFrameTimer.nsecsElapsed() / 1'000'000.0;

    if (pathologicalMiB > 0) {
        const QString paragraph = QStringLiteral(
            "## Synthetic result\n\nA [link](https://example.com) with **Markdown**.\n\n");
        QString body;
        body.reserve(pathologicalMiB * 1024 * 1024 + paragraph.size());
        while (body.size() < pathologicalMiB * 1024 * 1024)
            body += paragraph;
        QElapsedTimer populateTimer;
        EventLoopGapProbe gapProbe;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        gapProbe.restartMeasurement();
        populateTimer.start();
        view.resetMessages({QVariantMap{{"messageKey", "pathological"},
                                        {"kind", "assistant"},
                                        {"meta", "Codex"},
                                        {"body", body}}},
                           QStringLiteral("benchmark:pathological"));
        const double synchronousResetMs = populateTimer.nsecsElapsed() / 1'000'000.0;
        const bool usable = waitForUsableViewport(view);
        const double usableViewportMs = populateTimer.nsecsElapsed() / 1'000'000.0;
        const bool rendered = waitForFrame(view.quickView());
        const bool materialized = waitForMaterialization(view);
        const double materializationMs = populateTimer.nsecsElapsed() / 1'000'000.0;
        const double populateMs = populateTimer.nsecsElapsed() / 1'000'000.0;
        if (pathologicalStreamHz > 0) {
            auto* model = view.messageModel();
            int modelResets = 0;
            QObject::connect(model, &QAbstractItemModel::modelReset, &view, [&] { ++modelResets; });
            int emittedDeltas = 0;
            int sampledMaximumOutstanding = model->deferredOutstanding();
            QString streamedBody = body;
            streamedBody.reserve(streamedBody.size() +
                                 pathologicalStreamHz * pathologicalStreamSeconds * 24);
            QElapsedTimer streamTimer;
            QEventLoop streamLoop;
            QTimer stream;
            stream.setTimerType(Qt::PreciseTimer);
            stream.setInterval(1000 / pathologicalStreamHz);
            const int requestedDeltas = pathologicalStreamHz * pathologicalStreamSeconds;
            gapProbe.restartMeasurement();
            QObject::connect(&stream, &QTimer::timeout, &streamLoop, [&] {
                const int due = std::min(requestedDeltas, int(streamTimer.elapsed() *
                                                              qint64(pathologicalStreamHz) / 1000));
                while (emittedDeltas < due) {
                    streamedBody += QStringLiteral("stream-delta:%1\n").arg(emittedDeltas);
                    auto update = model->source(0);
                    update.insert(QStringLiteral("body"), streamedBody);
                    update.insert(QStringLiteral("partialBody"), true);
                    update.insert(QStringLiteral("meta"), QStringLiteral("Codex · streaming"));
                    model->set(0, update);
                    ++emittedDeltas;
                    sampledMaximumOutstanding =
                        std::max(sampledMaximumOutstanding, model->deferredOutstanding());
                }
            });
            QTimer::singleShot(pathologicalStreamSeconds * 1000, &streamLoop, &QEventLoop::quit);
            streamTimer.start();
            stream.start();
            streamLoop.exec();
            stream.stop();
            while (emittedDeltas < requestedDeltas) {
                streamedBody += QStringLiteral("stream-delta:%1\n").arg(emittedDeltas);
                auto update = model->source(0);
                update.insert(QStringLiteral("body"), streamedBody);
                update.insert(QStringLiteral("partialBody"), true);
                update.insert(QStringLiteral("meta"), QStringLiteral("Codex · streaming"));
                model->set(0, update);
                ++emittedDeltas;
                sampledMaximumOutstanding =
                    std::max(sampledMaximumOutstanding, model->deferredOutstanding());
            }
            const double streamWallSeconds = streamTimer.nsecsElapsed() / 1'000'000'000.0;

            streamedBody += QStringLiteral("terminal-flush\n");
            auto terminal = model->source(0);
            terminal.insert(QStringLiteral("body"), streamedBody);
            terminal.insert(QStringLiteral("partialBody"), false);
            terminal.insert(QStringLiteral("meta"), QStringLiteral("Codex"));
            QElapsedTimer terminalTimer;
            terminalTimer.start();
            model->set(0, terminal);
            sampledMaximumOutstanding =
                std::max(sampledMaximumOutstanding, model->deferredOutstanding());
            const bool terminalFlushed = waitUntil(
                [&] { return model->deferredOutstanding() == 0 && !model->materializing(); },
                30000);
            const double visibleTailLatencyMs = terminalTimer.nsecsElapsed() / 1'000'000.0;
            QString renderedBody;
            for (int row = 0; row < model->count(); ++row)
                renderedBody += model->get(row).value(QStringLiteral("body")).toString();
            const bool lossless = renderedBody == streamedBody;
            const auto renderedHead = model->get(0);
            const bool terminalState =
                !renderedHead.value(QStringLiteral("partialBody")).toBool() &&
                renderedHead.value(QStringLiteral("meta")).toString() == QStringLiteral("Codex");
            std::printf("AGENT_PATHOLOGICAL_STREAM_REQUESTED_HZ=%d\n", pathologicalStreamHz);
            std::printf("AGENT_PATHOLOGICAL_STREAM_DURATION_SECONDS=%d\n",
                        pathologicalStreamSeconds);
            std::printf("AGENT_PATHOLOGICAL_STREAM_WALL_SECONDS=%.3f\n", streamWallSeconds);
            std::printf("AGENT_PATHOLOGICAL_STREAM_DELTAS=%d\n", emittedDeltas);
            std::printf("AGENT_PATHOLOGICAL_STREAM_MAX_OUTSTANDING=%d\n",
                        std::max(sampledMaximumOutstanding, model->maximumDeferredOutstanding()));
            std::printf("AGENT_PATHOLOGICAL_STREAM_TERMINAL_FLUSHED=%d\n", terminalFlushed ? 1 : 0);
            std::printf("AGENT_PATHOLOGICAL_STREAM_TERMINAL_STATE=%d\n", terminalState ? 1 : 0);
            std::printf("AGENT_PATHOLOGICAL_STREAM_LOSSLESS=%d\n", lossless ? 1 : 0);
            std::printf("AGENT_PATHOLOGICAL_STREAM_MODEL_RESETS=%d\n", modelResets);
            std::printf("AGENT_PATHOLOGICAL_STREAM_VISIBLE_TAIL_LATENCY_MS=%.3f\n",
                        visibleTailLatencyMs);
            std::printf("AGENT_PATHOLOGICAL_STREAM_MAX_UI_GAP_MS=%.3f\n", gapProbe.maximumGapMs());
            std::printf("AGENT_PATHOLOGICAL_STREAM_MAX_RSS_KIB=%d\n", maximumResidentSetKiB());
            return terminalFlushed && terminalState && lossless && modelResets == 0 &&
                           model->maximumDeferredOutstanding() <= 2
                       ? 0
                       : 3;
        }
        const TimedRun scroll = runInterval(view.quickView(), 2000, [&](const double progress) {
            view.setBenchmarkScrollProgress(progress);
        });
        std::printf("AGENT_PATHOLOGICAL_SOURCE_MIB=%d\n", pathologicalMiB);
        std::printf("AGENT_PATHOLOGICAL_BLOCK_ROWS=%d\n", view.benchmarkMessageCount());
        std::printf("AGENT_PATHOLOGICAL_FIRST_FRAME_READY=%d\n", rendered ? 1 : 0);
        std::printf("AGENT_PATHOLOGICAL_USABLE_VIEWPORT_READY=%d\n", usable ? 1 : 0);
        std::printf("AGENT_PATHOLOGICAL_MATERIALIZED=%d\n", materialized ? 1 : 0);
        std::printf("AGENT_PATHOLOGICAL_SYNC_RESET_MS=%.3f\n", synchronousResetMs);
        std::printf("AGENT_PATHOLOGICAL_USABLE_VIEWPORT_MS=%.3f\n", usableViewportMs);
        std::printf("AGENT_PATHOLOGICAL_MATERIALIZATION_MS=%.3f\n", materializationMs);
        std::printf("AGENT_PATHOLOGICAL_MAX_UI_GAP_MS=%.3f\n", gapProbe.maximumGapMs());
        std::printf("AGENT_PATHOLOGICAL_POPULATE_MS=%.3f\n", populateMs);
        std::printf("AGENT_PATHOLOGICAL_SCROLL_CPU_PERCENT=%.3f\n", scroll.cpuPercent);
        std::printf("AGENT_PATHOLOGICAL_SCROLL_FPS=%.3f\n",
                    scroll.wallSeconds > 0.0 ? scroll.frames / scroll.wallSeconds : 0.0);
        std::printf("AGENT_PATHOLOGICAL_MAX_RSS_KIB=%d\n", maximumResidentSetKiB());
        return rendered ? 0 : 2;
    }

    if (multiAppStress) {
        QElapsedTimer populateTimer;
        populateTimer.start();
        QVector<QVariantList> residentConversations;
        QVariantList conversationSummaries;
        residentConversations.reserve(conversationCount);
        for (int index = 0; index < conversationCount; ++index) {
            residentConversations.append(makeConversation(index, messagesPerConversation));
            conversationSummaries.append(QVariantMap{
                {QStringLiteral("id"), QStringLiteral("benchmark-%1").arg(index)},
                {QStringLiteral("title"), QStringLiteral("Conversation %1").arg(index + 1)},
                {QStringLiteral("status"), QStringLiteral("Ready")},
                {QStringLiteral("running"), false},
                {QStringLiteral("active"), index == 0}});
        }
        view.setConversations(conversationSummaries);
        view.resetMessages(residentConversations.constFirst(), QStringLiteral("benchmark:0"));
        waitForFrame(view.quickView());
        const double populateMs = populateTimer.nsecsElapsed() / 1'000'000.0;

        runInterval(view.quickView(), 1500);
        const TimedRun idle = runInterval(view.quickView(), 3000);
        int selectedConversation = 0;
        const auto selectConversation = [&](const int next) {
            if (next == selectedConversation) {
                return;
            }
            QVariantMap previous = conversationSummaries.at(selectedConversation).toMap();
            previous.insert(QStringLiteral("active"), false);
            conversationSummaries[selectedConversation] = previous;
            selectedConversation = next;
            QVariantMap active = conversationSummaries.at(next).toMap();
            active.insert(QStringLiteral("active"), true);
            conversationSummaries[next] = active;
            view.setConversations(conversationSummaries);
            view.setActivePath(stressWorktrees.at(next).path);
            view.resetMessages(residentConversations.at(next),
                               QStringLiteral("benchmark:%1").arg(next));
        };
        const TimedRun switching = runInterval(view.quickView(), 3000, [&](const double progress) {
            selectConversation(
                qMin(conversationCount - 1, static_cast<int>(progress * conversationCount)));
        });
        const TimedRun warmSwitching =
            runInterval(view.quickView(), 3000, [&](const double progress) {
                const int visit = static_cast<int>(progress * conversationCount * 2);
                selectConversation(visit % conversationCount);
            });
        const TimedRun scroll = runInterval(view.quickView(), 3000, [&](const double progress) {
            const double cycle = std::fmod(progress * 6.0, 2.0);
            view.setBenchmarkScrollProgress(cycle <= 1.0 ? cycle : 2.0 - cycle);
        });

        std::printf("AGENT_STRESS_INSTANCE=%d\n", instance);
        std::printf("AGENT_QTQUICK_GRAPHICS_API=%s\n",
                    graphicsApiName(view.quickView()->rendererInterface()->graphicsApi()));
        std::printf("AGENT_FIRST_FRAME_READY=%d\n", firstFrameReady ? 1 : 0);
        std::printf("AGENT_FIRST_FRAME_MS=%.3f\n", firstFrameMs);
        std::printf("AGENT_STRESS_CONVERSATIONS=%d\n", conversationCount);
        std::printf("AGENT_STRESS_MESSAGES_PER_CONVERSATION=%d\n", messagesPerConversation);
        std::printf("AGENT_STRESS_TOTAL_RESIDENT_MESSAGES=%d\n",
                    conversationCount * messagesPerConversation);
        std::printf("AGENT_STRESS_WARM_TRANSCRIPTS=%d\n", view.benchmarkWarmTranscriptCount());
        std::printf("AGENT_STRESS_POPULATE_MS=%.3f\n", populateMs);
        std::printf("AGENT_STRESS_IDLE_CPU_PERCENT=%.3f\n", idle.cpuPercent);
        std::printf("AGENT_STRESS_IDLE_FRAMES=%d\n", idle.frames);
        std::printf("AGENT_STRESS_SWITCH_CPU_PERCENT=%.3f\n", switching.cpuPercent);
        std::printf("AGENT_STRESS_SWITCH_FRAMES=%d\n", switching.frames);
        std::printf("AGENT_STRESS_WARM_SWITCH_CPU_PERCENT=%.3f\n", warmSwitching.cpuPercent);
        std::printf("AGENT_STRESS_WARM_SWITCH_FRAMES=%d\n", warmSwitching.frames);
        std::printf("AGENT_STRESS_SCROLL_CPU_PERCENT=%.3f\n", scroll.cpuPercent);
        std::printf("AGENT_STRESS_SCROLL_FPS=%.3f\n",
                    scroll.wallSeconds > 0.0 ? scroll.frames / scroll.wallSeconds : 0.0);
        std::printf("AGENT_STRESS_MAX_RSS_KIB=%d\n", maximumResidentSetKiB());
        return firstFrameReady ? 0 : 2;
    }

    QElapsedTimer populateTimer;
    populateTimer.start();
    view.prepareBenchmarkMessages(1000);
    waitForFrame(view.quickView());
    const double populateMs = populateTimer.nsecsElapsed() / 1'000'000.0;

    // Let transient scrollbar/style animations finish before measuring steady idle.
    runInterval(view.quickView(), 2500);
    const TimedRun idle = runInterval(view.quickView(), 2000);
    const TimedRun scroll = runInterval(view.quickView(), 2000, [&](const double progress) {
        const double cycle = std::fmod(progress * 4.0, 2.0);
        view.setBenchmarkScrollProgress(cycle <= 1.0 ? cycle : 2.0 - cycle);
    });

    QElapsedTimer splitResizeTimer;
    splitResizeTimer.start();
    view.resize(520, 700);
    waitForFrame(view.quickView());
    const double splitResizeMs = splitResizeTimer.nsecsElapsed() / 1'000'000.0;
    runInterval(view.quickView(), 1000);
    const TimedRun splitScroll = runInterval(view.quickView(), 2000, [&](const double progress) {
        const double cycle = std::fmod(progress * 4.0, 2.0);
        view.setBenchmarkScrollProgress(cycle <= 1.0 ? cycle : 2.0 - cycle);
    });

    std::printf("AGENT_QTQUICK_GRAPHICS_API=%s\n",
                graphicsApiName(view.quickView()->rendererInterface()->graphicsApi()));
    std::printf("AGENT_FIRST_FRAME_READY=%d\n", firstFrameReady ? 1 : 0);
    std::printf("AGENT_FIRST_FRAME_MS=%.3f\n", firstFrameMs);
    std::printf("AGENT_POPULATE_1000_MS=%.3f\n", populateMs);
    std::printf("AGENT_IDLE_CPU_PERCENT=%.3f\n", idle.cpuPercent);
    std::printf("AGENT_IDLE_FRAMES=%d\n", idle.frames);
    std::printf("AGENT_SCROLL_CPU_PERCENT=%.3f\n", scroll.cpuPercent);
    std::printf("AGENT_SCROLL_FRAMES=%d\n", scroll.frames);
    std::printf("AGENT_SCROLL_FPS=%.3f\n",
                scroll.wallSeconds > 0.0 ? scroll.frames / scroll.wallSeconds : 0.0);
    std::printf("AGENT_SPLIT_RESIZE_MS=%.3f\n", splitResizeMs);
    std::printf("AGENT_SPLIT_SCROLL_CPU_PERCENT=%.3f\n", splitScroll.cpuPercent);
    std::printf("AGENT_SPLIT_SCROLL_FRAMES=%d\n", splitScroll.frames);
    std::printf("AGENT_SPLIT_SCROLL_FPS=%.3f\n",
                splitScroll.wallSeconds > 0.0 ? splitScroll.frames / splitScroll.wallSeconds : 0.0);
    return firstFrameReady ? 0 : 2;
}
