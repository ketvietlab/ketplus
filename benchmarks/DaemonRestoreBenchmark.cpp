#include "agent/AgentSessionManager.h"
#include "service/DaemonService.h"
#include "storage/LocalJournal.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

using namespace ketplus;
using namespace ketplus::daemon;

namespace {

constexpr int turnCount = 300;
constexpr int deltasPerTurn = 20;
constexpr int compactPageSize = 128;

bool populateJournal(const QString& state, const QString& worktree) {
    LocalJournal journal(state);
    if (!journal.isOpen() || !journal.upsertSession({QStringLiteral("benchmark-session"),
                                                     worktree,
                                                     QStringLiteral("codex"),
                                                     QStringLiteral("benchmark-thread"),
                                                     {},
                                                     QStringLiteral("ready"),
                                                     1,
                                                     0})) {
        return false;
    }
    for (int turn = 0; turn < turnCount; ++turn) {
        const QString turnId = QStringLiteral("turn-%1").arg(turn);
        const QString itemId = QStringLiteral("answer-%1").arg(turn);
        if (journal.appendEvent(
                QStringLiteral("benchmark-session"), 1, QStringLiteral("user.message"),
                {{QStringLiteral("text"), QStringLiteral("Synthetic prompt %1").arg(turn)}}) < 0 ||
            journal.appendEvent(QStringLiteral("benchmark-session"), 1,
                                QStringLiteral("turn.started"),
                                {{QStringLiteral("turn_id"), turnId}}) < 0) {
            return false;
        }
        for (int delta = 0; delta < deltasPerTurn; ++delta) {
            if (journal.appendEvent(
                    QStringLiteral("benchmark-session"), 1, QStringLiteral("assistant.delta"),
                    {{QStringLiteral("item_id"), itemId},
                     {QStringLiteral("delta"), QStringLiteral("streamed token ")}}) < 0) {
                return false;
            }
        }
        if (journal.appendEvent(
                QStringLiteral("benchmark-session"), 1, QStringLiteral("item.completed"),
                {{QStringLiteral("item"),
                  QJsonObject{{QStringLiteral("id"), itemId},
                              {QStringLiteral("type"), QStringLiteral("agentMessage")},
                              {QStringLiteral("text"),
                               QStringLiteral("Synthetic final answer %1").arg(turn)}}}}) < 0 ||
            journal.appendEvent(QStringLiteral("benchmark-session"), 1,
                                QStringLiteral("turn.completed"),
                                {{QStringLiteral("status"), QStringLiteral("completed")}}) < 0) {
            return false;
        }
    }
    return true;
}

int compactedEventCount(const QString& state, int* pageCount, qint64* lastCursor) {
    LocalJournal journal(state);
    const auto sessions = journal.sessions();
    if (sessions.isEmpty()) {
        return -1;
    }
    const StoredSession& session = sessions.constFirst();
    *lastCursor = session.lastCursor;
    int eventCount = 0;
    int pages = 0;
    qint64 cursor = 0;
    while (cursor < session.lastCursor) {
        const EventReplayPage page =
            journal.compactedEventsAfter(session.id, session.generation, cursor, compactPageSize);
        if (page.nextCursor <= cursor) {
            return -1;
        }
        cursor = page.nextCursor;
        eventCount += page.events.size();
        ++pages;
    }
    *pageCount = pages;
    return eventCount;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("KetPlusBenchmark"));
    QCoreApplication::setApplicationName(QStringLiteral("DaemonRestore"));
    QSettings().clear();

    QTemporaryDir directory;
    if (!directory.isValid()) {
        return 2;
    }
    const QString worktree = directory.filePath(QStringLiteral("worktree"));
    if (!QDir().mkpath(worktree)) {
        return 2;
    }
    const QString state = directory.filePath(QStringLiteral("journal.sqlite3"));
    const QString socket = directory.filePath(QStringLiteral("daemon.sock"));
    const QString token = directory.filePath(QStringLiteral("local.token"));
    if (!populateJournal(state, worktree)) {
        return 2;
    }

    int replayPages = 0;
    qint64 rawEvents = 0;
    const int compactedEvents = compactedEventCount(state, &replayPages, &rawEvents);
    if (compactedEvents < 0) {
        return 2;
    }

    qputenv("KETPLUS_DAEMON_SOCKET", socket.toUtf8());
    qputenv("KETPLUS_DAEMON_STATE", state.toUtf8());
    qputenv("KETPLUS_DAEMON_TOKEN_FILE", token.toUtf8());
    qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");

    DaemonService service(
        {state, token, socket, QStringLiteral("/nonexistent/ketplus-benchmark-codex")});
    if (!service.start()) {
        return 2;
    }

    QElapsedTimer timer;
    timer.start();
    AgentSessionManager desktop;
    desktop.activate(worktree);
    const int expectedMessages = turnCount * 2;
    while (desktop.messages(worktree).size() < expectedMessages && timer.elapsed() < 10'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    const qint64 restoreMs = timer.elapsed();
    const int restoredMessages = desktop.messages(worktree).size();

    std::printf("DAEMON_RESTORE_RAW_EVENTS=%lld\n", static_cast<long long>(rawEvents));
    std::printf("DAEMON_RESTORE_COMPACTED_EVENTS=%d\n", compactedEvents);
    std::printf("DAEMON_RESTORE_REPLAY_PAGES=%d\n", replayPages);
    std::printf("DAEMON_RESTORE_MESSAGES=%d\n", restoredMessages);
    std::printf("DAEMON_RESTORE_MS=%lld\n", static_cast<long long>(restoreMs));

    QSettings().clear();
    qunsetenv("KETPLUS_DAEMON_SOCKET");
    qunsetenv("KETPLUS_DAEMON_STATE");
    qunsetenv("KETPLUS_DAEMON_TOKEN_FILE");
    qunsetenv("KETPLUS_DISABLE_DAEMON_AUTOSTART");
    return restoredMessages == expectedMessages ? 0 : 2;
}
