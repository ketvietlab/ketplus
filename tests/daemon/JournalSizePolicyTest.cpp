#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
using namespace ketplus::daemon;
namespace {
bool sql(const QString& path, const QString& statement) {
    const auto name = QUuid::createUuid().toString();
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery query(db);
            ok = query.exec(statement);
            if (!ok)
                qWarning() << query.lastError().text();
        }
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}
QJsonObject request(const QString& type, const QJsonObject& payload = {}) {
    return {{"message_type", type},
            {"message_id", QUuid::createUuid().toString()},
            {"payload", payload}};
}
int marked(LocalJournal& journal) {
    int count = 0;
    for (const auto& value : journal.compactedEventsAfter("s", 1, 0, 500).events)
        if (value.toObject().value("payload").toObject().value("item").toObject().contains(
                "compacted"))
            ++count;
    return count;
}
} // namespace
class JournalSizePolicyTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void validatesPersistsAndFencesWrites() {
        JournalSizePolicy defaults;
        QCOMPARE(defaults.limitBytes, 5'000'000'000LL);
        QCOMPARE(defaults.warningPercent, 80);
        for (qint64 limit :
             {0LL, 1'000'000'000LL, 2'000'000'000LL, 5'000'000'000LL, 10'000'000'000LL})
            QVERIFY(JournalSizePolicy::fromJson(JournalSizePolicy{limit, 80, 0}.toJson()));
        for (const auto value : {QJsonValue(-1), QJsonValue(3'000'000'000LL), QJsonValue(2.5),
                                 QJsonValue("1000000000"), QJsonValue(true)}) {
            auto json = defaults.toJson();
            json.insert("limit_bytes", value);
            QVERIFY(!JournalSizePolicy::fromJson(json));
        }
        for (const auto value :
             {QJsonValue(0), QJsonValue(101), QJsonValue(2.5), QJsonValue("80")}) {
            auto json = defaults.toJson();
            json.insert("warning_percent", value);
            QVERIFY(!JournalSizePolicy::fromJson(json));
        }
        auto unknown = defaults.toJson();
        unknown.insert("extra", true);
        QVERIFY(!JournalSizePolicy::fromJson(unknown));
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        {
            LocalJournal journal(path);
            QCOMPARE(journal.sizePolicy()->revision, 0);
            QVERIFY(journal.setSizePolicy({1'000'000'000LL, 90, 0}, 0));
            QVERIFY(!journal.setSizePolicy(defaults, 0));
            QCOMPARE(journal.sizePolicy()->revision, 1);
            QVERIFY(sql(path, "CREATE TRIGGER reject_size BEFORE UPDATE ON journal_size_settings "
                              "BEGIN SELECT RAISE(ABORT,'synthetic'); END"));
            QVERIFY(!journal.setSizePolicy(defaults, 1));
            QCOMPARE(journal.sizePolicy()->limitBytes, 1'000'000'000LL);
            QVERIFY(sql(path, "DROP TRIGGER reject_size"));
        }
        LocalJournal reopened(path);
        QCOMPARE(reopened.sizePolicy()->warningPercent, 90);
        QVERIFY(sql(path, "UPDATE journal_size_settings SET policy='{}'"));
        QVERIFY(!reopened.sizePolicy());
        QVERIFY(!reopened.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()));
    }
    void ownerApiPreservesRetentionAndRejectsRemoteCacheReplay() {
        LocalJournal journal(":memory:");
        CodexAdapter adapter("/nonexistent/synthetic-codex");
        SessionHost host(&journal, &adapter);
        const auto read = host.handleCommand(request("storage.size.read"), "local");
        QVERIFY(read.value("ok").toBool());
        // The default 5 GB target is enforced without a save (ADR 0108).
        QVERIFY(read.value("status").toObject().value("pressure_enabled").toBool());
        QCOMPARE(read.value("policy").toObject().value("limit_bytes").toInteger(), 5'000'000'000LL);
        QCOMPARE(read.value("status").toObject().value("target_scope"), QJsonValue("sqlite_pages"));
        const auto write =
            request("storage.size.apply",
                    {{"expected_revision", 0}, {"policy", JournalSizePolicy{0, 95, 0}.toJson()}});
        const auto saved = host.handleCommand(write, "local");
        QVERIFY(saved.value("ok").toBool());
        QCOMPARE(saved.value("revision").toInteger(), 1);
        QVERIFY(!saved.value("status").toObject().value("pressure_enabled").toBool());
        QVERIFY(!saved.value("status").toObject().value("warning").toBool());
        QCOMPARE(host.handleCommand(write, "local"), saved);
        QCOMPARE(host.handleCommand(write, "cloud-control-v2").value("error_code"),
                 QJsonValue("forbidden"));
        QCOMPARE(host.handleCommand(request("storage.size.read"), "cloud-control-v2")
                     .value("error_code"),
                 QJsonValue("forbidden"));
        QCOMPARE(host.handleCommand(
                         request("storage.size.apply", {{"expected_revision", 1.5},
                                                        {"policy", JournalSizePolicy{}.toJson()}}),
                         "local")
                     .value("error_code"),
                 QJsonValue("invalid_policy"));
        QCOMPARE(journal.retentionPolicy()->fullDetailDays, 30);
        QVERIFY(journal.setRetentionPolicy({7, true, 0}, 0));
        QCOMPARE(journal.sizePolicy()->limitBytes, 0);
        QCOMPARE(host.handleCommand(
                         request("storage.size.apply", {{"expected_revision", 0},
                                                        {"policy", JournalSizePolicy{}.toJson()}}),
                         "local")
                     .value("error_code"),
                 QJsonValue("policy_conflict"));
        QCOMPARE(host.handleCommand(request("storage.size.read", {{"unknown", true}}), "local")
                     .value("error_code"),
                 QJsonValue("invalid_request"));
    }
    void pressureKeepsPromisesAndGuardsAndCompletesExpiredPassFirst() {
        const auto root = QCoreApplication::applicationDirPath();
        if (QStorageInfo(root).bytesAvailable() < 4LL * 1024 * 1024 * 1024)
            QSKIP("Synthetic 1.02 GB database needs 4 GiB free; no installed journal is used.");
        QTemporaryDir directory(root + "/journal-pressure-XXXXXX");
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("journal.sqlite");
        {
            LocalJournal journal(path);
            StoredSession session;
            session.id = "s";
            session.adapter = "codex";
            session.state = "ready";
            session.generation = 1;
            session.worktreePath = "synthetic";
            QVERIFY(journal.upsertSession(session));
            for (int turn = 1; turn <= 24; ++turn) {
                QVERIFY(journal.appendEvent("s", 1, "user.message", {{"text", "keep"}}) > 0);
                QVERIFY(journal.appendEvent("s", 1, "turn.started", {}) > 0);
                QVERIFY(journal.appendEvent("s", 1, "item.completed",
                                            {{"item", QJsonObject{{"id", QString::number(turn)},
                                                                  {"type", "commandExecution"},
                                                                  {"status", "completed"},
                                                                  {"aggregatedOutput",
                                                                   QString(60000, 'x')}}}}) > 0);
                QVERIFY(journal.appendEvent("s", 1, "turn.completed", {}) > 0);
            }
            // Force each sweep across the 128-row boundary without changing eligibility.
            for (int sample = 0; sample < 45; ++sample)
                QVERIFY(journal.appendEvent("s", 1, "usage.context", {{"sample", sample}}) > 0);
            QVERIFY(sql(path, "DELETE FROM events WHERE turn_sequence!=3"));
            QVERIFY(sql(path,
                        "UPDATE replay_events SET sent_at=strftime('%Y-%m-%dT%H:%M:%fZ','now','-45 "
                        "days') WHERE turn_sequence=1"));
            QVERIFY(sql(path, "UPDATE replay_events SET event_type='question.requested' WHERE "
                              "turn_sequence=4 AND event_type='user.message'"));
            QVERIFY(sql(path, "CREATE TABLE pressure_fixture(body BLOB)"));
            QVERIFY(sql(
                path,
                "INSERT INTO pressure_fixture VALUES(zeroblob(510000000)),(zeroblob(510000000))"));
            QVERIFY(sql(path, "PRAGMA wal_checkpoint(TRUNCATE)"));
            QVERIFY(journal.space()->pageCount * journal.space()->pageSize > 1'000'000'000LL);
            QVERIFY(journal.setSizePolicy({1'000'000'000LL, 80, 0}, 0));
            QVERIFY(journal.setRetentionPolicy({0, true, 0}, 0));
            QCOMPARE(
                journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()).value_or(-1),
                0);
            QVERIFY(journal.setRetentionPolicy({30, false, 0}, 1));
            QCOMPARE(
                journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()).value_or(-1),
                0);
            QCOMPARE(marked(journal), 0);
            QVERIFY(journal.setRetentionPolicy({30, true, 0}, 2));
            QVERIFY(sql(path, "UPDATE sessions SET state='running'"));
            QCOMPARE(
                journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()).value_or(-1),
                0);
            QVERIFY(sql(path, "UPDATE sessions SET state='ready'"));
            QVERIFY(journal.setSizePolicy({1'000'000'000LL, 80, 0}, 1)); // restart expired sweep
            QElapsedTimer pressureTimer;
            QVERIFY(
                sql(path,
                    "CREATE TRIGGER abort_pressure BEFORE UPDATE OF revision ON "
                    "replay_projection_state BEGIN SELECT RAISE(ABORT,'synthetic rollback'); END"));
            QVERIFY(!journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()));
            QCOMPARE(marked(journal), 0);
            QVERIFY(sql(path, "DROP TRIGGER abort_pressure"));
            pressureTimer.start();
            QCOMPARE(
                journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()).value_or(-1),
                1);
            QCOMPARE(marked(journal), 1); // only old turn 1; fresh turn 2 still full
            QCOMPARE(
                journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()).value_or(-1),
                0);
            QCOMPARE(marked(journal), 1); // finish expired sweep before starting any recent detail
            QCOMPARE(
                journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()).value_or(-1),
                1);
            QCOMPARE(marked(journal),
                     2); // turn 2; raw-unacknowledged turn 3 and question turn 4 stay
            qInfo() << "Expired and recent pressure passes on 1.02 GB fixture:"
                    << pressureTimer.elapsed() << "ms";
            QCOMPARE(
                journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()).value_or(-1),
                0);
            QVERIFY(sql(path, "DELETE FROM events"));
            QVERIFY(sql(path, "UPDATE replay_events SET event_type='user.message' WHERE "
                              "event_type='question.requested'"));
            QVERIFY(journal.setSizePolicy({1'000'000'000LL, 80, 0}, 2));
            QCOMPARE(
                journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()).value_or(-1),
                0);
            QCOMPARE(
                journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()).value_or(-1),
                0);
            QCOMPARE(
                journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()).value_or(-1),
                2);
            QCOMPARE(marked(journal), 4); // latest twenty untouched
            CodexAdapter adapter("/nonexistent/synthetic-codex");
            SessionHost host(&journal, &adapter);
            const auto status = host.handleCommand(request("storage.size.read"), "local")
                                    .value("status")
                                    .toObject();
            QVERIFY(status.value("over_limit").toBool()); // unsupported fixture keeps target unmet
            QVERIFY(status.value("warning").toBool());
            QVERIFY(status.value("pressure_enabled").toBool());
            QVERIFY(journal.setSizePolicy({0, 80, 0}, 3));
            QVERIFY(journal.appendEvent("s", 1, "user.message", {{"text", "new turn"}}) > 0);
            QVERIFY(journal.appendEvent("s", 1, "turn.started", {}) > 0);
            QVERIFY(journal.appendEvent("s", 1, "turn.completed", {}) > 0);
            QVERIFY(sql(path, "DELETE FROM events"));
            for (int pass = 0; pass < 4; ++pass)
                QCOMPARE(journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc())
                             .value_or(-1),
                         0);
            QCOMPARE(marked(journal), 4); // newly eligible recent turn 5 stays full under no-limit
            QVERIFY(sql(path, "DELETE FROM pressure_fixture WHERE rowid=1"));
            QVERIFY(sql(path,
                        "UPDATE replay_events SET payload=json_remove(payload,'$.item.compacted') "
                        "WHERE turn_sequence=1 AND event_type='item.completed'"));
            QVERIFY(sql(path,
                        "UPDATE replay_events SET "
                        "payload=json_set(payload,'$.item.aggregatedOutput',printf('%60000s','x')"
                        ") WHERE turn_sequence=1 AND event_type='item.completed'"));
            QVERIFY(journal.space()->freePages >= LocalJournal::minimumReclaimPages);
            QVERIFY(journal.setSizePolicy({1'000'000'000LL, 80, 0}, 4));
            QCOMPARE(
                journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()).value_or(-1),
                0);
            QCOMPARE(marked(journal), 3); // reclaim existing free pages before removing more detail
        }
        QVERIFY(directory.remove());
        QVERIFY(!QFileInfo::exists(path));
    }
};
QTEST_GUILESS_MAIN(JournalSizePolicyTest)
#include "JournalSizePolicyTest.moc"
