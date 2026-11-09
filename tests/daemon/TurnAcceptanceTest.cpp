#include "FakeCodex.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <limits>

using namespace ketplus::daemon;

namespace {
QJsonObject request(const QString& messageId, const QString& sessionId) {
    return {
        {"protocol_version", protocol::currentVersion},
        {"message_id", messageId},
        {"message_type", "session.start_turn"},
        {"session_id", sessionId},
        {"session_generation", 1},
        {"sent_at", protocol::timestamp()},
        {"payload", QJsonObject{{"prompt", "synthetic prompt"}, {"client_message_id", messageId}}}};
}
QSqlDatabase journalConnection(const QString& path) {
    for (const auto& name : QSqlDatabase::connectionNames())
        if (name.startsWith("ketplus-daemon-") &&
            QSqlDatabase::database(name).databaseName() == path)
            return QSqlDatabase::database(name);
    return {};
}
int eventCount(const QJsonArray& events, const QString& type) {
    return std::count_if(events.begin(), events.end(), [&](const QJsonValue& value) {
        return value.toObject().value("event_type") == type;
    });
}
} // namespace

class TurnAcceptanceTest final : public QObject {
    Q_OBJECT
  private slots:
    void failureRollsBackAcceptedPair_data() {
        QTest::addColumn<QString>("fault");
        for (const auto& fault :
             {"prompt-abort", "prompt-ignore", "queued-abort", "queued-ignore", "projection-abort",
              "projection-ignore", "state-abort", "state-ignore", "config-state-abort",
              "config-state-ignore", "config-event-abort", "config-event-ignore",
              "config-exhaustion", "commit"})
            QTest::newRow(fault) << QString(fault);
    }
    void failureRollsBackAcceptedPair() {
        QFETCH(QString, fault);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite3");
        const auto executable = dir.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, dir.filePath("provider.log")));
        LocalJournal journal(path);
        StoredSession seed;
        seed.id = "atomic-accept";
        seed.generation = 1;
        seed.worktreePath = dir.path();
        seed.adapter = "codex";
        seed.state = "idle";
        if (fault == "config-exhaustion")
            seed.configurationRevision = std::numeric_limits<qint64>::max();
        QVERIFY(journal.upsertSession(seed));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stop = qScopeGuard([&] { adapter.stop(); });
        QSignalSpy events(&host, &SessionHost::eventReady);
        auto db = journalConnection(path);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QString table = "events";
        QString condition = "NEW.event_type='turn.queued'";
        QString action = fault.endsWith("ignore")
                             ? "SELECT RAISE(IGNORE);"
                             : "SELECT RAISE(ABORT,'synthetic_acceptance_failure');";
        QString operation = "INSERT";
        if (fault.startsWith("prompt"))
            condition = "NEW.event_type='user.message'";
        if (fault.startsWith("projection"))
            table = "replay_events";
        if (fault.startsWith("state")) {
            table = "sessions";
            operation = "UPDATE";
            condition = "NEW.state='starting'";
        }
        if (fault.startsWith("config-state")) {
            table = "sessions";
            operation = "UPDATE";
            condition = "NEW.model='synthetic-model'";
        }
        if (fault.startsWith("config-event"))
            condition = "NEW.event_type='session.configuration_changed'";
        if (fault == "commit") {
            QVERIFY(q.exec("PRAGMA foreign_keys=ON"));
            QVERIFY(q.exec("CREATE TABLE acceptance_parent(id INTEGER PRIMARY KEY)"));
            QVERIFY(q.exec("CREATE TABLE acceptance_child(id INTEGER REFERENCES "
                           "acceptance_parent(id) DEFERRABLE INITIALLY DEFERRED)"));
            action = "INSERT INTO acceptance_child VALUES(99);";
        }
        QVERIFY2(q.exec("CREATE TRIGGER acceptance_fault BEFORE " + operation + " ON " + table +
                        " WHEN " + condition + " BEGIN " + action + " END"),
                 qPrintable(q.lastError().text()));
        const auto repair = qScopeGuard([&] {
            QSqlQuery cleanup(db);
            cleanup.exec("DROP TRIGGER IF EXISTS acceptance_fault");
        });
        auto submitted = request("rejected", seed.id);
        if (fault.startsWith("config-")) {
            auto payload = submitted.value("payload").toObject();
            payload.insert("options", QJsonObject{{"model", "synthetic-model"}});
            submitted.insert("payload", payload);
        }
        const auto failed = host.handleCommand(submitted, "desktop-test");
        QVERIFY(!failed.value("ok").toBool());
        QCOMPARE(failed.value("error_code").toString(), QString("journal_error"));
        const auto persisted = journal.eventsAfter(seed.id, 1, 0);
        QCOMPARE(eventCount(persisted, "user.message"), 0);
        QCOMPARE(eventCount(persisted, "turn.queued"), 0);
        for (const auto& arguments : events) {
            const auto type = arguments.first().toJsonObject().value("event_type").toString();
            QVERIFY(type != "user.message" && type != "turn.queued");
        }
        const auto after = journal.sessions().first();
        QCOMPARE(after.state, QString("idle"));
        QCOMPARE(after.lastCursor, qint64(0));
        QCOMPARE(after.currentTurnSequence, qint64(0));
        QCOMPARE(after.model, seed.model);
        QCOMPARE(after.configurationRevision, seed.configurationRevision);
        QVERIFY(after.lastTurnStartedAt.isEmpty());
        QVERIFY(q.exec("SELECT count(*) FROM replay_events"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
        q.finish();
        if (fault == "commit") {
            QVERIFY(q.exec("SELECT count(*) FROM acceptance_child"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 0);
            q.finish();
        }
        QVERIFY(!QFileInfo::exists(dir.filePath("provider.log")));
        QVERIFY(q.exec("DROP TRIGGER acceptance_fault"));
        {
            LocalJournal reopened(path);
            QVERIFY(reopened.isOpen());
            QCOMPARE(reopened.eventsAfter(seed.id, 1, 0).size(), 0);
            QCOMPARE(reopened.sessions().first().currentTurnSequence, qint64(0));
        }
        const auto accepted = host.handleCommand(request("repaired", seed.id), "desktop-test");
        QVERIFY2(accepted.value("ok").toBool(), qPrintable(accepted.value("error").toString()));
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(journal.eventsAfter(seed.id, 1, 0), "turn.completed"),
                                  1, 10000);
        const auto history = journal.eventsAfter(seed.id, 1, 0);
        QCOMPARE(eventCount(history, "user.message"), 1);
        QCOMPARE(eventCount(history, "turn.queued"), 1);
        QCOMPARE(history.first().toObject().value("payload").toObject().value("client_message_id"),
                 QJsonValue("repaired"));
        QCOMPARE(journal.sessions().first().currentTurnSequence, qint64(1));
        const auto duplicate = host.handleCommand(request("repaired", seed.id), "desktop-test");
        QVERIFY(duplicate.value("ok").toBool());
        QCOMPARE(eventCount(journal.eventsAfter(seed.id, 1, 0), "user.message"), 1);
        QVERIFY(q.exec("PRAGMA integrity_check"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("ok"));
    }

    void boundedAcceptance_data() {
        QTest::addColumn<QString>("fault");
        for (const auto& fault : {"cursor-full", "cursor-one-slot", "cursor-two-slots",
                                  "cursor-three-slots", "configuration-needs-three", "turn-full",
                                  "oversized-prompt", "oversized-configuration"})
            QTest::newRow(fault) << QString(fault);
    }
    void boundedAcceptance() {
        QFETCH(QString, fault);
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite3"));
        StoredSession seed;
        seed.id = "bounded";
        seed.worktreePath = dir.path();
        seed.adapter = "codex";
        seed.state = "idle";
        const qint64 max = std::numeric_limits<qint64>::max();
        if (fault == "cursor-full")
            seed.lastCursor = max;
        if (fault == "cursor-one-slot")
            seed.lastCursor = max - 1;
        if (fault == "cursor-two-slots" || fault == "configuration-needs-three")
            seed.lastCursor = max - 2;
        if (fault == "cursor-three-slots")
            seed.lastCursor = max - 3;
        if (fault == "turn-full")
            seed.currentTurnSequence = max;
        // Seed valid context metadata before setting synthetic extreme counters.
        const auto wantedCursor = seed.lastCursor;
        const auto wantedTurn = seed.currentTurnSequence;
        seed.lastCursor = 0;
        seed.currentTurnSequence = 0;
        QVERIFY(journal.upsertSession(seed));
        QSqlQuery q(journalConnection(dir.filePath("journal.sqlite3")));
        q.prepare("UPDATE sessions SET last_cursor=?,current_turn_sequence=? WHERE id='bounded'");
        q.addBindValue(wantedCursor);
        q.addBindValue(wantedTurn);
        QVERIFY(q.exec());
        q.finish();
        seed.lastCursor = wantedCursor;
        seed.currentTurnSequence = wantedTurn;
        auto accepted = seed;
        accepted.state = "starting";
        QJsonObject prompt{{"text", "synthetic"}};
        QJsonObject config;
        if (fault == "configuration-needs-three" || fault == "cursor-three-slots")
            config = {{"revision", 2}};
        if (fault == "oversized-prompt")
            prompt.insert("text", QString(protocol::maximumEventPayloadBytes + 1, 'p'));
        if (fault == "oversized-configuration")
            config = {{"unexpected", QString(protocol::maximumEventPayloadBytes + 1, 'c')}};
        QString error;
        const auto events = journal.appendTurnStart(accepted, prompt, config, &error);
        const bool success = fault == "cursor-two-slots" || fault == "cursor-three-slots";
        QCOMPARE(!events.isEmpty(), success);
        if (success) {
            QVERIFY(error.isEmpty());
            QCOMPARE(events.size(), config.isEmpty() ? 2 : 3);
            QCOMPARE(journal.sessions().first().lastCursor, max);
            QCOMPARE(journal.sessions().first().currentTurnSequence, qint64(1));
            QVERIFY(journal.appendEvent(seed.id, 1, "turn.completed", {}, &error) < 0);
        } else {
            QVERIFY(!error.isEmpty());
            QCOMPARE(journal.eventsAfter(seed.id, 1, 0).size(), 0);
            const auto current = journal.sessions().first();
            QCOMPARE(current.lastCursor, wantedCursor);
            QCOMPARE(current.currentTurnSequence, wantedTurn);
            QCOMPARE(current.state, QString("idle"));
            auto repair = seed;
            repair.lastCursor = 0;
            repair.currentTurnSequence = 0;
            QVERIFY(q.exec(
                "UPDATE sessions SET last_cursor=0,current_turn_sequence=0 WHERE id='bounded'"));
            error.clear();
            QCOMPARE(journal.appendTurnStart(repair, {{"text", "repaired"}}, {}, &error).size(), 2);
            QVERIFY(error.isEmpty());
        }
    }

    void firstPublishedEventSeesCommittedPair_data() {
        QTest::addColumn<bool>("configuration");
        QTest::newRow("unchanged") << false;
        QTest::newRow("changed") << true;
    }
    void firstPublishedEventSeesCommittedPair() {
        QFETCH(bool, configuration);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite3");
        const auto executable = dir.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, dir.filePath("provider.log")));
        LocalJournal journal(path);
        StoredSession seed;
        seed.id = "publication";
        seed.generation = 1;
        seed.worktreePath = dir.path();
        seed.adapter = "codex";
        seed.state = "idle";
        QVERIFY(journal.upsertSession(seed));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stop = qScopeGuard([&] { adapter.stop(); });
        const QString readerName = "acceptance-reader";
        const auto removeReader = qScopeGuard([&] { QSqlDatabase::removeDatabase(readerName); });
        auto reader = QSqlDatabase::addDatabase("QSQLITE", readerName);
        reader.setDatabaseName(path);
        QVERIFY(reader.open());
        int promptCallbacks = 0;
        connect(&host, &SessionHost::eventReady, this, [&](const QJsonObject& event) {
            if (event.value("event_type") != "user.message")
                return;
            ++promptCallbacks;
            QSqlQuery query(reader);
            QVERIFY(query.exec(
                "SELECT event_type FROM events WHERE session_id='publication' ORDER BY cursor"));
            QStringList types;
            while (query.next())
                types.append(query.value(0).toString());
            QStringList expected{"user.message", "turn.queued"};
            if (configuration)
                expected.append("session.configuration_changed");
            QCOMPARE(types, expected);
        });
        auto submitted = request("accepted", seed.id);
        if (configuration) {
            auto payload = submitted.value("payload").toObject();
            payload.insert("options", QJsonObject{{"model", "synthetic-model"}});
            submitted.insert("payload", payload);
        }
        const auto accepted = host.handleCommand(submitted, "desktop-test");
        QVERIFY(accepted.value("ok").toBool());
        QCOMPARE(promptCallbacks, 1);
        const auto history = journal.eventsAfter(seed.id, 1, 0);
        QCOMPARE(history.size(), configuration ? 3 : 2);
        QCOMPARE(history.at(0).toObject().value("turn_sequence"),
                 history.at(1).toObject().value("turn_sequence"));
        QCOMPARE(history.at(0).toObject().value("payload").toObject().value("context_revision"),
                 history.at(1).toObject().value("payload").toObject().value("context_revision"));
    }
    void stopDuringAFailedStartDoesNotReachTheNextTurn() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite3");
        const auto executable = dir.filePath("fake-codex");
        const auto providerLog = dir.filePath("provider.log");
        QVERIFY(writeFakeCodex(executable, providerLog));
        LocalJournal journal(path);
        StoredSession seed;
        seed.id = "stop-while-starting";
        seed.generation = 1;
        seed.worktreePath = dir.path();
        seed.adapter = "codex";
        seed.state = "idle";
        QVERIFY(journal.upsertSession(seed));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stop = qScopeGuard([&] { adapter.stop(); });
        const auto state = [&journal] { return journal.sessions().first().state; };
        const auto turn = [&](const QString& messageId, const QString& prompt) {
            auto submitted = request(messageId, seed.id);
            submitted.insert("payload",
                             QJsonObject{{"prompt", prompt}, {"client_message_id", messageId}});
            return host.handleCommand(submitted, "desktop-test");
        };
        QVERIFY(turn("first", "fail-start").value("ok").toBool());
        // Stop while the turn is still starting is kept for its turn.started.
        auto interrupt = request("stop", seed.id);
        interrupt.insert("message_type", "session.interrupt");
        interrupt.insert("payload", QJsonObject{});
        QVERIFY(host.handleCommand(interrupt, "desktop-test").value("ok").toBool());
        // That turn never starts.
        QTRY_COMPARE_WITH_TIMEOUT(state(), QString("failed"), 5000);
        // The next turn runs to its end instead of being stopped as soon as it starts.
        QVERIFY(turn("second", "hello").value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(state(), QString("ready"), 5000);
        QFile log(providerLog);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY2(!log.readAll().contains("interrupt:"), "the next turn was interrupted");
    }
    void staleCompletionDoesNotEndTheActiveTurn() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite3");
        const auto executable = dir.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, dir.filePath("provider.log")));
        LocalJournal journal(path);
        StoredSession seed;
        seed.id = "stale-completion";
        seed.generation = 1;
        seed.worktreePath = dir.path();
        seed.adapter = "codex";
        seed.state = "idle";
        QVERIFY(journal.upsertSession(seed));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stop = qScopeGuard([&] { adapter.stop(); });
        const auto stored = [&journal] { return journal.sessions().first(); };
        const auto turn = [&](const QString& messageId, const QString& prompt) {
            auto submitted = request(messageId, seed.id);
            submitted.insert("payload",
                             QJsonObject{{"prompt", prompt}, {"client_message_id", messageId}});
            return host.handleCommand(submitted, "desktop-test");
        };
        QVERIFY(turn("first", "hello").value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(stored().state, QString("ready"), 5000);
        QVERIFY(turn("second", "hang").value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(stored().state, QString("running"), 5000);
        const auto active = stored();
        QCOMPARE(active.turnId, QString("turn-2"));
        const auto completed = [&](const QString& turnId) {
            emit adapter.notificationReceived(
                "turn/completed", {{"threadId", active.threadId},
                                   {"turn", QJsonObject{{"id", turnId}, {"status", "completed"}}}});
        };
        // A late or repeated completion of an earlier turn leaves the second one running.
        completed("turn-1");
        completed("turn-0");
        QCOMPARE(stored().state, QString("running"));
        QCOMPARE(stored().turnId, QString("turn-2"));
        // Its own completion ends it; a repeat and a late turn/started do not change that.
        completed("turn-2");
        QCOMPARE(stored().state, QString("ready"));
        QVERIFY(stored().turnId.isEmpty());
        completed("turn-2");
        emit adapter.notificationReceived(
            "turn/started",
            {{"threadId", active.threadId}, {"turn", QJsonObject{{"id", "turn-2"}}}});
        QCOMPARE(stored().state, QString("ready"));
        QVERIFY(stored().turnId.isEmpty());
        // Only the two real completions are in the history.
        QCOMPARE(eventCount(journal.eventsAfter(seed.id, 1, 0), "turn.completed"), 2);
        // The session still takes the next turn.
        QVERIFY(turn("third", "hello").value("ok").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(stored().state, QString("ready"), 5000);
    }
    void twoClientsActingOnOneSessionSettleOnce() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite3");
        const auto executable = dir.filePath("fake-codex");
        const auto providerLog = dir.filePath("provider.log");
        QVERIFY(writeFakeCodex(executable, providerLog));
        LocalJournal journal(path);
        StoredSession seed;
        seed.id = "two-clients";
        seed.generation = 1;
        seed.worktreePath = dir.path();
        seed.adapter = "codex";
        seed.state = "idle";
        QVERIFY(journal.upsertSession(seed));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto stop = qScopeGuard([&] { adapter.stop(); });
        const auto command = [&](const QString& client, const QString& type,
                                 const QString& messageId, const QJsonObject& payload) {
            auto submitted = request(messageId, seed.id);
            submitted.insert("message_type", type);
            submitted.insert("payload", payload);
            return host.handleCommand(submitted, client);
        };
        const auto history = [&] { return journal.eventsAfter(seed.id, 1, 0); };
        // Desktop and a phone send a prompt at the same moment: one turn, one prompt.
        QVERIFY(command("desktop", "session.start_turn", "desktop-turn",
                        {{"prompt", "approve"}, {"client_message_id", "desktop-turn"}})
                    .value("ok")
                    .toBool());
        const auto busy = command("phone", "session.start_turn", "phone-turn",
                                  {{"prompt", "hello"}, {"client_message_id", "phone-turn"}});
        QCOMPARE(busy.value("error_code").toString(), QString("session_busy"));
        QCOMPARE(eventCount(history(), "user.message"), 1);
        // Both answer the one approval: the first answer wins, the second is refused.
        QString approvalId;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const auto& value : history()) {
                    const auto event = value.toObject();
                    if (event.value("event_type") == "approval.requested")
                        approvalId =
                            event.value("payload").toObject().value("approval_id").toString();
                }
                return !approvalId.isEmpty();
            }(),
            5000);
        const QJsonObject accept{{"approval_id", approvalId},
                                 {"resolution", QJsonObject{{"decision", "accept"}}}};
        const QJsonObject decline{{"approval_id", approvalId},
                                  {"resolution", QJsonObject{{"decision", "decline"}}}};
        QVERIFY(
            command("desktop", "approval.resolve", "desktop-answer", accept).value("ok").toBool());
        QCOMPARE(command("phone", "approval.resolve", "phone-answer", decline)
                     .value("error_code")
                     .toString(),
                 QString("already_resolved"));
        QTRY_COMPARE_WITH_TIMEOUT(journal.sessions().first().state, QString("ready"), 5000);
        QFile log(providerLog);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(log.readAll().count("approval:"), 1);
        QCOMPARE(eventCount(history(), "approval.resolved"), 1);
        // Read markers only move forward, whichever client reports last.
        const qint64 last = journal.sessions().first().lastCursor;
        QVERIFY(command("desktop", "session.mark_seen", "desktop-seen", {{"cursor", last}})
                    .value("ok")
                    .toBool());
        QVERIFY(command("phone", "session.mark_seen", "phone-seen", {{"cursor", 1}})
                    .value("ok")
                    .toBool());
        QCOMPARE(journal.sessions().first().lastSeenCursor, last);
        // The refused prompt can be sent again once the turn is over.
        QVERIFY(command("phone", "session.start_turn", "phone-retry",
                        {{"prompt", "hello"}, {"client_message_id", "phone-retry"}})
                    .value("ok")
                    .toBool());
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(history(), "turn.completed"), 2, 5000);
    }
};
QTEST_GUILESS_MAIN(TurnAcceptanceTest)
#include "TurnAcceptanceTest.moc"
