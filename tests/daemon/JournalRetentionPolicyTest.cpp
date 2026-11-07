#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include <QJsonDocument>
#include <QSqlQuery>
#include <QStandardPaths>
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
        }
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}
bool seed(LocalJournal& journal, const QString& path, int turns = 21) {
    StoredSession session;
    session.id = "s";
    session.adapter = "codex";
    session.state = "ready";
    session.generation = 1;
    session.worktreePath = "synthetic";
    if (!journal.upsertSession(session))
        return false;
    qint64 cursor = 0;
    for (int turn = 0; turn < turns; ++turn) {
        if (journal.appendEvent("s", 1, "user.message", {{"text", "synthetic"}}) <= 0 ||
            journal.appendEvent("s", 1, "turn.started", {}) <= 0 ||
            journal.appendEvent("s", 1, "usage.context", {{"sample", 1}}) <= 0 ||
            journal.appendEvent("s", 1, "usage.context", {{"sample", 2}}) <= 0)
            return false;
        cursor = journal.appendEvent("s", 1, "turn.completed", {});
        if (cursor <= 0)
            return false;
    }
    return journal.acknowledgeCursor("client", "s", 1, cursor) &&
           sql(path, "UPDATE replay_events SET sent_at=strftime('%Y-%m-%dT%H:%M:%fZ','now','-45 "
                     "days') WHERE turn_sequence=1");
}
QJsonObject request(const QString& type, const QJsonObject& payload = {}) {
    return {{"message_type", type},
            {"message_id", QUuid::createUuid().toString()},
            {"payload", payload}};
}
} // namespace
class JournalRetentionPolicyTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void strictValidation() {
        const JournalRetentionPolicy defaults;
        QCOMPARE(defaults.fullDetailDays, 30);
        QVERIFY(defaults.automaticDetailRetention);
        for (const auto& invalid : QJsonArray{-1, 1, 7.5, 365, "30", true, QJsonValue()}) {
            auto value = defaults.toJson();
            value.insert("full_detail_days", invalid);
            QVERIFY(!JournalRetentionPolicy::fromJson(value));
        }
        auto value = defaults.toJson();
        value.insert("automatic_detail_retention", 1);
        QVERIFY(!JournalRetentionPolicy::fromJson(value));
        value = defaults.toJson();
        value.insert("unknown", true);
        QVERIFY(!JournalRetentionPolicy::fromJson(value));
        QVERIFY(!JournalRetentionPolicy::fromJson({}));
        QVERIFY(!defaults.cutoff({}));
    }
    void persistsAndRejectsStaleOrFailedWrites() {
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        {
            LocalJournal journal(path);
            QVERIFY(journal.retentionPolicy());
            QCOMPARE(journal.retentionPolicy()->revision, 0);
            QVERIFY(journal.setRetentionPolicy({90, false, 0}, 0));
            QVERIFY(!journal.setRetentionPolicy({7, true, 0}, 0));
            QCOMPARE(journal.retentionPolicy()->fullDetailDays, 90);
            QCOMPARE(journal.retentionPolicy()->revision, 1);
            QVERIFY(!journal.setRetentionPolicy({31, true, 0}, 1));
            QVERIFY(sql(path,
                        "CREATE TRIGGER reject_policy BEFORE UPDATE ON journal_retention_settings "
                        "BEGIN SELECT RAISE(ABORT,'synthetic failure'); END"));
            QVERIFY(!journal.setRetentionPolicy({7, true, 0}, 1));
            QCOMPARE(journal.retentionPolicy()->revision, 1);
            QVERIFY(sql(path, "DROP TRIGGER reject_policy"));
        }
        LocalJournal reopened(path);
        const auto saved = reopened.retentionPolicy();
        QVERIFY(saved);
        QCOMPARE(saved->fullDetailDays, 90);
        QVERIFY(!saved->automaticDetailRetention);
        QCOMPARE(saved->revision, 1);
        QVERIFY(reopened.setRetentionPolicy({7, true, 0}, 1));
        QCOMPARE(reopened.retentionPolicy()->revision, 2);
    }
    void configuredRetention_data() {
        QTest::addColumn<int>("days");
        QTest::addColumn<bool>("enabled");
        QTest::newRow("7-days") << 7 << true;
        QTest::newRow("30-days") << 30 << true;
        QTest::newRow("90-days") << 90 << true;
        QTest::newRow("forever") << 0 << true;
        QTest::newRow("disabled") << 30 << false;
    }
    void configuredRetention() {
        QFETCH(int, days);
        QFETCH(bool, enabled);
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        QVERIFY(journal.setRetentionPolicy({days, enabled, 0}, 0));
        const auto before = journal.compactedEventsAfter("s", 1, 0, 500).events;
        const qint64 expected = enabled && days > 0 && days < 45 ? 1 : 0;
        QCOMPARE(journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()),
                 std::optional<qint64>(expected));
        const auto after = journal.compactedEventsAfter("s", 1, 0, 500).events;
        QCOMPARE(before.size() - after.size(), expected);
        if (!expected)
            QCOMPARE(after, before);
        QVERIFY(!journal.pruneConfiguredReplayDetails({}));
    }
    void changedPolicyRestartsTheBoundedSweep() {
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, 40)); // more rows than one bounded pass
        QVERIFY(journal.setRetentionPolicy({90, true, 0}, 0));
        QCOMPARE(journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()),
                 std::optional<qint64>(0));
        QVERIFY(journal.setRetentionPolicy({7, true, 0}, 1));
        QCOMPARE(journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()),
                 std::optional<qint64>(1));
    }
    void corruptPolicyFailsClosed_data() {
        QTest::addColumn<QString>("value");
        QTest::newRow("malformed-json") << QString("not-json");
        QTest::newRow("missing-fields") << QString("{}");
        QTest::newRow("unsupported-setting")
            << QString("{\"full_detail_days\":1,\"automatic_detail_retention\":true}");
    }
    void corruptPolicyFailsClosed() {
        QFETCH(QString, value);
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        QVERIFY(journal.setRetentionPolicy({}, 0));
        // Synthetic values contain no SQL quotes.
        QVERIFY(sql(path, "UPDATE journal_retention_settings SET policy='" + value + "'"));
        QVERIFY(!journal.retentionPolicy());
        const auto before = journal.compactedEventsAfter("s", 1, 0, 500).events;
        QVERIFY(!journal.pruneConfiguredReplayDetails(QDateTime::currentDateTimeUtc()));
        QCOMPARE(journal.compactedEventsAfter("s", 1, 0, 500).events, before);
    }
    void localApiUsesRevisionAndRejectsRemoteBeforeCache() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite"));
        CodexAdapter adapter("/nonexistent/synthetic-codex");
        SessionHost host(&journal, &adapter);
        const auto read = host.handleCommand(request("storage.retention.read"), "local");
        QVERIFY(read.value("ok").toBool());
        QCOMPARE(read.value("revision").toInteger(), 0);
        QVERIFY(read.value("usage").toObject().value("database_page_bytes").toInteger() > 0);
        QVERIFY(read.value("disk_usage").toObject().value("total_file_bytes").toInteger() > 0);
        QVERIFY(!QJsonDocument(read).toJson().contains(directory.path().toUtf8()));
        const auto write = request(
            "storage.retention.apply",
            {{"expected_revision", 0}, {"policy", JournalRetentionPolicy{7, true, 0}.toJson()}});
        const auto saved = host.handleCommand(write, "local");
        QVERIFY(saved.value("ok").toBool());
        QCOMPARE(saved.value("revision").toInteger(), 1);
        QCOMPARE(host.handleCommand(write, "local"), saved);
        QCOMPARE(host.handleCommand(write, "cloud-control-v2").value("error_code"), "forbidden");
        QCOMPARE(host.handleCommand(request("storage.retention.read"), "cloud-control-v2")
                     .value("error_code"),
                 "forbidden");
        auto stale = write;
        stale.insert("message_id", "fresh-stale");
        QCOMPARE(host.handleCommand(stale, "local").value("error_code"), "policy_conflict");
        QCOMPARE(journal.retentionPolicy()->revision, 1);
        const auto invalid =
            request("storage.retention.apply",
                    {{"expected_revision", 1.5}, {"policy", JournalRetentionPolicy{}.toJson()}});
        QCOMPARE(host.handleCommand(invalid, "local").value("error_code"), "invalid_policy");
        QCOMPARE(host.handleCommand(request("storage.retention.read", {{"unknown", true}}), "local")
                     .value("error_code"),
                 "invalid_request");
    }
};
QTEST_GUILESS_MAIN(JournalRetentionPolicyTest)
#include "JournalRetentionPolicyTest.moc"
