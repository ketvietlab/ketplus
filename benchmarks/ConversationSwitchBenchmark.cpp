#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentTranscriptPaging.h"
#include "agent/CodexAppServer.h"
#include "agent/DaemonTranscriptProjection.h"
#include "agent/TranscriptRowMeasurer.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <cstdio>
#include <ctime>
using namespace ketplus;
using namespace ketplus::daemon;
static void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
static bool until(const std::function<bool()>& ready, int ms = 15000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < ms)
        spin(1);
    return ready();
}
static double cpu() { return double(std::clock()) / CLOCKS_PER_SEC; }
static QString body(int kib) {
    QString text;
    for (int n = 0; text.size() < kib * 1024; ++n)
        text += QString("## Section %1\n\nThis is a **synthetic conversation** about application "
                        "architecture, performance, and correctness. A [local "
                        "link](https://example.invalid/) stays within the text.\n\n- The first "
                        "item describes the source.\n- The second item describes the result.\n\n")
                    .arg(n);
    return text;
}
static QVariantList rows(int conversation, int count, int kib) {
    QVariantList result;
    for (int i = 0; i < count; ++i) {
        bool user = i % 2 == 0;
        result.append(
            QVariantMap{{"kind", user ? "user" : "assistant"},
                        {"body", user ? QString("Synthetic prompt %1").arg(i) : body(kib)},
                        {"messageKey", QString("c%1-row%2").arg(conversation).arg(i)},
                        {"itemId", QString("item%1").arg(i)},
                        {"turnId", QString("turn%1").arg(i / 2)},
                        {"meta", user ? "You" : "Codex"}});
    }
    return result;
}
static void output(const QJsonObject& o) {
    std::puts(QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
}
int main(int argc, char** argv) {

    QGuiApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    app.setOrganizationName("KetPlusDiagnostic");
    app.setApplicationName("ConversationSwitchSynthetic");
    const QString mode = argc > 1 ? argv[1] : "history";
    const int kib = argc > 2 ? QString(argv[2]).toInt() : 8;
    if (mode == "measure") {
        QObject context;
        TranscriptRowMeasurer measurer;
        QFont font("Arial");
        font.setPixelSize(16);
        measurer.setGeometry(760, font);
        const auto source = rows(0, 60, kib);
        QJsonArray runs;
        for (int i = 0; i < 4; ++i) {
            bool done = false;
            QElapsedTimer timer;
            timer.start();
            const auto startCpu = cpu();
            TranscriptRowMeasurer::premeasure(source, &context, [&] { done = true; });
            if (!until([&] { return done; }))
                return 2;
            runs.append(QJsonObject{{"wall_ms", timer.nsecsElapsed() / 1e6},
                                    {"cpu_ms", (cpu() - startCpu) * 1000}});
        }
        output({{"mode", mode}, {"body_kib", kib}, {"same_page_repeated", runs}});
    } else {
        const int pages =
            argc > 3 ? QString(argv[3]).toInt() : transcript_paging::pageCounts().initial;
        transcript_paging::setPageCounts({pages, 1, true, false});
        QTemporaryDir tmp;
        const QString path = QFileInfo(tmp.path()).canonicalFilePath();
        {
            LocalJournal journal(tmp.filePath("journal.sqlite3"));
            for (int c = 0; c < 2; ++c) {
                auto id = QString("synthetic-%1").arg(c);
                journal.upsertSession({id, path, "codex", {}, {}, "ready", 1, 0, 0, id});
                for (int t = 0; t < 90; ++t) {
                    journal.appendEvent(id, 1, "user.message",
                                        {{"text", QString("prompt-%1").arg(t)}});
                    journal.appendEvent(id, 1, "item.completed",
                                        {{"item", QJsonObject{{"id", QString::number(t)},
                                                              {"type", "agentMessage"},
                                                              {"phase", "final_answer"},
                                                              {"text", body(kib)}}}});
                    journal.appendEvent(id, 1, "turn.completed", {{"status", "completed"}});
                }
            }
            QElapsedTimer directTimer;
            directTimer.start();
            const auto directPage = journal.transcriptWindow("synthetic-0", 1, 0, -1, -1,
                                                             transcript_paging::minimumTurns, 128);
            const double storageMs = directTimer.nsecsElapsed() / 1e6;
            directTimer.restart();
            const auto projected = agent_message::daemonWindowMessages(
                {{"events", directPage.events}, {"partial_items", directPage.partialItems}});
            const double projectionMs = directTimer.nsecsElapsed() / 1e6;
            directTimer.restart();
            int fragments = 0;
            for (const auto& value : directPage.events) {
                const auto reference =
                    value.toObject().value("payload").toObject().value("large_event").toObject();
                const auto transferId = reference.value("transfer_id").toString();
                if (transferId.isEmpty())
                    continue;
                qint64 offset = 0;
                while (offset < reference.value("total_bytes").toInteger()) {
                    const auto fragment = journal.largeEventFragment("synthetic-0", 1, transferId,
                                                                     offset, 128 * 1024);
                    if (!fragment.errorCode.isEmpty())
                        return 3;
                    offset = fragment.nextOffset;
                    ++fragments;
                }
            }
            const double fragmentStorageMs = directTimer.nsecsElapsed() / 1e6;
            output({{"mode", "history-direct"},
                    {"body_kib", kib},
                    {"storage_ms", storageMs},
                    {"projection_ms", projectionMs},
                    {"fragment_storage_ms", fragmentStorageMs},
                    {"fragments", fragments},
                    {"events", directPage.events.size()},
                    {"messages", projected.size()}});
        }
        qputenv("KETPLUS_DAEMON_SOCKET", tmp.filePath("daemon.sock").toUtf8());
        qputenv("KETPLUS_DAEMON_STATE", tmp.filePath("journal.sqlite3").toUtf8());
        qputenv("KETPLUS_DAEMON_TOKEN_FILE", tmp.filePath("local.token").toUtf8());
        qputenv("KETPLUS_DAEMON_EXECUTABLE", QCoreApplication::applicationFilePath().toUtf8());
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        QSettings().setValue(
            agent_message::persistedKey(path),
            QVariantMap{{"path", path},
                        {"activeId", "synthetic-0"},
                        {"conversations", QVariantList{QVariantMap{{"id", "synthetic-0"},
                                                                   {"threadId", "synthetic-0"},
                                                                   {"title", "Synthetic"}}}}});
        DaemonService service({tmp.filePath("journal.sqlite3"), tmp.filePath("local.token"),
                               tmp.filePath("daemon.sock"), "/nonexistent/diagnostic-provider"});
        if (!service.start())
            return 2;
        TranscriptRowMeasurer measurer;
        QFont font("Arial");
        font.setPixelSize(16);
        measurer.setGeometry(760, font);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        QElapsedTimer timer;
        double firstLatestMs = -1;
        double latestResetMs = -1;
        QObject::connect(&desktop, &AgentSessionManager::messagesReset, &desktop,
                         [&](const QString& worktree, const QVariantList& messages) {
                             if (worktree != path || messages.isEmpty())
                                 return;
                             latestResetMs = timer.nsecsElapsed() / 1e6;
                             if (firstLatestMs < 0)
                                 firstLatestMs = latestResetMs;
                         });
        timer.start();
        const auto startCpu = cpu();
        desktop.activate(path);
        if (!until([&] {
                return desktop.messages(path).size() >= 6 &&
                       !desktop.historyState(path).value("loading").toBool();
            }))
            return 2;
        output({{"mode", "history-initial"},
                {"pages", pages},
                {"body_kib", kib},
                {"wall_ms", timer.nsecsElapsed() / 1e6},
                {"first_latest_ms", firstLatestMs},
                {"cpu_ms", (cpu() - startCpu) * 1000},
                {"messages", desktop.messages(path).size()},
                {"batch", QJsonObject::fromVariantMap(
                              desktop.historyState(path).value("lastBatch").toMap())}});
        timer.restart();
        firstLatestMs = -1;
        latestResetMs = -1;
        auto start = cpu();
        desktop.selectConversation(path, "synthetic-1");
        if (!until([&] {
                return desktop.messages(path).size() >= 6 &&
                       !desktop.historyState(path).value("loading").toBool();
            }))
            return 2;
        output({{"mode", "history-switch-cold"},
                {"pages", pages},
                {"wall_ms", timer.nsecsElapsed() / 1e6},
                {"first_latest_ms", firstLatestMs},
                {"cpu_ms", (cpu() - start) * 1000},
                {"messages", desktop.messages(path).size()},
                {"batch", QJsonObject::fromVariantMap(
                              desktop.historyState(path).value("lastBatch").toMap())}});
        timer.restart();
        firstLatestMs = -1;
        latestResetMs = -1;
        desktop.selectConversation(path, "synthetic-0");
        if (!until([&] {
                return desktop.messages(path).size() >= 6 &&
                       !desktop.historyState(path).value("loading").toBool();
            }))
            return 2;
        output({{"mode", "history-return"},
                {"wall_ms", timer.nsecsElapsed() / 1e6},
                {"cached_visible_ms", firstLatestMs},
                {"refreshed_latest_ms", latestResetMs},
                {"messages", desktop.messages(path).size()}});
        QSettings().remove(agent_message::persistedKey(path));
    }
    QThreadPool::globalInstance()->waitForDone();
}
