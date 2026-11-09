#include "protocol/DaemonProtocol.h"
#include "providers/ClaudeAdapter.h"
#include "providers/CodexAdapter.h"
#include "providers/compat/ProviderCapabilities.h"
#include "providers/compat/ProviderCompatibility.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include "storage/McpMediaStore.h"

#include "FakeClaude.h"
#include "FakeCodex.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
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

QSqlDatabase journalConnection(const QString& path) {
    for (const auto& name : QSqlDatabase::connectionNames())
        if (name.startsWith("ketplus-daemon-") &&
            QSqlDatabase::database(name).databaseName() == path)
            return QSqlDatabase::database(name);
    return {};
}

bool seedEvents(QSqlDatabase db, const QString& sessionId, const int from, const int count) {
    QSqlQuery q(db);
    return q.exec(QString("WITH RECURSIVE n(i) AS (VALUES(%1) UNION ALL SELECT i+1 FROM n WHERE "
                          "i<%2) INSERT INTO events(session_id,generation,cursor,event_type,"
                          "payload,sent_at,turn_sequence) SELECT '%3',1,i,'assistant.delta',"
                          "'{}',datetime('now'),1 FROM n")
                      .arg(from)
                      .arg(from + count - 1)
                      .arg(sessionId)) &&
           q.exec(QString("UPDATE sessions SET last_cursor=%1 WHERE id='%2'")
                      .arg(from + count - 1)
                      .arg(sessionId));
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
    void forkOfAnArchivedConversationIsNotArchived() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto executable = dir.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, dir.filePath("codex.log")));
        LocalJournal journal(dir.filePath("journal.sqlite"));
        StoredSession source;
        source.id = "source";
        source.worktreePath = dir.path();
        source.adapter = "codex";
        source.threadId = "source-thread";
        source.state = "ready";
        QVERIFY(journal.upsertSession(source));
        const auto now = QDateTime::currentDateTimeUtc();
        QVERIFY(
            journal.setSessionArchived("source", 1, now.addDays(-40).toString(Qt::ISODateWithMs)));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        const auto result = fork(host, "fork-archived", "source", {{"fork_session_id", "target"}});
        QVERIFY2(result.value("ok").toBool(), qPrintable(text(result)));
        QCOMPARE(storedSession(journal, "target").archivedAt, QString());
        const auto catalog =
            host.handleCommand(request("session.list", "list-archived"), "desktop-test");
        bool listed = false;
        for (const auto& value : catalog.value("sessions").toArray()) {
            const auto row = value.toObject();
            if (row.value("session_id") == "target") {
                listed = true;
                QVERIFY(!row.contains("archived_at"));
            }
        }
        QVERIFY(listed);
        // The source is due; the fork must survive every later pass.
        QVERIFY(host.expireArchivedSessions(now));
        QVERIFY(!host.expireArchivedSessions(now));
        QVERIFY(storedSession(journal, "source").id.isEmpty());
        QCOMPARE(storedSession(journal, "target").id, QString("target"));
        adapter.stop();
    }

    void forkRequiresMediaOwnershipBeforeJournalCommit_data() {
        QTest::addColumn<QString>("fault");
        for (const auto* value :
             {"source-malformed", "source-directory", "source-symlink", "source-unreadable",
              "source-oversized", "target-directory", "target-symlink", "target-conflict",
              "root-symlink", "commit-failure", "target-matching", "empty-source"})
            QTest::newRow(value) << QString(value);
    }

    void forkRequiresMediaOwnershipBeforeJournalCommit() {
        QFETCH(QString, fault);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto executable = dir.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, dir.filePath("codex.log")));
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
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
        const auto media = McpMediaStore::directoryForJournal(path);
        const auto refs = media + "/refs";
        const auto refPath = [&](const QByteArray& id) {
            return refs + '/' + QCryptographicHash::hash(id, QCryptographicHash::Sha256).toHex() +
                   ".refs";
        };
        const auto sourceRef = refPath("source");
        const auto targetRef = refPath("target");
        const auto put = [](const QString& name, const QByteArray& bytes) {
            QFile file(name);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        McpMediaStore store(media);
        const auto png = QByteArray::fromHex("89504e470d0a1a0a0000000d494844520000000800000006");
        const auto hash = QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex();
        const auto original = hash + '\n';
        if (fault != "empty-source") {
            const auto image = store.storeImage(png, "image/png", "source");
            QVERIFY(!image.isEmpty());
            QVERIFY(journal.appendEvent(
                        "source", 1, "item.completed",
                        {{"item",
                          QJsonObject{{"id", "picture"},
                                      {"type", "mcpToolCall"},
                                      {"result", QJsonObject{{"content", QJsonArray{image}}}}}}}) >
                    0);
        }
        CodexAdapter codex(executable);
        SessionHost host(&journal, &codex);
        bool ready = false;
        codex.call("model/list", {},
                   [&](const QJsonObject&, const QString& error) { ready = error.isEmpty(); });
        QTRY_VERIFY_WITH_TIMEOUT(ready, 5000);
        const auto before = journal.eventsAfter("source", 1, 0);
        const auto foreign = dir.filePath("foreign.refs");
        QVERIFY(put(foreign, original));
        const auto backup = dir.filePath("backup-refs");
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery sql(db);
        QByteArray damaged;
        if (fault == "source-malformed" || fault == "source-oversized") {
            damaged = fault == "source-malformed"
                          ? original + "unsupported\n"
                          : original.repeated(4 * 1024 * 1024 / original.size() + 1);
            QVERIFY(put(sourceRef, damaged));
        } else if (fault == "source-directory" || fault == "source-symlink") {
            QVERIFY(QFile::remove(sourceRef));
            if (fault == "source-directory")
                QVERIFY(QDir().mkpath(sourceRef));
            else
                QVERIFY(QFile::link(foreign, sourceRef));
        } else if (fault == "source-unreadable") {
            QVERIFY(QFile::setPermissions(sourceRef, QFileDevice::WriteOwner));
        } else if (fault == "target-directory") {
            QVERIFY(QDir().mkpath(targetRef));
        } else if (fault == "target-symlink") {
            QVERIFY(QFile::link(foreign, targetRef));
        } else if (fault == "target-conflict" || fault == "target-matching") {
            damaged = fault == "target-conflict" ? QByteArray(64, 'b') + '\n' : original;
            QVERIFY(put(targetRef, damaged));
        } else if (fault == "root-symlink") {
            QVERIFY(QDir().rename(refs, backup));
            QVERIFY(QFile::link(backup, refs));
        } else if (fault == "commit-failure") {
            QVERIFY(sql.exec("PRAGMA foreign_keys=ON"));
            QVERIFY(sql.exec("CREATE TABLE fork_media_parent(id INTEGER PRIMARY KEY)"));
            QVERIFY(sql.exec("CREATE TABLE fork_media_child(id INTEGER REFERENCES "
                             "fork_media_parent(id) DEFERRABLE INITIALLY DEFERRED)"));
            QVERIFY(sql.exec("CREATE TRIGGER reject_media_fork AFTER INSERT ON sessions WHEN "
                             "NEW.id='target' BEGIN INSERT INTO fork_media_child VALUES(1); END"));
        }
        const auto result = fork(host, "media-fork", "source", {{"fork_session_id", "target"}});
        const bool valid = fault == "target-matching" || fault == "empty-source";
        QCOMPARE(result.value("ok").toBool(), valid);
        QCOMPARE(journal.eventsAfter("source", 1, 0), before);
        if (!valid) {
            QCOMPARE(result.value("error_code").toString(), "journal_error");
            QVERIFY(storedSession(journal, "target").id.isEmpty());
            // The abandoned copy is unlisted at once and purged in bounded passes (ADR 0094).
            QTRY_VERIFY(journal.eventsAfter("target", 1, 0).isEmpty() &&
                        !journal.hasPendingHistoryPurge());
            if (fault == "source-malformed" || fault == "source-oversized")
                QCOMPARE(logText(sourceRef), damaged);
            if (fault == "target-conflict")
                QCOMPARE(logText(targetRef), damaged);
            if (fault == "root-symlink") {
                QVERIFY(QFile::remove(refs));
                QVERIFY(QDir().rename(backup, refs));
            } else if (fault == "source-directory") {
                QVERIFY(QDir().rmdir(sourceRef));
            } else if (fault == "source-symlink") {
                QVERIFY(QFile::remove(sourceRef));
            } else if (fault == "source-unreadable") {
                QVERIFY(QFile::setPermissions(sourceRef,
                                              QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            } else if (fault == "target-directory") {
                QVERIFY(QDir().rmdir(targetRef));
            } else if (fault == "target-symlink" || fault == "target-conflict") {
                QVERIFY(QFile::remove(targetRef));
            } else if (fault == "commit-failure") {
                QCOMPARE(logText(targetRef), original); // prepared ownership survives DB rollback
                QVERIFY(sql.exec("DROP TRIGGER reject_media_fork"));
                LocalJournal reopened(path);
                QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
                const auto live = reopened.sessionIds();
                QVERIFY(live.has_value());
                QVERIFY(live->contains("source"));
                QVERIFY(!live->contains("target"));
                McpMediaStore restartedStore(media);
                restartedStore.retainSessions(*live);
                QVERIFY(!QFileInfo::exists(targetRef));
                QVERIFY(QFileInfo::exists(media + '/' + hash + ".png"));
                QCOMPARE(logText(sourceRef), original);
            }
            QVERIFY(put(sourceRef, original));
            const auto retry =
                fork(host, "media-fork-retry", "source", {{"fork_session_id", "target"}});
            QVERIFY2(retry.value("ok").toBool(), qPrintable(text(retry)));
        }
        QCOMPARE(journal.eventsAfter("source", 1, 0), before);
        QVERIFY(!storedSession(journal, "target").id.isEmpty());
        QCOMPARE(logText(foreign), original);
        if (fault != "empty-source") {
            QCOMPARE(logText(targetRef), original);
            store.releaseSession("source");
            QVERIFY(QFileInfo::exists(media + '/' + hash + ".png"));
            store.releaseSession("target");
            QVERIFY(!QFileInfo::exists(media + '/' + hash + ".png"));
        }
    }

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

    void journalForkBacklogCharacterization_data() {
        QTest::addColumn<int>("rows");
        QTest::newRow("1000") << 1000;
        QTest::newRow("100000") << 100000;
        QTest::newRow("300000") << 300000;
    }
    void journalForkBacklogCharacterization() {
        QFETCH(int, rows);
        if (rows > 1000 && !qEnvironmentVariableIsSet("KETPLUS_RUN_FORK_BACKLOG_TESTS"))
            QSKIP("Opt-in synthetic backlog characterization");
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        StoredSession source{"source", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}};
        QVERIFY(journal.upsertSession(source));
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY(db.transaction());
        const auto cte = QString("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n "
                                 "WHERE i<%1) ")
                             .arg(rows);
        QVERIFY2(q.exec(cte + "INSERT INTO events(session_id,generation,cursor,event_type,payload,"
                              "sent_at,turn_sequence) SELECT 'source',1,i,'assistant.delta',"
                              "'{\"text\":\"bounded fixture\"}',datetime('now'),1 FROM n"),
                 qPrintable(q.lastError().text()));
        QVERIFY2(q.exec(cte + "INSERT INTO replay_events(session_id,generation,cursor,event_type,"
                              "item_id,payload,sent_at,turn_sequence) SELECT 'source',1,i,"
                              "'assistant.delta','item','{\"text\":\"bounded fixture\"}',"
                              "datetime('now'),1 FROM n"),
                 qPrintable(q.lastError().text()));
        QVERIFY2(q.exec(QString("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n "
                                "WHERE i<%1) INSERT INTO large_event_payload_chunks "
                                "SELECT 'source',1,'transfer',i*65536,zeroblob(65536) FROM n")
                            .arg(qMin(rows, 4096))),
                 qPrintable(q.lastError().text()));
        QVERIFY2(q.exec(QString("UPDATE sessions SET last_cursor=%1 WHERE id='source'").arg(rows)),
                 qPrintable(q.lastError().text()));
        QVERIFY(db.commit());
        // Checkpoint the fixture so the timing below measures the fork, not setup pages.
        QVERIFY2(q.exec("PRAGMA wal_checkpoint(TRUNCATE)"), qPrintable(q.lastError().text()));
        q.finish();
        StoredSession copy = source;
        copy.id = "fork";
        QString error;
        QElapsedTimer timer;
        timer.start();
        auto staged = journal.beginFork("source", 1, copy, &error);
        QVERIFY2(staged, qPrintable(error));
        const auto began = timer.elapsed();
        qint64 slowestPass = 0;
        int passes = 0;
        for (LocalJournal::ForkStep step = LocalJournal::ForkStep::Progress;
             step != LocalJournal::ForkStep::Copied;) {
            QElapsedTimer pass;
            pass.start();
            step = journal.continueFork(*staged, &error);
            slowestPass = qMax(slowestPass, pass.elapsed());
            QVERIFY2(step != LocalJournal::ForkStep::Failed, qPrintable(error));
            QVERIFY(++passes < 100000);
        }
        timer.restart();
        QVERIFY2(journal.finishFork(*staged, copy, &error), qPrintable(error));
        const auto finished = timer.elapsed();
        qInfo() << "fork-characterization rows-per-event-table" << rows << "begin-ms" << began
                << "passes" << passes << "slowest-pass-ms" << slowestPass << "finish-ms"
                << finished;
        // Each event-loop turn stays short however large the source history is.
        QVERIFY2(began < 250 && slowestPass < 250 && finished < 250,
                 qPrintable(QString("%1 %2 %3").arg(began).arg(slowestPass).arg(finished)));
        QCOMPARE(journal.eventsAfter("fork", 1, 0, 1).size(), 1);
    }
    void journalForkCopiesOneSnapshot() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        StoredSession source{"source", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}};
        QVERIFY(journal.upsertSession(source));
        QVERIFY(seedEvents(journalConnection(path), "source", 1, 5000));
        // Malformed UTF-8 is stored as TEXT; the copy must keep its bytes.
        QSqlQuery q(journalConnection(path));
        QVERIFY(q.exec("UPDATE events SET payload=CAST(X'7B22FF22' AS TEXT) WHERE "
                       "session_id='source' AND cursor=7"));
        StoredSession copy = source;
        copy.id = "fork";
        QString error;
        auto staged = journal.beginFork("source", 1, copy, &error);
        QVERIFY2(staged, qPrintable(error));
        QCOMPARE(journal.continueFork(*staged, &error), LocalJournal::ForkStep::Progress);
        // The source keeps changing while the copy is staged and unlisted.
        QVERIFY(journal.appendEvent("source", 1, "user.message", {{"text", "after"}}) > 5000);
        QVERIFY(q.exec("DELETE FROM events WHERE session_id='source' AND cursor=4000"));
        QVERIFY(!journal.upsertSession(copy));
        QCOMPARE(journal.sessions().size(), 1);
        LocalJournal::ForkStep step = LocalJournal::ForkStep::Progress;
        while (step == LocalJournal::ForkStep::Progress)
            step = journal.continueFork(*staged, &error);
        QCOMPARE(step, LocalJournal::ForkStep::Copied);
        QVERIFY2(journal.finishFork(*staged, copy, &error), qPrintable(error));
        QCOMPARE(copy.lastCursor, qint64(5000));
        QCOMPARE(journal.sessionHead("fork").cursor, qint64(5000));
        QVERIFY(q.exec("SELECT count(*), sum(cursor) FROM events WHERE session_id='fork'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toLongLong(), qint64(5000));
        QCOMPARE(q.value(1).toLongLong(), qint64(5000) * 5001 / 2);
        QVERIFY(q.exec("SELECT hex(payload), typeof(payload) FROM events WHERE "
                       "session_id='fork' AND cursor=7"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QString("7B22FF22"));
        QCOMPARE(q.value(1).toString(), QString("text"));
    }
    void forkRefusesAnIdentityWithLeftoverHistory() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        StoredSession source{"source", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}};
        LocalJournal journal(path);
        QVERIFY(journal.upsertSession(source));
        QVERIFY(seedEvents(journalConnection(path), "source", 1, 10));
        // Rows under an identity with no session would otherwise join the copy unnoticed.
        QVERIFY(seedEvents(journalConnection(path), "fork", 1, 1));
        StoredSession copy = source;
        copy.id = "fork";
        QString error;
        QVERIFY(!journal.beginFork("source", 1, copy, &error));
        QCOMPARE(error, QString("The fork destination already has history."));
        QSqlQuery q(journalConnection(path));
        QVERIFY(q.exec("SELECT COUNT(*) FROM session_fork_staging") && q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }
    void interruptedForkBecomesPurgeWork() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        StoredSession source{"source", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}};
        StoredSession copy = source;
        copy.id = "fork";
        {
            LocalJournal journal(path);
            QVERIFY(journal.upsertSession(source));
            QVERIFY(seedEvents(journalConnection(path), "source", 1, 3000));
            QString error;
            auto staged = journal.beginFork("source", 1, copy, &error);
            QVERIFY2(staged, qPrintable(error));
            QCOMPARE(journal.continueFork(*staged, &error), LocalJournal::ForkStep::Progress);
            // A second fork cannot stage over the same identity.
            StoredSession again = copy;
            QVERIFY(!journal.beginFork("source", 1, again, &error));
            QCOMPARE(error, QString("The fork destination already exists."));
            // The daemon stops here: the snapshot is released, the staging row remains.
        }
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QCOMPARE(reopened.sessions().size(), 1);
        QVERIFY(reopened.hasPendingHistoryPurge());
        QVERIFY(!reopened.upsertSession(copy));
        QCOMPARE(reopened.errorString(),
                 QString("This session identity is still being cleaned up."));
        while (reopened.purgeDeletedSessionHistory() == LocalJournal::HistoryPurge::Progress) {
        }
        QVERIFY(!reopened.hasPendingHistoryPurge());
        QVERIFY(reopened.eventsAfter("fork", 1, 0).isEmpty());
        QSqlQuery q(journalConnection(path));
        QVERIFY(q.exec("SELECT count(*) FROM events WHERE session_id='source'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 3000);
        q.finish();
        QString error;
        QVERIFY2(reopened.forkSession("source", 1, copy, &error), qPrintable(error));
        QCOMPARE(reopened.sessionHead("fork").cursor, qint64(3000));
    }
    void abandonedForkKeepsOwnershipWhenCleanupIsIgnored_data() {
        QTest::addColumn<QString>("trigger");
        QTest::newRow("ignored-purge-insert")
            << "CREATE TRIGGER deny BEFORE INSERT ON session_history_purge "
               "BEGIN SELECT RAISE(IGNORE); END";
        QTest::newRow("ignored-staging-delete")
            << "CREATE TRIGGER deny BEFORE DELETE ON session_fork_staging "
               "BEGIN SELECT RAISE(IGNORE); END";
    }
    void abandonedForkKeepsOwnershipWhenCleanupIsIgnored() {
        QFETCH(QString, trigger);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        StoredSession source{"source", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}};
        QVERIFY(journal.upsertSession(source));
        QVERIFY(seedEvents(journalConnection(path), "source", 1, 3000));
        StoredSession copy = source;
        copy.id = "fork";
        QString error;
        auto staged = journal.beginFork("source", 1, copy, &error);
        QVERIFY2(staged, qPrintable(error));
        QCOMPARE(journal.continueFork(*staged, &error), LocalJournal::ForkStep::Progress);
        QSqlQuery q(journalConnection(path));
        QVERIFY2(q.exec(trigger), qPrintable(q.lastError().text()));
        // Neither a lost marker nor a lost release may hand the partial copy to a new session.
        QVERIFY(!journal.abandonFork(*staged, &error));
        QVERIFY(!journal.upsertSession(copy));
        QCOMPARE(journal.sessions().size(), 1);
        QVERIFY(!journal.hasPendingHistoryPurge());
        QVERIFY(q.exec("DROP TRIGGER deny"));
        QVERIFY2(journal.abandonFork(*staged, &error), qPrintable(error));
        QVERIFY(journal.hasPendingHistoryPurge());
        QVERIFY(!journal.upsertSession(copy));
    }
    void interruptedForkStaysStagedWhenRecoveryIsIgnored() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        StoredSession source{"source", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}};
        StoredSession copy = source;
        copy.id = "fork";
        {
            LocalJournal journal(path);
            QVERIFY(journal.upsertSession(source));
            QVERIFY(seedEvents(journalConnection(path), "source", 1, 3000));
            QString error;
            auto staged = journal.beginFork("source", 1, copy, &error);
            QVERIFY2(staged, qPrintable(error));
            QCOMPARE(journal.continueFork(*staged, &error), LocalJournal::ForkStep::Progress);
            QSqlQuery q(journalConnection(path));
            QVERIFY(q.exec("CREATE TRIGGER deny BEFORE INSERT ON session_history_purge "
                           "BEGIN SELECT RAISE(IGNORE); END"));
        }
        {
            LocalJournal reopened(path);
            QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
            QVERIFY(!reopened.hasPendingHistoryPurge());
            QVERIFY(!reopened.upsertSession(copy));
            QSqlQuery q(journalConnection(path));
            QVERIFY(q.exec("SELECT count(*) FROM session_fork_staging"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 1);
            QVERIFY(q.exec("DROP TRIGGER deny"));
        }
        LocalJournal repaired(path);
        QVERIFY2(repaired.isOpen(), qPrintable(repaired.errorString()));
        QVERIFY(repaired.hasPendingHistoryPurge());
        QVERIFY(!repaired.upsertSession(copy));
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

    void journalForkPreservesUnsupportedSourceCounters_data() {
        QTest::addColumn<QString>("column");
        QTest::addColumn<QString>("value");
        for (const auto& column : {QString("last_cursor"), QString("current_turn_sequence")})
            for (const auto& value :
                 {QString("-1"), QString("1.5"), QString("'unknown'"), QString("X'01'")})
                QTest::newRow(qPrintable(column + "-" + value)) << column << value;
    }
    void journalForkPreservesUnsupportedSourceCounters() {
        QFETCH(QString, column);
        QFETCH(QString, value);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        StoredSession source{"source", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, {}};
        QVERIFY(journal.upsertSession(source));
        QCOMPARE(journal.appendEvent("source", 1, "user.message", {{"text", "preserved"}}),
                 qint64(1));
        const auto history = journal.eventsAfter("source", 1, 0);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY(q.exec(QString("UPDATE sessions SET %1=%2 WHERE id='source'").arg(column, value)));
        const auto counters = [&] {
            QSqlQuery read(db);
            if (!read.exec("SELECT quote(last_cursor),quote(current_turn_sequence) FROM sessions "
                           "WHERE id='source'") ||
                !read.next())
                return QStringList{};
            return QStringList{read.value(0).toString(), read.value(1).toString()};
        };
        const auto before = counters();
        QCOMPARE(before.size(), 2);
        StoredSession copy = source;
        copy.id = "fork";
        QString error;
        QVERIFY(!journal.forkSession("source", 1, copy, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(counters(), before);
        QCOMPARE(journal.eventsAfter("source", 1, 0), history);
        QVERIFY(q.exec("SELECT count(*) FROM sessions WHERE id='fork'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
        QVERIFY2(q.exec("BEGIN"), qPrintable(q.lastError().text()));
        QVERIFY(q.exec("ROLLBACK"));
        QVERIFY(
            q.exec("UPDATE sessions SET last_cursor=1,current_turn_sequence=1 WHERE id='source'"));
        QVERIFY2(journal.forkSession("source", 1, copy, &error), qPrintable(error));
        QCOMPARE(copy.lastCursor, qint64(1));
        QCOMPARE(copy.currentTurnSequence, qint64(1));
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QCOMPARE(reopened.sessionHead("source").cursor, qint64(1));
        QCOMPARE(reopened.sessionHead("fork").cursor, qint64(1));
        QCOMPARE(reopened.eventsAfter("source", 1, 0), history);
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

    void hostForkCopiesInBoundedPasses() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString codexPath = directory.filePath(QStringLiteral("fake-codex"));
        const QString logPath = directory.filePath(QStringLiteral("codex.log"));
        QVERIFY(writeFakeCodex(codexPath, logPath));
        const QString path = directory.filePath(QStringLiteral("journal.sqlite3"));
        LocalJournal journal(path);
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        CodexAdapter codex(codexPath);
        SessionHost host(&journal, &codex);
        const QJsonObject started = startTurn(host, QStringLiteral("start"), directory.path(),
                                              QStringLiteral("codex"), QStringLiteral("hello"));
        QVERIFY2(started.value(QStringLiteral("ok")).toBool(), qPrintable(text(started)));
        const QString sourceId = started.value(QStringLiteral("session_id")).toString();
        QTRY_VERIFY_WITH_TIMEOUT(turnCompleted(journal, sourceId), 5000);
        const qint64 cursor = journal.sessionHead(sourceId).cursor;
        QVERIFY(seedEvents(journalConnection(path), sourceId, int(cursor) + 1, 60000));
        const auto staging = [&] {
            QSqlQuery q(journalConnection(path));
            return q.exec("SELECT count(*) FROM session_fork_staging") && q.next()
                       ? q.value(0).toInt()
                       : -1;
        };

        std::optional<QJsonObject> answer;
        host.forkSession(request(QStringLiteral("session.fork"), QStringLiteral("fork-1"),
                                 {{QStringLiteral("fork_session_id"), "staged-fork"}}, sourceId, 1),
                         [&answer](const QJsonObject& result) { answer = result; });
        QTRY_VERIFY_WITH_TIMEOUT(staging() == 1, 5000);
        QVERIFY(!answer.has_value());
        QVERIFY(storedSession(journal, QStringLiteral("staged-fork")).id.isEmpty());
        // The same destination cannot be requested again while it is being copied.
        const QJsonObject duplicate = fork(host, QStringLiteral("fork-2"), sourceId,
                                           {{QStringLiteral("fork_session_id"), "staged-fork"}});
        QCOMPARE(duplicate.value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("invalid_session_id"));
        // Deleting the source while it is copied cancels the fork and purges the copy.
        const QJsonObject deleted = host.handleCommand(
            request(QStringLiteral("session.delete"), QStringLiteral("delete"), {}, sourceId, 1),
            QStringLiteral("desktop-test"));
        QVERIFY2(deleted.value(QStringLiteral("ok")).toBool(), qPrintable(text(deleted)));
        QTRY_VERIFY_WITH_TIMEOUT(answer.has_value(), 10000);
        QCOMPARE(answer->value(QStringLiteral("error_code")).toString(),
                 QStringLiteral("session_not_found"));
        QTRY_VERIFY_WITH_TIMEOUT(!journal.hasPendingHistoryPurge(), 10000);
        QCOMPARE(staging(), 0);
        QVERIFY(journal.eventsAfter(QStringLiteral("staged-fork"), 1, 0).isEmpty());
        QVERIFY(storedSession(journal, QStringLiteral("staged-fork")).id.isEmpty());
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
