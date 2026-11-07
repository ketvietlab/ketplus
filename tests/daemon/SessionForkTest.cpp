#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/compat/ProviderCapabilities.h"
#include "providers/compat/ProviderCompatibility.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"

#include "FakeClaude.h"
#include "FakeCodex.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QtTest>

#include <optional>

using namespace ketplus::daemon;

namespace {

QJsonObject request(const QString& type, const QString& id, const QJsonObject& payload = {},
                    const QString& sessionId = {}, const qint64 generation = -1) {
    QJsonObject result{{"protocol_version", protocol::currentVersion},
                       {"message_id", id},
                       {"message_type", type},
                       {"sent_at", protocol::timestamp()},
                       {"payload", payload}};
    if (!sessionId.isEmpty())
        result.insert("session_id", sessionId);
    if (generation >= 0)
        result.insert("session_generation", generation);
    return result;
}

QJsonObject startTurn(SessionHost& host, const QString& id, const QString& worktree,
                      const QString& provider, const QString& prompt,
                      const QString& sessionId = {}) {
    return host.handleCommand(
        request(QStringLiteral("session.start_turn"), id,
                {{QStringLiteral("worktree_path"), worktree},
                 {QStringLiteral("prompt"), prompt},
                 {QStringLiteral("options"),
                  QJsonObject{{QStringLiteral("provider"), provider},
                              {QStringLiteral("permission"), QStringLiteral("workspace-write")}}}},
                sessionId, sessionId.isEmpty() ? -1 : 1),
        QStringLiteral("desktop-test"));
}

QJsonObject fork(SessionHost& host, const QString& id, const QString& sessionId,
                 const QJsonObject& payload = {}) {
    std::optional<QJsonObject> answer;
    host.forkSession(request(QStringLiteral("session.fork"), id, payload, sessionId, 1),
                     [&answer](const QJsonObject& result) { answer = result; });
    if (!QTest::qWaitFor([&answer] { return answer.has_value(); }, 5000))
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error_code"), "timeout"}};
    return *answer;
}

QStringList eventTypes(LocalJournal& journal, const QString& sessionId) {
    QStringList types;
    for (const QJsonValue& value : journal.eventsAfter(sessionId, 1, 0))
        types.append(value.toObject().value(QStringLiteral("event_type")).toString());
    return types;
}

bool turnCompleted(LocalJournal& journal, const QString& sessionId, const int count = 1) {
    return eventTypes(journal, sessionId).count(QStringLiteral("turn.completed")) >= count;
}

QByteArray logText(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

QString text(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

StoredSession storedSession(LocalJournal& journal, const QString& sessionId) {
    for (const StoredSession& session : journal.sessions())
        if (session.id == sessionId)
            return session;
    return {};
}

} // namespace

class SessionForkTest final : public QObject {
    Q_OBJECT

  private slots:
    void destinationCreatedWhileProviderForkIsPendingIsPreserved() {
        QTemporaryDir dir;
        const auto executable = dir.filePath("fake-codex");
        const auto log = dir.filePath("codex.log");
        const auto gate = dir.filePath("release-fork");
        QVERIFY(writeFakeCodex(executable, log));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::ReadOnly));
        auto bytes = script.readAll();
        script.close();
        const QByteArray marker =
            "        log('fork:' + json.dumps(message['params'], sort_keys=True))\n";
        QVERIFY(bytes.contains(marker));
        const auto gateExpr =
            QJsonDocument(QJsonArray{gate}).toJson(QJsonDocument::Compact) + "[0]";
        bytes.replace(marker, marker + "        while not os.path.exists(" + gateExpr +
                                  "):\n            time.sleep(0.005)\n");
        QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(script.write(bytes), bytes.size());
        script.close();
        LocalJournal journal(dir.filePath("journal.sqlite"));
        QVERIFY(journal.upsertSession({"source",
                                       QFileInfo(dir.path()).canonicalFilePath(),
                                       "codex",
                                       "source-thread",
                                       {},
                                       "ready",
                                       1,
                                       0,
                                       0,
                                       {}}));
        CodexAdapter codex(executable);
        SessionHost host(&journal, &codex);
        const auto release = qScopeGuard([&] {
            QFile f(gate);
            f.open(QIODevice::WriteOnly);
        });
        bool ready = false;
        codex.call("model/list", {},
                   [&](const QJsonObject&, const QString& error) { ready = error.isEmpty(); });
        QTRY_VERIFY_WITH_TIMEOUT(ready, 5000);
        const auto sourceBefore = journal.eventsAfter("source", 1, 0);
        QJsonObject forked;
        host.forkSession(
            request("session.fork", "fork-race", {{"fork_session_id", "target"}}, "source", 1),
            [&](const QJsonObject& value) { forked = value; });
        QTRY_VERIFY_WITH_TIMEOUT(logText(log).contains("fork:"), 5000);
        QVERIFY(forked.isEmpty());
        const auto started =
            startTurn(host, "new-owner", dir.path(), "codex", "owned history", "target");
        QVERIFY2(started.value("ok").toBool(), qPrintable(text(started)));
        QFile gateFile(gate);
        QVERIFY(gateFile.open(QIODevice::WriteOnly));
        gateFile.close();
        QTRY_VERIFY_WITH_TIMEOUT(!forked.isEmpty(), 5000);
        QVERIFY2(!forked.value("ok").toBool(), qPrintable(text(forked)));
        QCOMPARE(forked.value("error_code").toString(), QString("journal_error"));
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, "target"), 5000);
        QCOMPARE(storedSession(journal, "target").threadId, QString("thread-1"));
        QCOMPARE(journal.eventsAfter("source", 1, 0), sourceBefore);
        QVERIFY(!eventTypes(journal, "target").contains("session.forked"));
        QCOMPARE(storedSession(journal, "source").threadId, QString("source-thread"));
    }

    void journalForkRequiresUnusedDestination_data() {
        QTest::addColumn<QString>("destination");
        for (const auto& value : {"existing", "self", "deleted"})
            QTest::newRow(value) << QString(value);
    }
    void journalForkRequiresUnusedDestination() {
        QFETCH(QString, destination);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        StoredSession source{"source", dir.path(), "codex", "source-thread", {}, "ready", 1,
                             0,        0,          {}};
        QVERIFY(journal.upsertSession(source));
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        if (destination == "existing") {
            QVERIFY(journal.upsertSession(
                {"target", dir.path(), "codex", "owned-thread", {}, "ready", 1, 0, 0, {}}));
            QVERIFY(journal.appendEvent("target", 1, "user.message", {{"text", "owned-history"}}) >
                    0);
        }
        if (destination == "deleted")
            QVERIFY(q.exec("INSERT INTO session_tombstones "
                           "VALUES('target',1,1,'deleted-operation','2026-01-01T00:00:00.000Z')"));
        const auto snapshot = [&] {
            QVariantList result;
            QSqlQuery rows(db);
            if (!rows.exec("SELECT * FROM sessions ORDER BY id"))
                return result;
            while (rows.next())
                for (int i = 0; i < rows.record().count(); ++i)
                    result.append(rows.value(i));
            return result;
        };
        const auto before = snapshot();
        QVERIFY(!before.isEmpty());
        const auto history = journal.eventsAfter("target", 1, 0);
        StoredSession copy = source;
        copy.id = destination == "self" ? "source" : "target";
        copy.threadId = "new-fork-thread";
        QString error;
        QVERIFY(!journal.forkSession("source", 1, copy, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(snapshot(), before);
        QCOMPARE(journal.eventsAfter("target", 1, 0), history);
        QVERIFY(q.exec("BEGIN"));
        QVERIFY(q.exec("ROLLBACK"));
        if (destination == "deleted") {
            QVERIFY(q.exec("SELECT count(*) FROM session_tombstones WHERE session_id='target'"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 1);
        }
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        QCOMPARE(reopened.eventsAfter("target", 1, 0), history);
    }

    void journalForkCopiesEmptyHistory() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        StoredSession source{"source", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}};
        QVERIFY(journal.upsertSession(source));
        StoredSession copy = source;
        copy.id = "fork";
        QString error;
        QVERIFY2(journal.forkSession("source", 1, copy, &error), qPrintable(error));
        QCOMPARE(journal.sessionHead("fork").generation, qint64(1));
        QCOMPARE(journal.sessionHead("fork").cursor, qint64(0));
        QCOMPARE(journal.sessions().size(), 2);
    }
    void journalForkRequiresCompleteCopy_data() {
        QTest::addColumn<QString>("fault");
        for (const auto& fault :
             {"activity_summaries", "activity_items", "activity_heads", "events", "replay_events",
              "replay_projection_state", "large_event_payloads", "large_event_payload_chunks",
              "session-start", "commit", "events-late", "chunks-late"})
            QTest::newRow(fault) << QString(fault);
    }
    void journalForkRequiresCompleteCopy() {
        QFETCH(QString, fault);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        StoredSession source{"source", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}};
        QVERIFY(journal.upsertSession(source));
        QVERIFY(
            journal.upsertSession({"other", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}}));
        QVERIFY(journal.appendEvent("other", 1, "user.message", {{"text", "unrelated"}}) > 0);
        QVERIFY(journal.appendEvent("source", 1, "user.message", {{"text", "seed"}}) > 0);
        QVERIFY(journal.appendEvent(
                    "source", 1, "item.started",
                    {{"item_id", "command"},
                     {"item_type", "commandExecution"},
                     {"item", QJsonObject{{"id", "command"}, {"type", "commandExecution"}}}}) > 0);
        const auto cursor = journal.appendEvent(
            "source", 1, "item.completed",
            {{"item_id", "large"},
             {"item_type", "agentMessage"},
             {"item",
              QJsonObject{{"id", "large"},
                          {"type", "agentMessage"},
                          {"text", QString(2 * protocol::maximumEventPayloadBytes, 'x')}}}});
        QVERIFY(cursor > 2);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        const QStringList history{"activity_summaries",   "activity_items",
                                  "activity_heads",       "events",
                                  "replay_events",        "replay_projection_state",
                                  "large_event_payloads", "large_event_payload_chunks"};
        const auto snapshot = [&] {
            QVariantMap result;
            for (const auto& table :
                 history + QStringList{"sessions", "session_contexts", "worktree_catalog"}) {
                QSqlQuery rows(db);
                if (!rows.exec(QString("SELECT * FROM %1 ORDER BY 1,2,3").arg(table)))
                    return QVariantMap{};
                QVariantList values;
                while (rows.next())
                    for (int i = 0; i < rows.record().count(); ++i)
                        values.append(rows.value(i));
                if (rows.lastError().isValid())
                    return QVariantMap{};
                result.insert(table, values);
            }
            return result;
        };
        const auto before = snapshot();
        QCOMPARE(before.size(), history.size() + 3);
        QSqlQuery q(db);
        if (fault == "commit") {
            QVERIFY(q.exec("CREATE TABLE fork_parent(id INTEGER PRIMARY KEY)"));
            QVERIFY(q.exec("CREATE TABLE fork_child(id INTEGER REFERENCES fork_parent(id) "
                           "DEFERRABLE INITIALLY DEFERRED)"));
            QVERIFY(q.exec("CREATE TRIGGER fail_fork AFTER UPDATE ON sessions WHEN NEW.id='fork' "
                           "BEGIN INSERT INTO fork_child VALUES(1); END"));
        } else if (fault == "session-start") {
            QVERIFY(q.exec("CREATE TRIGGER fail_fork BEFORE UPDATE ON sessions WHEN NEW.id='fork' "
                           "BEGIN SELECT RAISE(IGNORE); END"));
        } else {
            const auto table = fault == "events-late"   ? QString("events")
                               : fault == "chunks-late" ? QString("large_event_payload_chunks")
                                                        : fault;
            const auto condition = fault == "events-late"   ? " AND NEW.cursor>2"
                                   : fault == "chunks-late" ? " AND NEW.chunk_offset>0"
                                                            : "";
            QVERIFY(q.exec(QString("CREATE TRIGGER fail_fork BEFORE INSERT ON %1 WHEN "
                                   "NEW.session_id='fork'%2 BEGIN SELECT RAISE(IGNORE); END")
                               .arg(table, condition)));
        }
        StoredSession copy = source;
        copy.id = "fork";
        copy.threadId = "forked-thread";
        QString error;
        QVERIFY(!journal.forkSession("source", 1, copy, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY2(q.exec("BEGIN"), qPrintable(q.lastError().text()));
        QVERIFY(q.exec("ROLLBACK"));
        QCOMPARE(snapshot(), before);
        QVERIFY(q.exec("DROP TRIGGER fail_fork"));
        QVERIFY2(journal.forkSession("source", 1, copy, &error), qPrintable(error));
        QCOMPARE(journal.sessionHead("source").cursor, cursor);
        QCOMPARE(journal.sessionHead("fork").cursor, cursor);
        const auto rowsFor = [&](const QString& table, const QString& id) {
            QVariantList result;
            QSqlQuery rows(db);
            rows.prepare(
                QString("SELECT * FROM %1 WHERE session_id=? AND generation=1 ORDER BY 3,4")
                    .arg(table));
            rows.addBindValue(id);
            if (!rows.exec())
                return QVariantList{};
            while (rows.next())
                for (int i = 2; i < rows.record().count(); ++i)
                    result.append(rows.value(i));
            return result;
        };
        for (const auto& table : history) {
            const auto sourceRows = rowsFor(table, "source");
            QVERIFY2(!sourceRows.isEmpty(), qPrintable(table));
            QCOMPARE(rowsFor(table, "fork"), sourceRows);
        }
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QCOMPARE(reopened.sessionHead("source").cursor, cursor);
        QCOMPARE(reopened.sessionHead("fork").cursor, cursor);
        QCOMPARE(reopened.sessionHead("other").cursor, qint64(1));
    }

    void onlyCodexAndClaudeCanFork() {
        QCOMPARE(compat::capabilityForMethod(u"thread/fork"), QStringLiteral("threadFork"));
        QVERIFY(compat::policy(QStringLiteral("codex"))
                    .baselineSupports(compat::capability::threadFork));
        QVERIFY(compat::policy(QStringLiteral("claude"))
                    .baselineSupports(compat::capability::threadFork));
        QVERIFY(!compat::policy(QStringLiteral("cursor"))
                     .baselineSupports(compat::capability::threadFork));
        QVERIFY(protocol::features(1).contains(QStringLiteral("session.fork")));
        QVERIFY(!protocol::features(0).contains(QStringLiteral("session.fork")));
    }

    void forksClaudeConversationWithItsTranscript() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claudePath = directory.filePath(QStringLiteral("fake-claude"));
        const QString logPath = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claudePath, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        const QString codexPath = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(codexPath, directory.filePath(QStringLiteral("codex.log"))));
        CodexAdapter codex(codexPath);
        ClaudeAdapter claude(claudePath);
        SessionHost host(&journal, &codex, &claude);

        const QJsonObject started = startTurn(host, QStringLiteral("start"), directory.path(),
                                              QStringLiteral("claude"), QStringLiteral("hello"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(), qPrintable(text(started)));
        const QString sourceId = started.value(QStringLiteral("session_id")).toString();
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, sourceId), 5000);
        const QStringList sourceEvents = eventTypes(journal, sourceId);
        const StoredSession source = storedSession(journal, sourceId);
        QCOMPARE(source.threadId, QStringLiteral("claude-thread-1"));

        const QJsonObject forked = fork(host, QStringLiteral("fork-1"), sourceId,
                                        {{QStringLiteral("fork_session_id"), "fork-session"}});
        QVERIFY2(forked.value(QStringLiteral("ok")).toBool(), qPrintable(text(forked)));
        QCOMPARE(forked.value(QStringLiteral("session_id")).toString(),
                 QStringLiteral("fork-session"));
        QCOMPARE(forked.value(QStringLiteral("adapter")).toString(), QStringLiteral("claude"));
        QCOMPARE(forked.value(QStringLiteral("worktree_path")).toString(), source.worktreePath);
        QVERIFY(logText(logPath).contains(
            QStringLiteral("fork:{\"cwd\": \"%1\", \"threadId\": \"claude-thread-1\"}")
                .arg(source.worktreePath)
                .toUtf8()));

        // The fork replays its source's transcript at the same cursors, then records the fork.
        const QStringList forkEvents = eventTypes(journal, QStringLiteral("fork-session"));
        QCOMPARE(forkEvents.mid(0, sourceEvents.size()), sourceEvents);
        QCOMPARE(forkEvents.size(), sourceEvents.size() + 1);
        QCOMPARE(forkEvents.last(), QStringLiteral("session.forked"));
        const QJsonObject marker = journal.eventsAfter(QStringLiteral("fork-session"), 1, 0)
                                       .last()
                                       .toObject()
                                       .value(QStringLiteral("payload"))
                                       .toObject();
        QCOMPARE(marker.value(QStringLiteral("from_session_id")).toString(), sourceId);
        QCOMPARE(marker.value(QStringLiteral("through_cursor")).toInteger(), source.lastCursor);
        const StoredSession stored = storedSession(journal, QStringLiteral("fork-session"));
        QCOMPARE(stored.threadId, QStringLiteral("fork-of-claude-thread-1"));
        QCOMPARE(stored.currentTurnSequence, source.currentTurnSequence);
        QCOMPARE(stored.lastCursor, source.lastCursor + 1);
        QCOMPARE(eventTypes(journal, sourceId), sourceEvents);

        // A retried request returns the first answer instead of forking again.
        QCOMPARE(fork(host, QStringLiteral("fork-1"), sourceId,
                      {{QStringLiteral("fork_session_id"), "fork-session"}}),
                 forked);
        QCOMPARE(logText(logPath).count("fork:"), 1);

        // The next turn in the fork continues the forked SDK session, not the source's.
        const QJsonObject next =
            startTurn(host, QStringLiteral("next"), directory.path(), QStringLiteral("claude"),
                      QStringLiteral("continue"), QStringLiteral("fork-session"));
        QVERIFY2(next.value(QStringLiteral("ok")).toBool(), qPrintable(text(next)));
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, QStringLiteral("fork-session"), 2), 5000);
        QCOMPARE(storedSession(journal, QStringLiteral("fork-session")).threadId,
                 QStringLiteral("fork-of-claude-thread-1"));
        QCOMPARE(storedSession(journal, sourceId).threadId, QStringLiteral("claude-thread-1"));
    }

    void forksCodexConversationAndResumesTheCopy() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString codexPath = directory.filePath(QStringLiteral("fake-codex"));
        const QString logPath = directory.filePath(QStringLiteral("codex.log"));
        QVERIFY(writeFakeCodex(codexPath, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(codexPath);
        SessionHost host(&journal, &codex);

        const QJsonObject started = startTurn(host, QStringLiteral("start"), directory.path(),
                                              QStringLiteral("codex"), QStringLiteral("hello"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(), qPrintable(text(started)));
        const QString sourceId = started.value(QStringLiteral("session_id")).toString();
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, sourceId), 5000);

        const QJsonObject forked = fork(host, QStringLiteral("fork-1"), sourceId);
        QVERIFY2(forked.value(QStringLiteral("ok")).toBool(), qPrintable(text(forked)));
        const QString forkId = forked.value(QStringLiteral("session_id")).toString();
        QVERIFY(!forkId.isEmpty());
        QVERIFY(forkId != sourceId);
        QVERIFY(logText(logPath).contains("fork:{\"cwd\": "));
        QVERIFY(logText(logPath).contains(
            "\"ephemeral\": false, \"excludeTurns\": true, \"threadId\": \"thread-1\"}"));
        QCOMPARE(storedSession(journal, forkId).threadId, QStringLiteral("fork-of-thread-1"));

        const QJsonObject next = startTurn(host, QStringLiteral("next"), directory.path(),
                                           QStringLiteral("codex"), QStringLiteral("go"), forkId);
        QVERIFY2(next.value(QStringLiteral("ok")).toBool(), qPrintable(text(next)));
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, forkId, 2), 5000);
        QVERIFY(logText(logPath).contains("turn:fork-of-thread-1"));
    }

    void forksRunningSession_data() {
        QTest::addColumn<QString>("provider");
        QTest::newRow("claude") << QStringLiteral("claude");
        QTest::newRow("codex") << QStringLiteral("codex");
    }

    void forksRunningSession() {
        QFETCH(QString, provider);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString claudePath = directory.filePath(QStringLiteral("fake-claude"));
        const QString logPath = directory.filePath(QStringLiteral("claude.log"));
        QVERIFY(writeFakeClaude(claudePath, logPath));
        LocalJournal journal(directory.filePath(QStringLiteral("journal.sqlite3")));
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        const QString codexPath = directory.filePath(QStringLiteral("fake-codex"));
        QVERIFY(writeFakeCodex(codexPath, directory.filePath(QStringLiteral("codex.log"))));
        CodexAdapter codex(codexPath);
        ClaudeAdapter claude(claudePath);
        SessionHost host(&journal, &codex, &claude);

        QCOMPARE(fork(host, QStringLiteral("unknown"), QStringLiteral("nope"))
                     .value(QStringLiteral("error_code"))
                     .toString(),
                 QStringLiteral("session_not_found"));

        const QJsonObject started = startTurn(host, QStringLiteral("start"), directory.path(),
                                              provider, QStringLiteral("hello"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(), qPrintable(text(started)));
        const QString sourceId = started.value(QStringLiteral("session_id")).toString();
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, sourceId), 5000);
        const QJsonObject active =
            startTurn(host, QStringLiteral("source-active"), directory.path(), provider,
                      QStringLiteral("hang"), sourceId);
        QVERIFY2(active.value(QStringLiteral("ok")).toBool(), qPrintable(text(active)));
        QTRY_COMPARE_WITH_TIMEOUT(
            eventTypes(journal, sourceId).count(QStringLiteral("turn.started")), 2, 5000);
        QString unrelatedId;
        StoredSession unrelatedBefore;
        if (provider == QStringLiteral("codex")) {
            const auto unrelated = startTurn(host, QStringLiteral("unrelated-active"),
                                             directory.path(), provider, QStringLiteral("hang"));
            QVERIFY(unrelated.value(QStringLiteral("ok")).toBool());
            unrelatedId = unrelated.value(QStringLiteral("session_id")).toString();
            QTRY_VERIFY(eventTypes(journal, unrelatedId).contains(QStringLiteral("turn.started")));
            unrelatedBefore = storedSession(journal, unrelatedId);
        }
        QSignalSpy stopped(&codex, &CodexAdapter::stopped);
        const StoredSession before = storedSession(journal, sourceId);
        const QStringList sourceEvents = eventTypes(journal, sourceId);
        const QJsonObject result = fork(host, QStringLiteral("live-fork"), sourceId);
        QVERIFY2(result.value(QStringLiteral("ok")).toBool(), qPrintable(text(result)));
        const QString forkId = result.value(QStringLiteral("session_id")).toString();
        QCOMPARE(storedSession(journal, sourceId).state, before.state);
        QCOMPARE(storedSession(journal, sourceId).turnId, before.turnId);
        QCOMPARE(eventTypes(journal, sourceId), sourceEvents);
        QCOMPARE(storedSession(journal, forkId).state, QStringLiteral("ready"));
        QVERIFY(storedSession(journal, forkId).turnId.isEmpty());
        QVERIFY(storedSession(journal, forkId).threadId != before.threadId);
        QVERIFY(turnCompleted(journal, forkId));
        QCOMPARE(fork(host, QStringLiteral("live-fork"), sourceId), result);
        const QJsonObject next = startTurn(host, QStringLiteral("fork-next"), directory.path(),
                                           provider, QStringLiteral("continue"), forkId);
        QVERIFY2(next.value(QStringLiteral("ok")).toBool(), qPrintable(text(next)));
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, forkId, 3), 5000);
        QVERIFY(!turnCompleted(journal, sourceId, 2));
        QCOMPARE(stopped.count(), 0);
        if (!unrelatedId.isEmpty()) {
            QCOMPARE(storedSession(journal, unrelatedId).state, unrelatedBefore.state);
            QCOMPARE(storedSession(journal, unrelatedId).turnId, unrelatedBefore.turnId);
            QVERIFY(!turnCompleted(journal, unrelatedId));
        }
        QCOMPARE(storedSession(journal, sourceId).state, before.state);
    }
};

QTEST_GUILESS_MAIN(SessionForkTest)
#include "SessionForkTest.moc"
