#include "FakeCodex.h"
#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

namespace ketplus::daemon::detail {
QString sessionTombstonesQuery(bool continuation);
}

using namespace ketplus::daemon;
namespace {
QList<QPair<QString, QString>> unsupportedOwnershipMetadata() {
    return {{"broken-json", "not-json"},
            {"root-array", "[]"},
            {"provider-string", R"({"codex":"unknown"})"},
            {"thread-number", R"({"codex":{"thread_id":42}})"},
            {"retired-string", R"({"codex":{"retired_thread_ids":"shared-thread"}})"},
            {"retired-number", R"({"codex":{"retired_thread_ids":[42]}})"},
            {"unknown-fields", R"({"codex":{"future_refs":["shared-thread"]}})"},
            {"seen-cursor-string", R"({"codex":{"thread_id":"t","seen_cursor":"7"}})"},
            {"empty-provider", R"({"":{"thread_id":"shared-thread"}})"}};
}
} // namespace
class ProviderDeletionTest final : public QObject {
    Q_OBJECT
  private slots:
    void deletionBacklogCharacterization_data() {
        QTest::addColumn<int>("rows");
        QTest::newRow("1000") << 1000;
        QTest::newRow("100000") << 100000;
        QTest::newRow("300000") << 300000;
    }
    void deletionBacklogCharacterization() {
        QFETCH(int, rows);
        if (rows > 1000 && !qEnvironmentVariableIsSet("KETPLUS_RUN_DELETE_BACKLOG_TESTS"))
            QSKIP("Opt-in synthetic backlog characterization");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(journal.isOpen());
        StoredSession stored;
        stored.id = "deleted";
        stored.generation = 1;
        stored.worktreePath = dir.path();
        stored.adapter = "codex";
        stored.state = "ready";
        QVERIFY(journal.upsertSession(stored));
        stored.id = "unrelated";
        QVERIFY(journal.upsertSession(stored));
        QVERIFY(journal.appendEvent(stored.id, 1, "assistant.delta", {{"text", "keep"}}) > 0);
        const auto unrelatedBefore = journal.eventsAfter(stored.id, 1, 0);
        CodexAdapter adapter(dir.filePath("missing-fixture-provider"));
        SessionHost host(&journal, &adapter);
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
                              "sent_at,turn_sequence) SELECT 'deleted',1,i,'assistant.delta',"
                              "'{\"text\":\"bounded fixture\"}',datetime('now'),1 FROM n"),
                 qPrintable(q.lastError().text()));
        QVERIFY2(q.exec(cte + "INSERT INTO replay_events(session_id,generation,cursor,event_type,"
                              "item_id,payload,sent_at,turn_sequence) SELECT 'deleted',1,i,"
                              "'assistant.delta','item','{\"text\":\"bounded fixture\"}',"
                              "datetime('now'),1 FROM n"),
                 qPrintable(q.lastError().text()));
        const int chunks = qMin(rows, 4096);
        QVERIFY2(q.exec(QString("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n "
                                "WHERE i<%1) INSERT INTO large_event_payload_chunks "
                                "SELECT 'deleted',1,'transfer',i*65536,zeroblob(65536) FROM n")
                            .arg(chunks)),
                 qPrintable(q.lastError().text()));
        QVERIFY(db.commit());
        // Checkpoint the fixture so the timing below measures deletion, not setup pages.
        QVERIFY2(q.exec("PRAGMA wal_checkpoint(TRUNCATE)"), qPrintable(q.lastError().text()));
        q.finish();
        const QJsonObject request{{"protocol_version", 0},
                                  {"message_id", "delete-op"},
                                  {"message_type", "session.delete"},
                                  {"session_id", "deleted"},
                                  {"session_generation", 1},
                                  {"payload", QJsonObject{}}};
        bool ticked = false;
        QTimer::singleShot(0, &host, [&] { ticked = true; });
        QElapsedTimer timer;
        timer.start();
        const auto result = host.handleCommand(request, "desktop");
        const auto elapsed = timer.elapsed();
        QVERIFY2(result.value("ok").toBool(), qPrintable(QJsonDocument(result).toJson()));
        QVERIFY(result.value("applied").toBool());
        QVERIFY(!ticked);
        // The command commits one bounded batch; the rest is unreachable purge work.
        QVERIFY2(elapsed < 500, qPrintable(QString::number(elapsed)));
        QVERIFY(journal.hasPendingHistoryPurge());
        QCOMPARE(journal.sessions().size(), 1);
        qint64 slowestPass = 0;
        int passes = 0;
        for (;;) {
            QElapsedTimer pass;
            pass.start();
            const auto outcome = journal.purgeDeletedSessionHistory();
            slowestPass = qMax(slowestPass, pass.elapsed());
            if (outcome == LocalJournal::HistoryPurge::Idle)
                break;
            QCOMPARE(outcome, LocalJournal::HistoryPurge::Progress);
            QVERIFY(++passes < 100000);
        }
        QVERIFY2(slowestPass < 250, qPrintable(QString::number(slowestPass)));
        QVERIFY(!journal.hasPendingHistoryPurge());
        qInfo() << "delete-characterization rows-per-event-table" << rows << "chunk-bytes"
                << qint64(chunks) * 65536 << "synchronous-ms" << elapsed << "purge-passes" << passes
                << "slowest-pass-ms" << slowestPass;
        for (const auto& table : {"events", "replay_events", "large_event_payload_chunks"}) {
            QVERIFY2(
                q.exec(QString("SELECT count(*) FROM %1 WHERE session_id='deleted'").arg(table)),
                qPrintable(q.lastError().text()));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toLongLong(), 0);
            q.finish();
        }
        QCOMPARE(journal.eventsAfter("unrelated", 1, 0), unrelatedBefore);
        QCOMPARE(journal.sessions().size(), 1);
        QCOMPARE(journal.sessionTombstone("deleted", 1).operationId, QString("delete-op"));
        QVERIFY(host.handleCommand(request, "desktop").value("applied").toBool());
        auto different = request;
        different["message_id"] = "different-op";
        QVERIFY(!host.handleCommand(different, "desktop").value("ok").toBool());
        QCoreApplication::processEvents();
        QVERIFY(ticked);
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        QCOMPARE(reopened.sessions().size(), 1);
        QCOMPARE(reopened.sessionTombstone("deleted", 1).operationId, QString("delete-op"));
    }
    void tombstoneBacklogCharacterization_data() {
        QTest::addColumn<int>("rows");
        QTest::newRow("1000") << 1000;
        QTest::newRow("100000") << 100000;
        QTest::newRow("300000") << 300000;
    }
    void tombstoneBacklogCharacterization() {
        QFETCH(int, rows);
        if (rows > 1000 && !qEnvironmentVariableIsSet("KETPLUS_RUN_DELETE_BACKLOG_TESTS"))
            QSKIP("Opt-in synthetic backlog characterization");
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        QVERIFY(journal.isOpen());
        CodexAdapter adapter(dir.filePath("missing-fixture-provider"));
        SessionHost host(&journal, &adapter);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == journal.databasePath())
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY2(q.exec(QString("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n "
                                "WHERE i<%1) INSERT INTO session_tombstones "
                                "SELECT 'deleted-'||i,1,1,'operation-'||i,datetime('now') FROM n")
                            .arg(rows)),
                 qPrintable(q.lastError().text()));
        q.finish();
        QElapsedTimer timer;
        timer.start();
        const auto result = host.handleCommand({{"protocol_version", 0},
                                                {"message_id", "list"},
                                                {"message_type", "session.list"},
                                                {"payload", QJsonObject{}}},
                                               "desktop");
        const auto elapsed = timer.elapsed();
        if (rows <= 8192) {
            QVERIFY(result.value("ok").toBool());
            QCOMPARE(result.value("deleted_sessions").toArray().size(), rows);
        } else {
            QVERIFY(!result.value("ok").toBool());
            QCOMPARE(result.value("error_code").toString(), "session_catalog_upgrade_required");
        }
        qsizetype total = 0, pages = 0, maximumBytes = 0;
        QJsonObject after;
        QString snapshot;
        QElapsedTimer pageTimer;
        qint64 maximumMs = 0;
        do {
            QJsonObject payload{{"deleted_sessions_page_version", 1},
                                {"deleted_sessions_only", true}};
            if (!after.isEmpty())
                payload.insert("deleted_sessions_after", after);
            if (!snapshot.isEmpty())
                payload.insert("deleted_sessions_snapshot", snapshot);
            pageTimer.start();
            const auto page = host.handleCommand(
                {{"message_type", "session.list"}, {"payload", payload}}, "desktop");
            maximumMs = qMax(maximumMs, pageTimer.elapsed());
            QVERIFY2(page.value("ok").toBool(), qPrintable(QJsonDocument(page).toJson()));
            const auto count = page.value("deleted_sessions").toArray().size();
            QVERIFY(count <= 128);
            QVERIFY(!page.contains("sessions"));
            const auto size = QJsonDocument(page).toJson(QJsonDocument::Compact).size();
            QVERIFY(size < 66 * 1024);
            maximumBytes = qMax(maximumBytes, size);
            total += count;
            ++pages;
            const auto next = page.value("deleted_sessions_next").toObject();
            QVERIFY(next.isEmpty() || next != after);
            after = next;
            snapshot = page.value("deleted_sessions_snapshot").toString();
        } while (!after.isEmpty());
        QCOMPARE(total, rows);
        qInfo() << "tombstone-characterization rows" << rows << "legacy-ms" << elapsed << "pages"
                << pages << "maximum-page-bytes" << maximumBytes << "maximum-page-ms" << maximumMs
                << "all-pages-ms" << timer.elapsed();
    }
    void legacyCatalogUsesActualWireBudget() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        QVERIFY(journal.isOpen());
        CodexAdapter adapter(dir.filePath("missing-provider"));
        SessionHost host(&journal, &adapter);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (QSqlDatabase::database(name).databaseName() == journal.databasePath())
                db = QSqlDatabase::database(name);
        QSqlQuery q(db);
        QVERIFY(q.exec("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n "
                       "WHERE i<18700) INSERT INTO session_tombstones "
                       "SELECT 'deleted-'||i,1,1,'op','now' FROM n"));
        q.finish();
        const QJsonObject request{{"message_type", "session.list"},
                                  {"message_id", QString(128, QChar(1))},
                                  {"payload", QJsonObject{}}};
        const auto result = host.handleCommand(request, "desktop");
        QVERIFY2(result.value("ok").toBool(), qPrintable(QJsonDocument(result).toJson()));
        QCOMPARE(result.value("deleted_sessions").toArray().size(), 18700);
        const auto bytes = protocol::encodeFrame(protocol::response(request, result)).size() - 4;
        QVERIFY(bytes > 960 * 1024);
        QVERIFY(bytes <= protocol::maximumFrameBytes);
    }

    void liveCatalogDoesNotReadDeletionHistory() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        QVERIFY(
            journal.upsertSession({"live", dir.path(), "codex", {}, {}, "ready", 1, 0, 0, "Live"}));
        CodexAdapter adapter(dir.filePath("missing-provider"));
        SessionHost host(&journal, &adapter);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (QSqlDatabase::database(name).databaseName() == journal.databasePath())
                db = QSqlDatabase::database(name);
        QSqlQuery q(db);
        const QJsonObject payload{{"deleted_sessions_page_version", 1},
                                  {"session_catalog_live_only", true}};
        const auto list = [&](const QJsonObject& value) {
            return host.handleCommand({{"message_type", "session.list"}, {"payload", value}},
                                      "desktop");
        };
        for (const auto& table : {"session_tombstone_catalog", "session_tombstones"}) {
            QVERIFY(q.exec(QString("DROP TABLE %1").arg(table)));
            q.finish();
            const auto result = list(payload);
            QVERIFY2(result.value("ok").toBool(), qPrintable(QJsonDocument(result).toJson()));
            QVERIFY(result.value("session_catalog_live_only").toBool());
            QCOMPARE(result.value("sessions").toArray().size(), 1);
            QCOMPARE(result.value("sessions").toArray().first().toObject().value("session_id"),
                     QJsonValue("live"));
            for (const auto& key : {"deleted_sessions", "deleted_sessions_page_version",
                                    "deleted_sessions_next", "deleted_sessions_snapshot"})
                QVERIFY(!result.contains(key));
            QCOMPARE(list({{"deleted_sessions_page_version", 1}}).value("error_code").toString(),
                     "journal_error"); // Full reconciliation still fails closed.
        }
        QVERIFY(protocol::features(1).contains("session.catalog_live/1"));
    }

    void liveCatalogRejectsAmbiguousRequests() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        CodexAdapter adapter(dir.filePath("missing-provider"));
        SessionHost host(&journal, &adapter);
        const auto list = [&](const QJsonObject& payload) {
            return host.handleCommand({{"message_type", "session.list"}, {"payload", payload}},
                                      "desktop");
        };
        const QJsonObject valid{{"deleted_sessions_page_version", 1},
                                {"session_catalog_live_only", true}};
        for (const auto& key :
             {"deleted_sessions_after", "deleted_sessions_snapshot", "deleted_sessions_only"}) {
            auto invalid = valid;
            invalid.insert(key, false); // Presence is ambiguous even when false or empty.
            QCOMPARE(list(invalid).value("error_code"), QJsonValue("invalid_session_catalog"));
        }
        for (const auto& value : {QJsonValue("true"), QJsonValue(1), QJsonValue(QJsonValue::Null),
                                  QJsonValue(QJsonObject{})}) {
            auto invalid = valid;
            invalid.insert("session_catalog_live_only", value);
            QCOMPARE(list(invalid).value("error_code"), QJsonValue("invalid_session_catalog"));
        }
        auto invalid = valid;
        invalid.remove("deleted_sessions_page_version");
        QCOMPARE(list(invalid).value("error_code"), QJsonValue("invalid_session_catalog"));
        invalid = valid;
        invalid.insert("deleted_sessions_page_version", 2);
        QCOMPARE(list(invalid).value("error_code"), QJsonValue("invalid_tombstone_page"));
        auto disabled = valid;
        disabled.insert("session_catalog_live_only", false);
        const auto normal = list(disabled);
        QVERIFY(normal.value("ok").toBool());
        QVERIFY(normal.contains("deleted_sessions"));
        QVERIFY(!normal.contains("session_catalog_live_only"));
    }

    void liveCatalogStillChecksTheWireBudget() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        QVERIFY(journal.upsertSession({"live",
                                       dir.path(),
                                       "codex",
                                       {},
                                       {},
                                       "ready",
                                       1,
                                       0,
                                       0,
                                       QString(protocol::maximumFrameBytes, 'x')}));
        CodexAdapter adapter(dir.filePath("missing-provider"));
        SessionHost host(&journal, &adapter);
        const auto result =
            host.handleCommand({{"message_type", "session.list"},
                                {"payload", QJsonObject{{"deleted_sessions_page_version", 1},
                                                        {"session_catalog_live_only", true}}}},
                               "desktop");
        QCOMPARE(result.value("error_code"), QJsonValue("session_catalog_too_large"));
        QVERIFY(!result.contains("sessions"));
    }

    void tombstoneSnapshotFencesMutationRollbackAndRestart() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        QVERIFY(journal.isOpen());
        CodexAdapter adapter(dir.filePath("missing-provider"));
        SessionHost host(&journal, &adapter);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (QSqlDatabase::database(name).databaseName() == journal.databasePath())
                db = QSqlDatabase::database(name);
        QSqlQuery q(db);
        QVERIFY(q.exec("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n "
                       "WHERE i<200) INSERT INTO session_tombstones "
                       "SELECT 'middle-'||i,1,1,'op','now' FROM n"));
        q.finish();
        const auto list = [&](const QJsonObject& payload) {
            return host.handleCommand({{"message_type", "session.list"}, {"payload", payload}},
                                      "desktop");
        };
        const QJsonObject firstPayload{{"deleted_sessions_page_version", 1},
                                       {"deleted_sessions_only", true}};
        const auto first = list(firstPayload);
        QVERIFY(first.value("ok").toBool());
        const auto token = first.value("deleted_sessions_snapshot");
        const QJsonObject continuation{
            {"deleted_sessions_page_version", 1},
            {"deleted_sessions_snapshot", token},
            {"deleted_sessions_after", first.value("deleted_sessions_next")}};
        QVERIFY(list(continuation).value("ok").toBool());
        QVERIFY(db.transaction());
        QVERIFY(q.exec("INSERT INTO session_tombstones VALUES('aaa',1,1,'op','now')"));
        QVERIFY(db.rollback());
        QCOMPARE(list(firstPayload).value("deleted_sessions_snapshot"), token);
        // A committed insertion behind the keyset cursor must not silently disappear.
        QVERIFY(q.exec("INSERT INTO session_tombstones VALUES('aaa',1,1,'op','now')"));
        QCOMPARE(list(continuation).value("error_code").toString(), "session_catalog_changed");
        auto current = list(firstPayload);
        QCOMPARE(current.value("deleted_sessions")
                     .toArray()
                     .first()
                     .toObject()
                     .value("session_id")
                     .toString(),
                 "aaa");
        // Fence the final fresh-live read too, and invalidate on retention and metadata updates.
        for (const auto& sql :
             {"UPDATE session_tombstones SET operation_id='changed' WHERE session_id='aaa'",
              "DELETE FROM session_tombstones WHERE session_id='aaa'"}) {
            const auto previous = current.value("deleted_sessions_snapshot");
            QVERIFY(q.exec(sql));
            QCOMPARE(list({{"deleted_sessions_page_version", 1},
                           {"deleted_sessions_snapshot", previous}})
                         .value("error_code")
                         .toString(),
                     "session_catalog_changed");
            current = list(firstPayload);
        }
        q.finish();
        {
            LocalJournal reopened(journal.databasePath());
            QVERIFY(reopened.isOpen());
            CodexAdapter other(dir.filePath("missing-provider"));
            SessionHost restarted(&reopened, &other);
            const auto result = restarted.handleCommand(
                {{"message_type", "session.list"},
                 {"payload", QJsonObject{{"deleted_sessions_page_version", 1},
                                         {"deleted_sessions_snapshot",
                                          current.value("deleted_sessions_snapshot")}}}},
                "desktop");
            QCOMPARE(result.value("error_code").toString(), "session_catalog_changed");
        }
        auto invalid = continuation;
        invalid.remove("deleted_sessions_snapshot");
        QCOMPARE(list(invalid).value("error_code").toString(), "invalid_tombstone_page");
        invalid.insert("deleted_sessions_snapshot", QString(129, 'x'));
        QCOMPARE(list(invalid).value("error_code").toString(), "invalid_tombstone_page");
        QVERIFY(q.exec("UPDATE session_tombstone_catalog SET revision=zeroblob(2097152)"));
        QCOMPARE(list(firstPayload).value("error_code").toString(), "journal_error");
    }

    void tombstonePagesValidateStorageAndSeekTheActualQuery() {
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        QVERIFY(journal.isOpen());
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == journal.databasePath())
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        // Large escaped identities hit the byte budget before the row budget.
        QVERIFY(q.prepare("INSERT INTO session_tombstones VALUES(?,1,1,'op',datetime('now'))"));
        for (int i = 0; i < 40; ++i) {
            q.bindValue(0, QString(1000, QChar(1)) + QString::number(i).rightJustified(3, '0'));
            QVERIFY(q.exec());
        }
        auto page = journal.sessionTombstones();
        QVERIFY(page.has_value());
        QVERIFY(page->tombstones.size() < 128);
        QVERIFY(!page->next.isEmpty());
        QVERIFY(QJsonDocument(page->tombstones).toJson(QJsonDocument::Compact).size() <= 64 * 1024);
        const auto cursor = page->next;
        QVERIFY(q.prepare("EXPLAIN QUERY PLAN " + detail::sessionTombstonesQuery(true)));
        q.addBindValue(cursor.value("session_id").toString());
        q.addBindValue(cursor.value("session_generation").toInteger());
        q.addBindValue(129);
        QVERIFY(q.exec());
        QString plan;
        while (q.next())
            plan += q.value(3).toString();
        QVERIFY2(plan.contains("SEARCH session_tombstones USING COVERING INDEX"), qPrintable(plan));
        QVERIFY2(plan.contains("(session_id,generation)>(?,?)"), qPrintable(plan));
        QVERIFY(!plan.contains("TEMP B-TREE"));
        q.finish();
        QVERIFY(!journal.sessionTombstones({{"session_id", "id"}}));
        QVERIFY(!journal.sessionTombstones({{"session_id", "id"}, {"session_generation", 1.5}}));
        QVERIFY(q.exec("DELETE FROM session_tombstones"));
        for (const auto& bad : {"''", "zeroblob(2097152)", "CAST(zeroblob(2097152) AS TEXT)"}) {
            QVERIFY(q.exec(
                QString("INSERT INTO session_tombstones VALUES(%1,1,1,'op','now')").arg(bad)));
            QVERIFY(!journal.sessionTombstones());
            QVERIFY(q.exec(detail::sessionTombstonesQuery(false).replace("?", "129")));
            QVERIFY(q.next());
            QVERIFY(q.value(0).isNull());
            QCOMPARE(q.value(0).toByteArray().size(), 0);
            q.finish();
            QVERIFY(q.exec("DELETE FROM session_tombstones"));
        }
        for (const auto& bad : {"-1", "1.5", "zeroblob(2097152)", "'broken'"}) {
            QVERIFY(q.exec(
                QString("INSERT INTO session_tombstones VALUES('id',%1,1,'op','now')").arg(bad)));
            QVERIFY(!journal.sessionTombstones());
            QVERIFY(q.exec("DELETE FROM session_tombstones"));
        }
        for (const auto& invalid : {QString("80"), QString("C3")}) {
            QVERIFY(q.exec(QString("INSERT INTO session_tombstones VALUES(CAST(x'%1' AS TEXT),"
                                   "1,1,'op','now')")
                               .arg(invalid)));
            QVERIFY(!journal.sessionTombstones());
            QVERIFY(q.exec("SELECT hex(session_id) FROM session_tombstones"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toString(), invalid);
            q.finish();
            QVERIFY(q.exec("DELETE FROM session_tombstones"));
        }
        // A valid leading BOM is part of the identity, not a decoder marker.
        QVERIFY(q.exec("INSERT INTO session_tombstones VALUES(CAST(x'EFBBBF313233' AS TEXT),"
                       "1,1,'op','now')"));
        page = journal.sessionTombstones();
        QVERIFY(page.has_value());
        QCOMPARE(page->tombstones.first().toObject().value("session_id").toString().toUtf8(),
                 QByteArray::fromHex("EFBBBF313233"));
        QVERIFY(q.exec("DELETE FROM session_tombstones"));
        QVERIFY(q.exec("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE i<200) "
                       "INSERT INTO session_tombstones SELECT 'same',i,1,'op','now' FROM n"));
        page = journal.sessionTombstones();
        QVERIFY(page.has_value());
        QCOMPARE(page->tombstones.size(), 128);
        QCOMPARE(page->next.value("session_generation").toInteger(), 128);
        // Removing the cursor row between pages cannot shift an OFFSET and skip a row.
        QVERIFY(q.exec("DELETE FROM session_tombstones WHERE generation<=128"));
        page = journal.sessionTombstones(page->next);
        QVERIFY(page.has_value());
        QCOMPARE(page->tombstones.size(), 72);
        QCOMPARE(page->tombstones.first().toObject().value("session_generation").toInteger(), 129);
        QVERIFY(page->next.isEmpty());
        QVERIFY(q.exec("DROP TABLE session_tombstones"));
        QVERIFY(!journal.sessionTombstones());
    }
    void deletedHistoryIsPurgedInBoundedPasses() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        auto journal = std::make_unique<LocalJournal>(path);
        QVERIFY(journal->isOpen());
        StoredSession stored;
        stored.id = "deleted";
        stored.generation = 1;
        stored.worktreePath = dir.path();
        stored.adapter = "codex";
        stored.state = "ready";
        QVERIFY(journal->upsertSession(stored));
        stored.id = "unrelated";
        QVERIFY(journal->upsertSession(stored));
        QVERIFY(journal->appendEvent(stored.id, 1, "assistant.delta", {{"text", "keep"}}) > 0);
        const auto unrelatedBefore = journal->eventsAfter(stored.id, 1, 0);
        const auto connection = [&] {
            for (const auto& name : QSqlDatabase::connectionNames())
                if (name.startsWith("ketplus-daemon-") &&
                    QSqlDatabase::database(name).databaseName() == path)
                    return QSqlDatabase::database(name);
            return QSqlDatabase();
        };
        auto db = connection();
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY2(q.exec("WITH RECURSIVE n(i) AS (VALUES(1) UNION ALL SELECT i+1 FROM n WHERE "
                        "i<5000) INSERT INTO events(session_id,generation,cursor,event_type,"
                        "payload,sent_at,turn_sequence) SELECT 'deleted',1,i,'assistant.delta',"
                        "'{}',datetime('now'),1 FROM n"),
                 qPrintable(q.lastError().text()));
        // An ignored purge marker would hide history that the batch left behind.
        QVERIFY(q.exec("CREATE TRIGGER fail_purge BEFORE INSERT ON session_history_purge "
                       "BEGIN SELECT RAISE(IGNORE); END"));
        QString error;
        QVERIFY(!journal->deleteSession("deleted", 1, "delete-op", 1, &error));
        QCOMPARE(error, QString("The history cleanup work was not stored."));
        QCOMPARE(journal->sessions().size(), 2);
        QVERIFY(journal->sessionTombstone("deleted", 1).sessionId.isEmpty());
        QVERIFY(q.exec("DROP TRIGGER fail_purge"));
        QVERIFY2(journal->deleteSession("deleted", 1, "delete-op", 1, &error), qPrintable(error));
        QCOMPARE(journal->sessions().size(), 1);
        QVERIFY(journal->hasPendingHistoryPurge());
        // The tombstone keeps the generation from being restored onto leftover rows,
        // and retention keeps that tombstone until the purge finishes.
        stored.id = "deleted";
        QVERIFY(!journal->upsertSession(stored));
        QVERIFY(q.exec("UPDATE session_tombstones SET deleted_at='2000-01-01T00:00:00.000Z'"));
        // The journal owns the connection: release every handle on it before closing it.
        q = QSqlQuery();
        db = QSqlDatabase();
        journal.reset();
        journal = std::make_unique<LocalJournal>(path);
        QVERIFY(journal->isOpen());
        QCOMPARE(journal->sessionTombstone("deleted", 1).operationId, QString("delete-op"));
        QVERIFY(journal->hasPendingHistoryPurge());
        db = connection();
        q = QSqlQuery(db);
        QVERIFY(q.exec("CREATE TRIGGER stall_purge BEFORE DELETE ON events "
                       "BEGIN SELECT RAISE(IGNORE); END"));
        QCOMPARE(journal->purgeDeletedSessionHistory(), LocalJournal::HistoryPurge::Failed);
        QCOMPARE(journal->errorString(), QString("Session history cleanup made no progress."));
        QVERIFY(journal->hasPendingHistoryPurge());
        QVERIFY(q.exec("DROP TRIGGER stall_purge"));
        // The daemon drains the rest one pass per event-loop turn.
        CodexAdapter adapter(dir.filePath("missing-fixture-provider"));
        auto host = std::make_unique<SessionHost>(journal.get(), &adapter);
        bool ticked = false;
        QTimer::singleShot(0, host.get(), [&] { ticked = true; });
        QTRY_VERIFY(!journal->hasPendingHistoryPurge());
        QVERIFY(ticked);
        QVERIFY(q.exec("SELECT count(*) FROM events WHERE session_id='deleted'"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toLongLong(), 0);
        q.finish();
        QCOMPARE(journal->eventsAfter("unrelated", 1, 0), unrelatedBefore);
        QCOMPARE(journal->purgeDeletedSessionHistory(), LocalJournal::HistoryPurge::Idle);
        // The host reads the journal until it is destroyed, so it goes first.
        host.reset();
        q = QSqlQuery();
        db = QSqlDatabase();
        journal.reset();
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        QVERIFY(reopened.sessionTombstone("deleted", 1).sessionId.isEmpty());
    }
    void ignoredPurgeMarkerDeleteFails() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(journal.isOpen());
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY(q.exec("INSERT INTO session_history_purge VALUES('deleted',1)"));
        QVERIFY(q.exec("CREATE TRIGGER ignore_marker BEFORE DELETE ON session_history_purge "
                       "BEGIN SELECT RAISE(IGNORE); END"));
        // Reporting progress here would reschedule a 0 ms pass forever.
        QCOMPARE(journal.purgeDeletedSessionHistory(), LocalJournal::HistoryPurge::Failed);
        QCOMPARE(journal.errorString(), QString("The history cleanup marker was not removed."));
        QVERIFY(journal.hasPendingHistoryPurge());
        QVERIFY(q.exec("DROP TRIGGER ignore_marker"));
        QCOMPARE(journal.purgeDeletedSessionHistory(), LocalJournal::HistoryPurge::Progress);
        QVERIFY(!journal.hasPendingHistoryPurge());
        QCOMPARE(journal.purgeDeletedSessionHistory(), LocalJournal::HistoryPurge::Idle);
    }
    void failedSessionDeletionRollsBackAndCanRetry_data() {
        QTest::addColumn<QString>("fault");
        QTest::newRow("commit") << "commit";
        QTest::newRow("ignored-tombstone") << "session_tombstones";
        QTest::newRow("ignored-work") << "session_deletion_work";
        QTest::newRow("ignored-history") << "events";
    }
    void failedSessionDeletionRollsBackAndCanRetry() {
        QFETCH(QString, fault);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(journal.isOpen());
        StoredSession session;
        session.id = "deleted";
        session.generation = 1;
        session.worktreePath = dir.path();
        session.adapter = "codex";
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        const auto number = journal.assignConversationNumber(session.id, 1);
        QVERIFY(!number.isEmpty());
        QVERIFY(journal.appendEvent(session.id, 1, "assistant.delta", {{"text", "keep"}}) > 0);
        const auto before = journal.eventsAfter(session.id, 1, 0);
        session.id = "unrelated";
        QVERIFY(journal.upsertSession(session));
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        if (fault == "commit") {
            QVERIFY(q.exec("CREATE TABLE deletion_parent(id INTEGER PRIMARY KEY)"));
            QVERIFY(q.exec("CREATE TABLE deletion_child(id INTEGER REFERENCES deletion_parent(id) "
                           "DEFERRABLE INITIALLY DEFERRED)"));
            QVERIFY(q.exec("CREATE TRIGGER fail_delete AFTER DELETE ON sessions "
                           "BEGIN INSERT INTO deletion_child VALUES(1); END"));
        } else if (fault == "events") {
            QVERIFY(q.exec("CREATE TRIGGER fail_delete BEFORE DELETE ON events "
                           "BEGIN SELECT RAISE(IGNORE); END"));
        } else {
            QVERIFY(q.exec(QString("CREATE TRIGGER fail_delete BEFORE INSERT ON %1 "
                                   "BEGIN SELECT RAISE(IGNORE); END")
                               .arg(fault)));
        }
        QString error;
        const QJsonArray targets{QJsonObject{{"provider", "codex"}, {"thread_id", "owned"}}};
        QVERIFY(!journal.deleteSession("deleted", 1, "delete-op", 1, &error, targets));
        QVERIFY(!error.isEmpty());
        // A failed commit must release the write transaction, not expose an
        // uncommitted tombstone that would falsely succeed on the next retry.
        QVERIFY2(q.exec("BEGIN"), qPrintable(q.lastError().text()));
        QVERIFY(q.exec("ROLLBACK"));
        QCOMPARE(journal.sessions().size(), 2);
        QCOMPARE(journal.eventsAfter("deleted", 1, 0), before);
        QCOMPARE(journal.conversationNumber("deleted", 1), number);
        QVERIFY(journal.sessionTombstone("deleted", 1).sessionId.isEmpty());
        QVERIFY(journal.deletionWork().isEmpty());
        QVERIFY(q.exec("DROP TRIGGER fail_delete"));
        QVERIFY2(journal.deleteSession("deleted", 1, "delete-op", 1, &error, targets),
                 qPrintable(error));
        QCOMPARE(journal.sessions().size(), 1);
        QCOMPARE(journal.sessions().first().id, QString("unrelated"));
        QVERIFY(journal.eventsAfter("deleted", 1, 0).isEmpty());
        QCOMPARE(journal.sessionTombstone("deleted", 1).operationId, QString("delete-op"));
        QCOMPARE(journal.deletionWork().size(), 1);
        QCOMPARE(journal.deletionWork().first().toObject().value("provider_targets").toArray(),
                 targets);
        QVERIFY(journal.deleteSession("deleted", 1, "delete-op", 1, &error, targets));
        QVERIFY(!journal.deleteSession("deleted", 1, "different-op", 1, &error, targets));
        LocalJournal reopened(path);
        QVERIFY(reopened.isOpen());
        QCOMPARE(reopened.sessions().size(), 1);
        QCOMPARE(reopened.sessionTombstone("deleted", 1).operationId, QString("delete-op"));
        QCOMPARE(reopened.deletionWork().size(), 1);
    }
    void purgeIsDurableAndScopedToUnsharedProviderThreads() {
        QTemporaryDir directory;
        const auto executable = directory.filePath("codex");
        const auto log = directory.filePath("codex.log");
        QVERIFY(writeFakeCodex(executable, log));
        LocalJournal journal(directory.filePath("journal.sqlite"));
        StoredSession session;
        session.id = "deleted";
        session.adapter = "codex";
        session.threadId = "codex-owned";
        session.worktreePath = directory.path();
        session.state = "ready";
        session.providerThreads =
            R"({"claude":{"thread_id":"claude-old"},"codex":{"retired_thread_ids":["codex-retired"]}})";
        QVERIFY(journal.upsertSession(session));
        CodexAdapter adapter(executable);
        SessionHost host(&journal, &adapter);
        QSignalSpy events(&host, &SessionHost::eventReady);
        const auto result = host.handleCommand({{"protocol_version", 0},
                                                {"message_id", "delete"},
                                                {"message_type", "session.delete"},
                                                {"session_id", session.id},
                                                {"session_generation", session.generation},
                                                {"payload", QJsonObject{}}},
                                               "desktop");
        QVERIFY(result.value("ok").toBool());
        QVERIFY(journal.sessions().isEmpty());
        QCOMPARE(journal.deletionWork().size(), 1);
        QCOMPARE(
            journal.deletionWork().first().toObject().value("provider_targets").toArray().size(),
            3);
        QVERIFY(!events.isEmpty());
        QCOMPARE(events.last().first().toJsonObject().value("event_type").toString(),
                 QString("session.deleted"));
        QTRY_COMPARE_WITH_TIMEOUT(
            journal.deletionWork().first().toObject().value("provider_targets").toArray().size(), 1,
            5000);
        QFile calls(log);
        QVERIFY(calls.open(QIODevice::ReadOnly));
        const auto removed = calls.readAll();
        QVERIFY(removed.contains("delete:codex-owned"));
        QVERIFY(removed.contains("delete:codex-retired"));
        const auto deletion =
            journal.deletionWork().first().toObject().value("deletion").toObject();
        QVERIFY(
            !journal.acknowledgeCloudDeletion(session.id, session.generation, "wrong-operation"));
        QVERIFY(journal.acknowledgeCloudDeletion(session.id, session.generation,
                                                 deletion.value("operationId").toString()));
        // Claude was unavailable: keep its exact target, even across a journal reopen.
        LocalJournal reopened(journal.databasePath());
        const auto target = reopened.deletionWork()
                                .first()
                                .toObject()
                                .value("provider_targets")
                                .toArray()
                                .first()
                                .toObject();
        QCOMPARE(target.value("thread_id").toString(), QString("claude-old"));
        QVERIFY(reopened.completeProviderCleanup(session.id, session.generation, "claude",
                                                 "claude-old"));
        QVERIFY(reopened.deletionWork().isEmpty());
    }
    void unavailableProviderCleanupRunsAfterRestart() {
        QTemporaryDir directory;
        const auto executable = directory.filePath("codex");
        const auto log = directory.filePath("codex.log");
        const auto database = directory.filePath("journal.sqlite");
        {
            LocalJournal journal(database);
            StoredSession session;
            session.id = "retry";
            session.worktreePath = directory.path();
            session.adapter = "codex";
            session.threadId = "retry-provider-thread";
            session.state = "ready";
            QVERIFY(journal.upsertSession(session));
            CodexAdapter unavailable(directory.filePath("not-installed"));
            SessionHost host(&journal, &unavailable);
            const auto result = host.handleCommand({{"protocol_version", 0},
                                                    {"message_id", "delete"},
                                                    {"message_type", "session.delete"},
                                                    {"session_id", session.id},
                                                    {"session_generation", session.generation},
                                                    {"payload", QJsonObject{}}},
                                                   "desktop");
            QVERIFY(result.value("ok").toBool());
            QCoreApplication::processEvents();
            QCOMPARE(journal.deletionWork().size(), 1);
        }
        QVERIFY(writeFakeCodex(executable, log));
        LocalJournal reopened(database);
        CodexAdapter available(executable);
        SessionHost restarted(&reopened, &available);
        QTRY_VERIFY_WITH_TIMEOUT(reopened.deletionWork()
                                     .first()
                                     .toObject()
                                     .value("provider_targets")
                                     .toArray()
                                     .isEmpty(),
                                 5000);
        QFile calls(log);
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QVERIFY(calls.readAll().contains("delete:retry-provider-thread"));
        const auto catalog = restarted.handleCommand({{"protocol_version", 0},
                                                      {"message_id", "list"},
                                                      {"message_type", "session.list"},
                                                      {"payload", QJsonObject{}}},
                                                     "desktop");
        QCOMPARE(catalog.value("deleted_sessions").toArray().size(), 1);
    }
    // T-60: a provider without thread cleanup (Cursor, or one this daemon does not know)
    // no longer blocks deletion. The conversation's KetPlus data goes; its provider history,
    // outside KetPlus-owned paths, stays; a Codex thread it handed off from is still cleaned.
    void providerWithoutCleanupIsDeletedLocally_data() {
        QTest::addColumn<QString>("provider");
        QTest::newRow("cursor") << QStringLiteral("cursor");
        QTest::newRow("unknown") << QStringLiteral("future-agent");
    }
    void providerWithoutCleanupIsDeletedLocally() {
        QFETCH(QString, provider);
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        StoredSession session;
        session.id = provider + "-owned";
        session.worktreePath = directory.path();
        session.adapter = provider;
        session.threadId = provider + "-thread";
        session.state = "ready";
        session.providerThreads =
            QStringLiteral(R"({"codex":{"retired_thread_ids":["codex-before"]}})");
        QVERIFY(journal.upsertSession(session));
        // The provider's own store, standing in for ~/.cursor: never touched.
        const QString store = directory.filePath("provider-store/chat.json");
        QVERIFY(QDir().mkpath(QFileInfo(store).path()));
        QFile chat(store);
        QVERIFY(chat.open(QIODevice::WriteOnly));
        chat.write("{}");
        chat.close();
        // The conversation's handoff directory, which KetPlus owns.
        const QString handoff = directory.filePath("handoff/" + session.id);
        QVERIFY(QDir().mkpath(handoff));
        CodexAdapter adapter("/missing-provider");
        SessionHost host(&journal, &adapter);
        QSignalSpy events(&host, &SessionHost::eventReady);
        const auto result = host.handleCommand({{"protocol_version", 0},
                                                {"message_id", "delete"},
                                                {"message_type", "session.delete"},
                                                {"session_id", session.id},
                                                {"session_generation", session.generation},
                                                {"payload", QJsonObject{}}},
                                               "desktop");
        QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error_message").toString()));
        QVERIFY(journal.sessions().isEmpty());
        QVERIFY(!QFileInfo::exists(handoff));
        QVERIFY(QFileInfo::exists(store));
        QVERIFY(!events.isEmpty());
        QCOMPARE(events.last().first().toJsonObject().value("event_type").toString(),
                 QString("session.deleted"));
        // Only the Codex thread is queued; the provider's own thread is not.
        QCOMPARE(journal.deletionWork().size(), 1);
        const auto targets =
            journal.deletionWork().first().toObject().value("provider_targets").toArray();
        QCOMPARE(targets.size(), 1);
        QCOMPARE(targets.first().toObject().value("provider").toString(), QString("codex"));
        QCOMPARE(targets.first().toObject().value("thread_id").toString(), QString("codex-before"));
    }
    void newDeletionDefersWhenOwnershipCannotBeRead() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        StoredSession session;
        session.id = "first";
        session.generation = 1;
        session.adapter = "codex";
        session.threadId = "shared-thread";
        session.worktreePath = dir.path();
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        session.id = "second";
        QVERIFY(journal.upsertSession(session));
        CodexAdapter adapter("/missing-provider");
        SessionHost host(&journal, &adapter);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        QVERIFY(q.exec("ALTER TABLE sessions RENAME COLUMN provider_threads TO hidden_threads"));
        const auto result = host.handleCommand({{"protocol_version", 0},
                                                {"message_id", "delete-with-unreadable-ownership"},
                                                {"message_type", "session.delete"},
                                                {"session_id", "first"},
                                                {"session_generation", 1},
                                                {"payload", QJsonObject{}}},
                                               "desktop");
        QVERIFY(q.exec("ALTER TABLE sessions RENAME COLUMN hidden_threads TO provider_threads"));
        QVERIFY2(!result.value("ok").toBool(), QJsonDocument(result).toJson().constData());
        QCOMPARE(journal.sessions().size(), 2);
        QVERIFY(journal.sessionTombstone("first", 1).sessionId.isEmpty());
        QVERIFY(journal.deletionWork().isEmpty());
        const auto retry = host.handleCommand({{"protocol_version", 0},
                                               {"message_id", "delete-after-repair"},
                                               {"message_type", "session.delete"},
                                               {"session_id", "first"},
                                               {"session_generation", 1},
                                               {"payload", QJsonObject{}}},
                                              "desktop");
        QVERIFY(retry.value("ok").toBool());
        QCOMPARE(journal.sessions().size(), 1);
        QCOMPARE(journal.sessions().first().id, QString("second"));
        QVERIFY(journal.deletionWork()
                    .first()
                    .toObject()
                    .value("provider_targets")
                    .toArray()
                    .isEmpty());
    }
    void queuedCleanupDefersWhenOwnershipCannotBeRead_data() {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<QString>("malformed");
        QTest::newRow("query-error") << "query" << QString{};
        QTest::newRow("partial-row-error") << "partial" << QString{};
        for (const auto& entry : unsupportedOwnershipMetadata())
            QTest::newRow(qPrintable(entry.first)) << "metadata" << entry.second;
    }
    void queuedCleanupDefersWhenOwnershipCannotBeRead() {
        QFETCH(QString, fault);
        QFETCH(QString, malformed);
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        const auto executable = dir.filePath("codex");
        const auto log = dir.filePath("codex.log");
        QVERIFY(writeFakeCodex(executable, log));
        LocalJournal journal(path);
        StoredSession session;
        session.id = "deleted";
        session.generation = 1;
        session.adapter = "codex";
        session.threadId = "shared-thread";
        session.worktreePath = dir.path();
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        QString error;
        QVERIFY(journal.deleteSession(
            "deleted", 1, "op", 1, &error,
            {QJsonObject{{"provider", "codex"}, {"thread_id", "shared-thread"}}}));
        const QString historical = R"({"codex":{"retired_thread_ids":["shared-thread"]}})";
        session.id = "z-retained";
        session.threadId = "current-thread";
        session.providerThreads = historical;
        QVERIFY(journal.upsertSession(session));
        session.id = "a-readable";
        session.threadId = "unrelated-thread";
        session.providerThreads = "{}";
        QVERIFY(journal.upsertSession(session));
        CodexAdapter adapter(executable);
        bool warm = false;
        adapter.call("model/list", {}, [&](const QJsonObject&, const QString& e) {
            QVERIFY2(e.isEmpty(), qPrintable(e));
            warm = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(warm, 5000);
        QSqlDatabase db;
        for (const auto& name : QSqlDatabase::connectionNames())
            if (name.startsWith("ketplus-daemon-") &&
                QSqlDatabase::database(name).databaseName() == path)
                db = QSqlDatabase::database(name);
        QVERIFY(db.isOpen());
        QSqlQuery q(db);
        {
            SessionHost host(&journal, &adapter);
            if (fault == "partial") {
                QVERIFY(q.exec("PRAGMA table_info(sessions)"));
                QStringList columns;
                while (q.next()) {
                    const auto column = q.value(1).toString();
                    columns.append(
                        column == "provider_threads"
                            ? "CASE WHEN id='z-retained' THEN json_extract('invalid','$') "
                              "ELSE provider_threads END AS provider_threads"
                            : '"' + column + '"');
                }
                q.finish();
                QVERIFY(q.exec("ALTER TABLE sessions RENAME TO held_sessions"));
                QVERIFY(q.exec("CREATE VIEW sessions AS SELECT " + columns.join(',') +
                               " FROM held_sessions"));
            } else if (fault == "query") {
                QVERIFY(q.exec(
                    "ALTER TABLE sessions RENAME COLUMN provider_threads TO hidden_threads"));
            } else {
                q.prepare("UPDATE sessions SET provider_threads=? WHERE id='z-retained'");
                q.addBindValue(malformed);
                QVERIFY(q.exec());
            }
            // Run the queued maintenance, then a provider barrier. Any earlier
            // thread/delete must finish before that barrier response arrives.
            QCoreApplication::sendPostedEvents();
            bool barrier = false;
            adapter.call("model/list", {}, [&](const QJsonObject&, const QString& e) {
                QVERIFY2(e.isEmpty(), qPrintable(e));
                barrier = true;
            });
            QTRY_VERIFY_WITH_TIMEOUT(barrier, 5000);
            QFile calls(log);
            QVERIFY(calls.open(QIODevice::ReadOnly));
            const auto recorded = calls.readAll();
            QVERIFY2(!recorded.contains("delete:shared-thread"), recorded.constData());
            QCOMPARE(journal.deletionWork()
                         .first()
                         .toObject()
                         .value("provider_targets")
                         .toArray()
                         .size(),
                     1);
            if (fault == "partial") {
                QVERIFY(q.exec("DROP VIEW sessions"));
                QVERIFY(q.exec("ALTER TABLE held_sessions RENAME TO sessions"));
            } else if (fault == "query") {
                QVERIFY(q.exec(
                    "ALTER TABLE sessions RENAME COLUMN hidden_threads TO provider_threads"));
            } else {
                q.prepare("UPDATE sessions SET provider_threads=? WHERE id='z-retained'");
                q.addBindValue(historical);
                QVERIFY(q.exec());
            }
        }
        SessionHost restarted(&journal, &adapter);
        QTRY_VERIFY(journal.deletionWork()
                        .first()
                        .toObject()
                        .value("provider_targets")
                        .toArray()
                        .isEmpty());
        QFile calls(log);
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QVERIFY(!calls.readAll().contains("delete:shared-thread"));
        QCOMPARE(journal.sessions().size(), 2);
    }
    void localDeletionPreservesUnsupportedOwnershipMetadata_data() {
        QTest::addColumn<QString>("malformed");
        QTest::addColumn<bool>("corruptSource");
        for (const auto& entry : unsupportedOwnershipMetadata()) {
            QTest::newRow(qPrintable("source-" + entry.first)) << entry.second << true;
            QTest::newRow(qPrintable("retained-" + entry.first)) << entry.second << false;
        }
    }
    void localDeletionPreservesUnsupportedOwnershipMetadata() {
        QFETCH(QString, malformed);
        QFETCH(bool, corruptSource);
        QTemporaryDir dir;
        LocalJournal journal(dir.filePath("journal.sqlite"));
        StoredSession session;
        session.id = "first";
        session.generation = 1;
        session.adapter = "codex";
        session.threadId = "shared-thread";
        session.worktreePath = dir.path();
        session.state = "ready";
        session.providerThreads = corruptSource ? malformed : "{}";
        QVERIFY(journal.upsertSession(session));
        session.id = "second";
        session.threadId = "unrelated-current-thread";
        session.providerThreads = corruptSource ? "{}" : malformed;
        QVERIFY(journal.upsertSession(session));
        CodexAdapter adapter("/missing-provider");
        SessionHost host(&journal, &adapter);
        const auto result = host.handleCommand({{"protocol_version", 0},
                                                {"message_id", "delete-with-unsupported-ownership"},
                                                {"message_type", "session.delete"},
                                                {"session_id", "first"},
                                                {"session_generation", 1},
                                                {"payload", QJsonObject{}}},
                                               "desktop");
        QVERIFY2(!result.value("ok").toBool(), QJsonDocument(result).toJson().constData());
        QCOMPARE(journal.sessions().size(), 2);
        QCOMPARE(journal.sessions().at(corruptSource ? 0 : 1).providerThreads, malformed);
        QVERIFY(journal.sessionTombstone("first", 1).sessionId.isEmpty());
        QVERIFY(journal.deletionWork().isEmpty());
    }
    // Earlier daemons stored a read marker beside each provider thread. A conversation that
    // still has one neither blocks deleting another conversation nor loses its own threads.
    void legacySeenCursorDoesNotBlockDeletion() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        StoredSession first;
        first.id = "first";
        first.adapter = "codex";
        first.threadId = "first-thread";
        first.worktreePath = directory.path();
        first.state = "ready";
        first.providerThreads = R"({"claude":{"thread_id":"claude-old","seen_cursor":12}})";
        auto second = first;
        second.id = "second";
        second.threadId = "second-thread";
        second.providerThreads = R"({"claude":{"thread_id":"shared-old","seen_cursor":3}})";
        QVERIFY(journal.upsertSession(first));
        QVERIFY(journal.upsertSession(second));
        CodexAdapter adapter("/missing-provider");
        SessionHost host(&journal, &adapter);
        const auto result = host.handleCommand({{"protocol_version", 0},
                                                {"message_id", "delete"},
                                                {"message_type", "session.delete"},
                                                {"session_id", "first"},
                                                {"session_generation", first.generation},
                                                {"payload", QJsonObject{}}},
                                               "desktop");
        QVERIFY2(result.value("ok").toBool(), QJsonDocument(result).toJson().constData());
        QCOMPARE(journal.sessions().size(), 1);
        const auto targets =
            journal.deletionWork().first().toObject().value("provider_targets").toArray();
        QVERIFY(
            targets.contains(QJsonObject{{"provider", "codex"}, {"thread_id", "first-thread"}}));
        QVERIFY(targets.contains(QJsonObject{{"provider", "claude"}, {"thread_id", "claude-old"}}));
        QVERIFY(
            !targets.contains(QJsonObject{{"provider", "claude"}, {"thread_id", "shared-old"}}));
    }
    void sharedThreadsRemainOwnedByTheOtherConversation() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        StoredSession first;
        first.id = "first";
        first.adapter = "codex";
        first.threadId = "shared";
        first.worktreePath = directory.path();
        first.state = "ready";
        auto second = first;
        second.id = "second";
        QVERIFY(journal.upsertSession(first));
        QVERIFY(journal.upsertSession(second));
        CodexAdapter adapter("/missing-provider");
        SessionHost host(&journal, &adapter);
        const auto result = host.handleCommand({{"protocol_version", 0},
                                                {"message_id", "delete"},
                                                {"message_type", "session.delete"},
                                                {"session_id", "first"},
                                                {"session_generation", first.generation},
                                                {"payload", QJsonObject{}}},
                                               "desktop");
        QVERIFY(result.value("ok").toBool());
        QVERIFY(journal.deletionWork()
                    .first()
                    .toObject()
                    .value("provider_targets")
                    .toArray()
                    .isEmpty());
        QCOMPARE(journal.sessions().size(), 1);
    }
};
QTEST_GUILESS_MAIN(ProviderDeletionTest)
#include "ProviderDeletionTest.moc"
