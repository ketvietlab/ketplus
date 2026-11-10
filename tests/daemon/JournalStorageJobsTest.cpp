#include "runtime/JournalStorageJobs.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/JournalSpace.h"
#include "storage/LocalJournal.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
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
// One conversation with a few commands and one large generated image.
bool seed(LocalJournal& journal) {
    StoredSession session;
    session.id = "s";
    session.worktreePath = "synthetic";
    session.adapter = "codex";
    session.state = "ready";
    session.title = "Port the image pipeline";
    if (!journal.upsertSession(session))
        return false;
    const auto append = [&](const QString& type, const QJsonObject& data) {
        return journal.appendEvent("s", 1, type, data) > 0;
    };
    if (!append("user.message", {{"text", "draw a kite"}}) || !append("turn.started", {}))
        return false;
    for (int command = 0; command < 3; ++command)
        if (!append("item.completed",
                    {{"item", QJsonObject{{"id", QString("command-%1").arg(command)},
                                          {"type", "commandExecution"},
                                          {"command", "ls"},
                                          {"status", "completed"},
                                          {"aggregatedOutput", QString(2000, 'x')}}}}))
            return false;
    return append("item.completed",
                  {{"item", QJsonObject{{"id", "image-1"},
                                        {"type", "imageGeneration"},
                                        {"status", "completed"},
                                        {"revisedPrompt", "kite"},
                                        {"result", QString(1500 * 1024, 'A')}}}}) &&
           append("turn.completed", {});
}
} // namespace
class JournalStorageJobsTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void optionsAreRevisionedSeparateAndLocalOnly() {
        LocalJournal journal(":memory:");
        CodexAdapter adapter("/nonexistent/synthetic-codex");
        SessionHost host(&journal, &adapter);
        const auto read = host.handleCommand(request("storage.options.read"), "local");
        QVERIFY(read.value("ok").toBool());
        QCOMPARE(read.value("revision").toInteger(), 0);
        QCOMPARE(read.value("options").toObject(), (QJsonObject{{"archived_history_days", 90},
                                                                {"generated_images", "preview"},
                                                                {"warn_near_limit", true}}));
        const QJsonObject chosen{{"archived_history_days", 365},
                                 {"generated_images", "caption"},
                                 {"warn_near_limit", false}};
        for (const auto& invalid :
             {QJsonObject{{"archived_history_days", 45},
                          {"generated_images", "caption"},
                          {"warn_near_limit", false}},
              QJsonObject{{"archived_history_days", 30},
                          {"generated_images", "thumbnail"},
                          {"warn_near_limit", false}},
              QJsonObject{{"archived_history_days", 30}, {"generated_images", "full"}}})
            QCOMPARE(host.handleCommand(request("storage.options.apply",
                                                {{"expected_revision", 0}, {"options", invalid}}),
                                        "local")
                         .value("error_code"),
                     QJsonValue("invalid_policy"));
        const auto write =
            request("storage.options.apply", {{"expected_revision", 0}, {"options", chosen}});
        const auto saved = host.handleCommand(write, "local");
        QVERIFY(saved.value("ok").toBool());
        QCOMPARE(saved.value("revision").toInteger(), 1);
        QCOMPARE(host.handleCommand(write, "local"), saved); // the same request replays
        QCOMPARE(host.handleCommand(request("storage.options.apply",
                                            {{"expected_revision", 0}, {"options", chosen}}),
                                    "local")
                     .value("error_code"),
                 QJsonValue("policy_conflict"));
        QCOMPARE(host.handleCommand(request("storage.options.read"), "cloud-control-v2")
                     .value("error_code"),
                 QJsonValue("forbidden"));
        // Saving the detail policy leaves the options alone, as with the size policy.
        const auto retention = host.handleCommand(request("storage.retention.read"), "local");
        QVERIFY(retention.value("storage_options_supported").toBool());
        QVERIFY(
            host.handleCommand(request("storage.retention.apply",
                                       {{"expected_revision", 0},
                                        {"policy", JournalRetentionPolicy{7, true, 0}.toJson()}}),
                               "local")
                .value("ok")
                .toBool());
        const auto after = host.handleCommand(request("storage.options.read"), "local");
        QCOMPARE(after.value("revision").toInteger(), 1);
        QCOMPARE(after.value("options").toObject(), chosen);
    }

    void warningFollowsTheOptionAndCompactNowClearsIt() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal));
        CodexAdapter adapter("/nonexistent/synthetic-codex");
        SessionHost host(&journal, &adapter);
        QVERIFY(journal.setSizePolicy({1'000'000'000LL, 80, 0}, 0));
        QVERIFY(sql(path, "CREATE TABLE storage_fixture(body BLOB)"));
        QVERIFY(sql(path, "INSERT INTO storage_fixture VALUES(zeroblob(850000000))"));
        const auto status = [&] {
            return host.handleCommand(request("storage.size.read"), "local")
                .value("status")
                .toObject();
        };
        QVERIFY(status().value("warning").toBool());
        QVERIFY(!status().value("over_limit").toBool());
        QVERIFY(status().contains("low_disk"));
        // The near-limit warning can be turned off; it reaches remote clients too.
        JournalStorageOptions quiet;
        quiet.warnNearLimit = false;
        QVERIFY(journal.setStorageOptions(quiet, 0));
        QVERIFY(!status().value("warning").toBool());
        QVERIFY(sql(path, "INSERT INTO storage_fixture VALUES(zeroblob(200000000))"));
        QVERIFY(status().value("over_limit").toBool());
        QVERIFY(status().value("warning").toBool()); // over the limit always warns
        const auto catalog = host.handleCommand(request("session.list"), "cloud-control-v2");
        QVERIFY(catalog.value("journal_storage").toObject().value("over_limit").toBool());

        // Deleted rows leave free pages; Compact now returns them and the warning clears.
        QVERIFY(sql(path, "DROP TABLE storage_fixture"));
        const auto before = journal.space()->pageCount * journal.space()->pageSize;
        const auto started = host.handleCommand(request("storage.compaction.start"), "local");
        QVERIFY(started.value("ok").toBool());
        QCOMPARE(started.value("compaction").toObject().value("state"), QJsonValue("running"));
        QTRY_VERIFY_WITH_TIMEOUT(host.handleCommand(request("storage.compaction.read"), "local")
                                         .value("compaction")
                                         .toObject()
                                         .value("state") == QJsonValue("idle"),
                                 120000);
        const auto done = host.handleCommand(request("storage.size.read"), "local");
        const auto compaction = done.value("compaction").toObject();
        QCOMPARE(compaction.value("outcome"), QJsonValue("done"));
        QVERIFY(compaction.value("database_bytes").toInteger() < 100'000'000LL);
        QCOMPARE(compaction.value("before_bytes").toInteger(), before);
        const auto last = compaction.value("last").toObject();
        QVERIFY(QDateTime::fromString(last.value("completed_at").toString(), Qt::ISODateWithMs)
                    .isValid());
        QCOMPARE(last.value("before_bytes").toInteger(), before);
        QVERIFY(!done.value("status").toObject().value("warning").toBool());
        // Messages and the picture survive: nothing here was past its detail period.
        QCOMPARE(journal.compactedEventsAfter("s", 1, 0, 500).events.size(), 7);
    }

    void compactionWaitsForRunningTurns() {
        LocalJournal journal(":memory:");
        bool busy = true;
        int expiries = 0;
        JournalStorageJobs jobs(&journal, {[&] { return busy; },
                                           [&](const QDateTime&) {
                                               ++expiries;
                                               return false;
                                           }});
        QVERIFY(jobs.handle("storage.compaction.start", {}).value("ok").toBool());
        QTRY_COMPARE(jobs.compactionStatus().value("state"), QJsonValue("waiting_for_idle"));
        QTest::qWait(1500);
        QCOMPARE(expiries, 0); // nothing ran while the turn was running
        busy = false;
        QTRY_COMPARE_WITH_TIMEOUT(jobs.compactionStatus().value("state"), QJsonValue("idle"),
                                  30000);
        QCOMPARE(jobs.compactionStatus().value("outcome"), QJsonValue("done"));
        QVERIFY(expiries > 0);
        QCOMPARE(jobs.handle("storage.compaction.start", {{"now", true}}).value("error_code"),
                 QJsonValue("invalid_request"));
    }

    void breakdownMeasuresThePartsOffTheEventLoop() {
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal));
        CodexAdapter adapter("/nonexistent/synthetic-codex");
        SessionHost host(&journal, &adapter);
        const auto first = host.handleCommand(request("storage.breakdown.read"), "local");
        QVERIFY(first.value("ok").toBool());
        QCOMPARE(first.value("state"), QJsonValue("measuring"));
        QJsonObject result;
        QTRY_VERIFY_WITH_TIMEOUT(
            (result = host.handleCommand(request("storage.breakdown.read"), "local"))
                    .value("state") == QJsonValue("ready"),
            30000);
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        const auto breakdown = result.value("breakdown").toObject();
        const auto parts = breakdown.value("parts").toObject();
        QVERIFY(parts.value("image_bytes").toInteger() >= 1500 * 1024);
        QCOMPARE(parts.value("image_count").toInteger(), 1);
        QVERIFY(parts.value("history_bytes").toInteger() > 0);
        QCOMPARE(parts.value("history_bytes").toInteger() + parts.value("image_bytes").toInteger() +
                     parts.value("activity_bytes").toInteger() +
                     parts.value("free_bytes").toInteger(),
                 breakdown.value("database_page_bytes").toInteger());
        const auto largest = breakdown.value("largest").toArray();
        QCOMPARE(largest.size(), 1);
        QCOMPARE(largest.first().toObject().value("title"), QJsonValue("Port the image pipeline"));
        QCOMPARE(largest.first().toObject().value("note"), QJsonValue("1 generated image"));
        QVERIFY(breakdown.contains("oldest_event_at"));
    }

    void exportWritesOneOwnerOnlyFilePerConversation() {
        QTemporaryDir directory;
        QTemporaryDir target;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal));
        CodexAdapter adapter("/nonexistent/synthetic-codex");
        SessionHost host(&journal, &adapter);
        QCOMPARE(host.handleCommand(request("storage.export.start", {{"directory", "relative"}}),
                                    "local")
                     .value("error_code"),
                 QJsonValue("invalid_request"));
        QVERIFY(host.handleCommand(request("storage.export.start", {{"directory", target.path()}}),
                                   "local")
                    .value("ok")
                    .toBool());
        QJsonObject status;
        QTRY_VERIFY_WITH_TIMEOUT(
            (status = host.handleCommand(request("storage.export.read"), "local")
                          .value("export")
                          .toObject())
                    .value("state") != QJsonValue("running"),
            30000);
        QCOMPARE(status.value("state"), QJsonValue("done"));
        QCOMPARE(status.value("exported").toInt(), 1);
        const QDir folder(status.value("directory").toString());
        QVERIFY(folder.path().startsWith(target.path()));
        const auto files = folder.entryList({"*.json"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        QVERIFY(files.first().endsWith("-port-the-image-pipeline.json"));
        QFile file(folder.filePath(files.first()));
        QCOMPARE(file.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
                 QFileDevice::Permissions{});
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto exported = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(exported.value("format"), QJsonValue("ketplus.conversation-export/1"));
        QCOMPARE(exported.value("conversation").toObject().value("title"),
                 QJsonValue("Port the image pipeline"));
        bool image = false;
        for (const auto& value : exported.value("events").toArray()) {
            const auto item = value.toObject().value("payload").toObject().value("item").toObject();
            if (item.value("type") == "imageGeneration") {
                image = true; // the large item is put back together
                QCOMPARE(item.value("result").toString().size(), 1500 * 1024);
            }
        }
        QVERIFY(image);
    }

    // The journal size check in docs/testing.md. It runs only when KETPLUS_JOURNAL_SIZE_CHECK
    // names a copy of a real journal, and it compacts that copy: never point it at a live one.
    void journalCopyShrinksOnFirstCompaction() {
        const auto path = qEnvironmentVariable("KETPLUS_JOURNAL_SIZE_CHECK");
        if (path.isEmpty())
            QSKIP("Set KETPLUS_JOURNAL_SIZE_CHECK to a copy of a journal to run the size check.");
        const auto gb = [](qint64 bytes) { return double(bytes) / 1e9; };
        const auto fileBefore = QFileInfo(path).size();
        QElapsedTimer timer;
        timer.start();
        // As at daemon start: the one-time move to incremental vacuum comes first.
        const auto migration = journal_space::migrateToIncrementalVacuum(path);
        QVERIFY2(migration.outcome == journal_space::Migration::Migrated ||
                     migration.outcome == journal_space::Migration::NotNeeded,
                 qPrintable(migration.error));
        qInfo("start migration: %.2f GB file -> %.2f GB in %.0f s", gb(fileBefore),
              gb(QFileInfo(path).size()), timer.elapsed() / 1000.0);
        LocalJournal journal(path);
        CodexAdapter adapter("/nonexistent/size-check-codex");
        SessionHost host(&journal, &adapter);
        const auto pages = [&] { return journal.space()->pageCount * journal.space()->pageSize; };
        const auto compact = [&] {
            timer.restart();
            QVERIFY(host.handleCommand(request("storage.compaction.start"), "local")
                        .value("ok")
                        .toBool());
            QTRY_VERIFY_WITH_TIMEOUT(host.handleCommand(request("storage.compaction.read"), "local")
                                             .value("compaction")
                                             .toObject()
                                             .value("state") == QJsonValue("idle"),
                                     3'600'000);
            const auto compaction = host.handleCommand(request("storage.compaction.read"), "local")
                                        .value("compaction")
                                        .toObject();
            QCOMPARE(compaction.value("outcome"), QJsonValue("done"));
            qInfo("compaction: %.2f GB -> %.2f GB in %.0f s",
                  gb(compaction.value("before_bytes").toInteger()), gb(pages()),
                  timer.elapsed() / 1000.0);
        };
        compact();
        if (QTest::currentTestFailed())
            return;
        const auto first = pages();
        QVERIFY2(first < 4'000'000'000LL, qPrintable(QString::number(gb(first))));
        compact(); // and it stays there
        QVERIFY(pages() <= first);
        const auto status =
            host.handleCommand(request("storage.size.read"), "local").value("status").toObject();
        QVERIFY(!status.value("over_limit").toBool());
    }
};
QTEST_GUILESS_MAIN(JournalStorageJobsTest)
#include "JournalStorageJobsTest.moc"
