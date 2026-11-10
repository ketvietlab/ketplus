#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include "storage/McpMediaStore.h"
#include <QBuffer>
#include <QImage>

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSemaphore>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <thread>

// Internal link seam: test the production GC predicate, not a copied SQL approximation.
namespace ketplus::daemon::detail {
QString largePayloadUnreferenced();
}

using namespace ketplus::daemon;

namespace {
class Database {
  public:
    explicit Database(const QString& path) : name_(QUuid::createUuid().toString()) {
        db_ = QSqlDatabase::addDatabase("QSQLITE", name_);
        db_.setDatabaseName(path);
        if (!db_.open())
            qFatal("Fixture database cannot open");
    }
    ~Database() {
        db_.close();
        db_ = {};
        QSqlDatabase::removeDatabase(name_);
    }
    bool exec(const QString& statement) {
        QSqlQuery query(db_);
        if (query.exec(statement))
            return true;
        qWarning() << query.lastError();
        return false;
    }
    bool patchItems(const QJsonObject& fields, const QString& eventType = "item.completed") {
        QSqlQuery query(db_);
        query.prepare("UPDATE replay_events SET payload=json_patch(payload,?) WHERE "
                      "event_type=?");
        query.addBindValue(QString::fromUtf8(
            QJsonDocument(QJsonObject{{"item", fields}}).toJson(QJsonDocument::Compact)));
        query.addBindValue(eventType);
        return query.exec();
    }
    QString queryPlan(const QString& statement, const QVariantList& values = {}) {
        QSqlQuery query(db_);
        query.prepare("EXPLAIN QUERY PLAN " + statement);
        for (const auto& value : values)
            query.addBindValue(value);
        if (!query.exec())
            return query.lastError().text();
        QStringList details;
        while (query.next())
            details.append(query.value(3).toString());
        return details.join('\n');
    }
    int rows(const QString& statement, const QVariantList& values) {
        QSqlQuery query(db_);
        query.prepare(statement);
        for (const auto& value : values)
            query.addBindValue(value);
        if (!query.exec())
            return -1;
        int count = 0;
        while (query.next())
            ++count;
        return query.lastError().isValid() ? -1 : count;
    }
    qint64 scalar(const QString& statement) {
        QSqlQuery query(db_);
        if (!query.exec(statement) || !query.next())
            return -1;
        return query.value(0).toLongLong();
    }

  private:
    QString name_;
    QSqlDatabase db_;
};

QDateTime cutoff() { return QDateTime::currentDateTimeUtc().addDays(-30); }

bool drainAcknowledged(LocalJournal& journal, Database& database) {
    for (int pass = 0; pass < 2048 && database.scalar("SELECT count(*) FROM events") > 0; ++pass)
        if (!journal.pruneAcknowledgedEvents())
            return false;
    return database.scalar("SELECT count(*) FROM events") == 0;
}

bool seed(LocalJournal& journal, const QString& path, bool acknowledge = true, int turns = 24,
          const QString& output = {}, bool largeTool = false, bool generatedImage = false,
          bool toolImage = false) {
    StoredSession session;
    session.id = "session";
    session.worktreePath = "synthetic";
    session.adapter = "codex";
    session.state = "ready";
    if (!journal.upsertSession(session))
        return false;
    qint64 cursor = 0;
    const auto append = [&](const QString& type, const QJsonObject& data) {
        cursor = journal.appendEvent("session", 1, type, data);
        return cursor > 0;
    };
    for (int turn = 1; turn <= turns; ++turn) {
        if (!append("user.message", {{"text", QString("prompt-%1").arg(turn)}}) ||
            !append("turn.started", {}))
            return false;
        for (const QString& type :
             {QString("usage.context"), QString("usage.limits"), QString("usage.session")}) {
            for (int sample = 1; sample <= 3; ++sample)
                if (!append(type, {{"sample", sample}, {"turn", turn}}))
                    return false;
        }
        QJsonObject item{
            {"id", QString("item-%1").arg(turn)}, {"type", "agentMessage"}, {"text", "answer"}};
        if (!output.isEmpty()) {
            item = {{"id", QString("item-%1").arg(turn)},
                    {"type", "commandExecution"},
                    {"command", "synthetic-command"},
                    {"cwd", "synthetic"},
                    {"exitCode", 0},
                    {"durationMs", 123},
                    {"status", "completed"},
                    {"aggregatedOutput", output}};
        }
        if (largeTool) {
            const QJsonArray content{QJsonObject{{"type", "text"}, {"text", output}}};
            item = {{"id", QString("item-%1").arg(turn)},
                    {"type", "mcpToolCall"},
                    {"status", "completed"},
                    {"server", "synthetic"},
                    {"tool", "large-result"},
                    {"result", QJsonObject{{"content", content}}}};
        }
        if (toolImage) {
            // A screenshot journaled before MCP media moved to the media store.
            const QJsonArray content{
                QJsonObject{{"type", "text"}, {"text", "captured"}},
                QJsonObject{{"type", "image"}, {"mimeType", "image/png"}, {"data", output}}};
            item = {{"id", QString("item-%1").arg(turn)},
                    {"type", "mcpToolCall"},
                    {"status", "completed"},
                    {"server", "browser"},
                    {"tool", "screenshot"},
                    {"result", QJsonObject{{"content", content}}}};
        }
        QJsonObject started = item;
        if (toolImage)
            started = {{"id", QString("item-%1").arg(turn)},
                       {"type", "mcpToolCall"},
                       {"status", "inProgress"},
                       {"server", "browser"},
                       {"tool", "screenshot"}};
        if (generatedImage) {
            // Older Codex builds journaled the generated PNG inline as base64.
            started = {{"id", QString("item-%1").arg(turn)},
                       {"type", "imageGeneration"},
                       {"status", "inProgress"}};
            item = started;
            item.insert("status", "completed");
            item.insert("revisedPrompt", QString("kite-%1").arg(turn));
            item.insert("savedPath", QString("/synthetic/kite-%1.png").arg(turn));
            item.insert("result", output);
        }
        if (!append("item.started", {{"item", started}}) ||
            !append("item.completed", {{"item", item}}) || !append("turn.completed", {}))
            return false;
    }
    Database database(path);
    return database.exec("UPDATE replay_events SET sent_at='2020-01-01T00:00:00.000Z'") &&
           (!acknowledge || (journal.acknowledgeCursor("client", "session", 1, cursor) &&
                             drainAcknowledged(journal, database)));
}

qint64 sweep(LocalJournal& journal, int maximumRows = 128) {
    qint64 total = 0;
    // The fixture has at most 336 rows. A few complete sweeps also prove idempotency.
    for (int pass = 0; pass < 8; ++pass) {
        const auto removed = journal.pruneReplayDetails(cutoff(), maximumRows);
        if (!removed)
            return -1;
        total += *removed;
    }
    return total;
}

QJsonArray allReplay(LocalJournal& journal) {
    QJsonArray result;
    qint64 cursor = 0;
    for (;;) {
        const auto page = journal.compactedEventsAfter("session", 1, cursor, 500);
        if (page.events.isEmpty())
            return result;
        if (page.nextCursor <= cursor)
            qFatal("Replay cursor made no progress");
        for (const auto& event : page.events)
            result.append(event);
        cursor = page.nextCursor;
    }
}

QJsonArray withoutUsage(const QJsonArray& events) {
    QJsonArray result;
    for (const auto& event : events) {
        if (!event.toObject().value("event_type").toString().startsWith("usage."))
            result.append(event);
    }
    return result;
}
} // namespace

class JournalUsageRetentionTest final : public QObject {
    Q_OBJECT
  private slots:
    void largeDetailWaitsForQueuedRawCleanup() {
        QTemporaryDir dir;
        const auto path = dir.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, false, 21, QString(300000, 'x'), true));
        Database database(path);
        QVERIFY(journal.acknowledgeCursor("client", "session", 1,
                                          journal.sessions().first().lastCursor));
        QVERIFY(database.scalar("SELECT count(*) FROM events") > 0);
        QVERIFY(sweep(journal) >= 0);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "json_type(payload,'$.item.compacted')='object'"),
                 0);
        QVERIFY(drainAcknowledged(journal, database));
        QVERIFY(sweep(journal) > 0);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "json_type(payload,'$.item.compacted')='object'"),
                 1);
    }
    void largePayloadGcUsesIndexedReplayReferencesOnReopen() {
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        QString transfer;
        {
            LocalJournal journal(path);
            QVERIFY(seed(journal, path, true, 21, QString(300000, 'x'), true));
            for (const auto& value : allReplay(journal)) {
                const auto event = value.toObject();
                if (event.value("event_type") == "item.completed" &&
                    event.value("turn_sequence").toInteger() == 1)
                    transfer = event.value("payload")
                                   .toObject()
                                   .value("large_event")
                                   .toObject()
                                   .value("transfer_id")
                                   .toString();
            }
            QVERIFY(!transfer.isEmpty());
        }
        Database database(path);
        // Emulate a journal predating the index, with a same-item retained backlog.
        QVERIFY(database.exec("DROP INDEX IF EXISTS replay_events_item_transfer"));
        QVERIFY(database.exec("UPDATE replay_events SET cursor=cursor+400000"));
        QVERIFY(database.exec(
            "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<300000) "
            "INSERT INTO replay_events "
            "(session_id,generation,cursor,event_type,item_id,payload,sent_at,turn_sequence) "
            "SELECT 'session',1,x,'item.started','item-1', "
            "json_object('item',json_object('id','item-1','type','mcpToolCall',"
            "'status','inProgress','progress',printf('%1000s','x'))), "
            "'2020-01-01T00:00:00.000Z',21 FROM n"));
        QElapsedTimer timer;
        timer.start();
        LocalJournal journal(path);
        const auto reopenMs = timer.elapsed();
        QVERIFY2(journal.isOpen(), qPrintable(journal.errorString()));
        QCOMPARE(database.scalar(
                     "SELECT count(*) FROM sqlite_master WHERE name='replay_events_item_transfer'"),
                 1);
        const auto gcQuery = "SELECT 1 FROM large_event_payloads p WHERE "
                             "p.session_id = ? AND p.generation = ? AND p.transfer_id = ? AND " +
                             detail::largePayloadUnreferenced();
        const QVariantList bindings{QString("session"), 1, transfer};
        const auto plan = database.queryPlan(gcQuery, bindings);
        QVERIFY2(plan.contains("replay_events_item_transfer"), qPrintable(plan));
        QVERIFY2(plan.contains("<expr>=?"), qPrintable(plan));
        timer.restart();
        for (int iteration = 0; iteration < 25; ++iteration)
            QCOMPARE(database.rows(gcQuery, bindings), 0);
        qInfo() << "25 production GC lookups ms:" << timer.elapsed();
        timer.restart();
        const auto changed = journal.pruneReplayDetails(cutoff(), 16);
        const auto elapsed = timer.elapsed();
        QVERIFY(changed.has_value());
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "json_type(payload,'$.item.compacted')='object'"),
                 1);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE cursor<=300000"),
                 300000);
        qInfo() << "300000 retained same-item rows: reopen/index ms" << reopenMs
                << "16-row retention pass ms" << elapsed << plan;
        QVERIFY2(elapsed < 1000, "Retention stalled on same-item replay backlog");
    }
    void generatedImagesLoseTheirPixelsOnePerPass() {
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        // Above the shared 1 MiB lane, as a legacy inline PNG of about 1.1 MB would be.
        QVERIFY(seed(journal, path, true, 24, QString(1500 * 1024, 'A'), false, true));
        Database database(path);
        const auto payloads = database.scalar("SELECT count(*) FROM large_event_payloads");
        QCOMPARE(payloads, 24);
        const auto before = allReplay(journal);
        QVERIFY(journal.pruneReplayDetails(cutoff(), 256).has_value());
        QCOMPARE(database.scalar("SELECT count(*) FROM large_event_payloads"), payloads - 1);
        QVERIFY(sweep(journal) > 0);
        // Only the four turns older than the latest twenty lose their pixels.
        QCOMPARE(database.scalar("SELECT count(*) FROM large_event_payloads"), payloads - 4);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE json_extract("
                                 "payload,'$.item.compacted.policy')='image-pixels-v1'"),
                 4);
        const auto after = allReplay(journal);
        for (const auto& value : after) {
            const auto event = value.toObject();
            const auto turn = event.value("turn_sequence").toInteger();
            if (event.value("event_type") != "item.completed")
                continue;
            const auto item = event.value("payload").toObject().value("item").toObject();
            if (turn > 4) {
                QVERIFY(before.contains(value));
                continue;
            }
            QVERIFY(!item.contains("result"));
            QCOMPARE(item.value("revisedPrompt").toString(), QString("kite-%1").arg(turn));
            QCOMPARE(item.value("savedPath").toString(),
                     QString("/synthetic/kite-%1.png").arg(turn));
            QCOMPARE(item.value("status").toString(), QString("completed"));
        }
        // Rows only change; none go, apart from the intermediate usage samples.
        QCOMPARE(withoutUsage(after).size(), withoutUsage(before).size());
    }

    void generatedImagesKeepAPreview() {
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        CodexAdapter adapter("/nonexistent/codex");
        SessionHost host(&journal, &adapter);
        QImage picture(1024, 768, QImage::Format_RGB32);
        picture.fill(QColor(200, 40, 40));
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly) && picture.save(&buffer, "PNG"));
        QVERIFY(seed(journal, path, true, 24, QString::fromLatin1(png.toBase64()), false, true));
        Database database(path);
        // These PNGs are small enough to be inline, yet each preview decodes the full picture:
        // a pass decodes one and leaves the rest for the next passes.
        QVERIFY(journal.pruneReplayDetails(cutoff(), 256).has_value());
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE json_extract("
                                 "payload,'$.item.compacted.policy')='image-preview-v1'"),
                 1);
        QVERIFY(sweep(journal) > 0);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE json_extract("
                                 "payload,'$.item.compacted.policy')='image-preview-v1'"),
                 4);
        const QDir media(McpMediaStore::directoryForJournal(path));
        for (const auto& value : allReplay(journal)) {
            const auto event = value.toObject();
            const auto item = event.value("payload").toObject().value("item").toObject();
            if (event.value("event_type") != "item.completed" ||
                event.value("turn_sequence").toInteger() > 4)
                continue;
            const auto block =
                item.value("result").toObject().value("content").toArray().first().toObject();
            QCOMPARE(block.value("type").toString(), QString("image"));
            QVERIFY(qMax(block.value("width").toInt(), block.value("height").toInt()) <= 512);
            QCOMPARE(block.value("width").toInt(), 512);
            const auto reference = block.value("media_ref").toString();
            QVERIFY(reference.startsWith("sha256:"));
            QCOMPARE(media.entryList({reference.mid(7) + ".*"}, QDir::Files).size(), 1);
            QCOMPARE(
                item.value("savedPath").toString(),
                QString("/synthetic/kite-%1.png").arg(event.value("turn_sequence").toInteger()));
        }
    }

    void toolResultImagesKeepAPreview() {
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        CodexAdapter adapter("/nonexistent/codex");
        SessionHost host(&journal, &adapter);
        QImage picture(900, 1200, QImage::Format_ARGB32);
        picture.fill(QColor(20, 120, 200, 128));
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly) && picture.save(&buffer, "PNG"));
        QVERIFY(
            seed(journal, path, true, 24, QString::fromLatin1(png.toBase64()), false, false, true));
        Database database(path);
        const QString previewed = "SELECT count(*) FROM replay_events WHERE json_extract("
                                  "payload,'$.item.compacted.inline_images.previews')=1";
        // One decoding item per pass, as for generated images.
        QVERIFY(journal.pruneReplayDetails(cutoff(), 256).has_value());
        QCOMPARE(database.scalar(previewed), 1);
        QVERIFY(sweep(journal) > 0);
        QCOMPARE(database.scalar(previewed), 4);
        for (const auto& value : allReplay(journal)) {
            const auto event = value.toObject();
            if (event.value("event_type") != "item.completed" ||
                event.value("turn_sequence").toInteger() > 4)
                continue;
            const auto content = event.value("payload")
                                     .toObject()
                                     .value("item")
                                     .toObject()
                                     .value("result")
                                     .toObject()
                                     .value("content")
                                     .toArray();
            QCOMPARE(content.size(), 2);
            QCOMPARE(content.at(0).toObject().value("text").toString(), QString("captured"));
            const auto block = content.at(1).toObject();
            QVERIFY(block.value("media_ref").toString().startsWith("sha256:"));
            QCOMPARE(block.value("height").toInt(), 512);
            QVERIFY(!block.contains("data"));
        }
    }

    void toolResultImagesLeaveTheJournal_data() {
        QTest::addColumn<int>("imageBytes");
        QTest::addColumn<bool>("textTrimmed");
        QTest::newRow("inline") << 100 * 1024 << false;             // an ordinary replay row
        QTest::newRow("inline-text-trimmed") << 100 * 1024 << true; // by a pass before ADR 0099
        QTest::newRow("large") << 1500 * 1024 << false; // chunked, above the shared lane
    }
    void toolResultImagesLeaveTheJournal() {
        QFETCH(int, imageBytes);
        QFETCH(bool, textTrimmed);
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, true, 24, QString(imageBytes, 'A'), false, false, true));
        Database database(path);
        if (textTrimmed)
            QVERIFY(database.exec(
                "UPDATE replay_events SET payload=json_set(payload,'$.item.compacted',"
                "json('{\"policy\":\"tool-result-text-v1\"}')) WHERE event_type='item.completed'"));
        const auto payloads = database.scalar("SELECT count(*) FROM large_event_payloads");
        const auto before = allReplay(journal);
        QVERIFY(sweep(journal) > 0);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE json_type("
                                 "payload,'$.item.compacted.inline_images')='object'"),
                 4);
        if (payloads > 0)
            QCOMPARE(database.scalar("SELECT count(*) FROM large_event_payloads"), payloads - 4);
        const auto after = allReplay(journal);
        for (const auto& value : after) {
            const auto event = value.toObject();
            if (event.value("event_type") != "item.completed")
                continue;
            if (event.value("turn_sequence").toInteger() > 4) {
                QVERIFY(before.contains(value));
                continue;
            }
            const auto content = event.value("payload")
                                     .toObject()
                                     .value("item")
                                     .toObject()
                                     .value("result")
                                     .toObject()
                                     .value("content")
                                     .toArray();
            QCOMPARE(content.size(), 2);
            QCOMPARE(content.at(0).toObject().value("text").toString(), QString("captured"));
            QVERIFY(content.at(1).toObject().value("omitted").toBool());
            QVERIFY(!content.at(1).toObject().contains("data"));
        }
        // Completions stay; only usage samples and superseded starts (ADR 0041) may go.
        const auto completions = [](const QJsonArray& events) {
            return std::count_if(events.begin(), events.end(), [](const QJsonValue& event) {
                return event.toObject().value("event_type") == "item.completed";
            });
        };
        QCOMPARE(completions(after), completions(before));
    }

    // Probing text-only tool payloads for images counts against the pass, found or not.
    void textOnlyToolBacklogKeepsPassesShort() {
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, true, 21, QString(8 * 1024 * 1024, 'x'), true));
        Database database(path);
        // 512 more manifests of the oldest 8 MB text result, all eligible.
        QVERIFY(database.exec(
            "WITH RECURSIVE n(v) AS (VALUES(1) UNION ALL SELECT v+1 FROM n WHERE v<512) "
            "INSERT INTO replay_events(session_id,generation,cursor,event_type,item_id,payload,"
            "sent_at,turn_sequence) SELECT r.session_id,r.generation,100000+n.v,r.event_type,"
            "r.item_id,r.payload,r.sent_at,r.turn_sequence FROM n CROSS JOIN replay_events r "
            "WHERE r.event_type='item.completed' AND r.turn_sequence=1"));
        const auto payloads = database.scalar("SELECT count(*) FROM large_event_payloads");
        qint64 slowest = 0;
        for (int pass = 0; pass < 8; ++pass) {
            QElapsedTimer timer;
            timer.start();
            QVERIFY(journal.pruneReplayDetails(cutoff()).has_value());
            slowest = qMax(slowest, timer.elapsed());
        }
        qInfo() << "Slowest text-only probe pass ms:" << slowest;
        QCOMPARE(database.scalar("SELECT count(*) FROM large_event_payloads"), payloads);
        // Unbudgeted, one default pass probed 128 × 8 MB and took about 0.9-1.9 s.
        QVERIFY2(slowest < 500, qPrintable(QString("slowest pass %1 ms").arg(slowest)));
    }

    void generatedImagesWaitForAcknowledgement() {
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, false, 24, QString(1500 * 1024, 'A'), false, true));
        Database database(path);
        QVERIFY(sweep(journal) >= 0);
        QCOMPARE(database.scalar("SELECT count(*) FROM large_event_payloads"), 24);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE json_extract("
                                 "payload,'$.item.compacted.policy')='image-pixels-v1'"),
                 0);
    }

    void boundedLargePayloadsCompactAtomically_data() {
        QTest::addColumn<bool>("revisionFailure");
        QTest::newRow("blob-delete") << false;
        QTest::newRow("projection-revision") << true;
    }
    void boundedLargePayloadsCompactAtomically() {
        QFETCH(bool, revisionFailure);
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, true, 24, QString(300000, 'x'), true));
        Database database(path);
        const auto before = allReplay(journal);
        const auto window = journal.transcriptWindow("session", 1);
        const auto payloads = database.scalar("SELECT count(*) FROM large_event_payloads");
        const auto chunks = database.scalar("SELECT count(*) FROM large_event_payload_chunks");
        QVERIFY(payloads >= 24);
        QString transfer;
        for (const auto& value : before) {
            const auto event = value.toObject();
            if (event.value("event_type") == "item.completed" &&
                event.value("turn_sequence").toInteger() == 1)
                transfer = event.value("payload")
                               .toObject()
                               .value("large_event")
                               .toObject()
                               .value("transfer_id")
                               .toString();
        }
        QVERIFY(!transfer.isEmpty());
        QVERIFY(journal.largeEventFragment("session", 1, transfer, 0).errorCode.isEmpty());
        QVERIFY(database.exec(
            revisionFailure
                ? "CREATE TRIGGER fail_large_delete BEFORE UPDATE OF revision ON "
                  "replay_projection_state BEGIN SELECT RAISE(ABORT,'rollback'); END"
                : "CREATE TRIGGER fail_large_delete BEFORE DELETE ON large_event_payloads WHEN "
                  "OLD.event_type='item.completed' BEGIN SELECT RAISE(ABORT,'rollback'); END"));
        QVERIFY(!journal.pruneReplayDetails(cutoff()));
        QCOMPARE(allReplay(journal), before);
        QCOMPARE(database.scalar("SELECT count(*) FROM large_event_payloads"), payloads);
        QCOMPARE(database.scalar("SELECT count(*) FROM large_event_payload_chunks"), chunks);
        QVERIFY(database.exec("DROP TRIGGER fail_large_delete"));
        QElapsedTimer timer;
        timer.start();
        const auto firstPass = journal.pruneReplayDetails(cutoff(), 256);
        QVERIFY(firstPass.has_value());
        const auto firstCount =
            payloads - database.scalar("SELECT count(*) FROM large_event_payloads");
        QVERIFY(firstCount > 0 && firstCount <= 3);
        QVERIFY(sweep(journal) > 0);
        qInfo() << "Bounded large-payload sweep ms:" << timer.elapsed();
        QCOMPARE(database.scalar("SELECT count(*) FROM large_event_payloads"), payloads - 4);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "json_type(payload,'$.item.compacted')='object'"),
                 4);
        QCOMPARE(journal.largeEventFragment("session", 1, transfer, 0).errorCode,
                 QString("large_event_not_found"));
        QCOMPARE(journal
                     .transcriptWindow("session", 1, window.beforeCursor, window.snapshotCursor,
                                       window.projectionRevision)
                     .errorCode,
                 QString("snapshot_changed"));
        const auto after = allReplay(journal);
        for (const auto& value : before) {
            const auto event = value.toObject();
            if (event.value("turn_sequence").toInteger() > 4)
                QVERIFY(after.contains(value)); // latest twenty, including manifests, are untouched
        }
        QCOMPARE(sweep(journal), 0);
    }
    void largePayloadGuards_data() {
        QTest::addColumn<QString>("guard");
        for (const auto& guard :
             {"unacked", "running", "recent", "other-turn-raw", "oversize", "digest", "revision",
              "missing-chunk", "corrupt-bytes", "oversize-chunk", "image", "question", "open-turn",
              "old-generation", "unknown-manifest"})
            QTest::newRow(guard) << QString(guard);
    }
    void largePayloadGuards() {
        QFETCH(QString, guard);
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, guard != "unacked", 21,
                     QString(guard == "oversize" ? 1100000 : 300000, 'x'), true));
        Database database(path);
        if (guard == "running")
            QVERIFY(database.exec("UPDATE sessions SET state='running'"));
        if (guard == "recent")
            QVERIFY(database.exec("UPDATE replay_events SET sent_at=datetime('now')"));
        if (guard == "other-turn-raw")
            QVERIFY(database.exec(
                "INSERT INTO "
                "events(session_id,generation,cursor,event_type,payload,sent_at,turn_sequence) "
                "SELECT session_id,generation,cursor,event_type,payload,sent_at,turn_sequence FROM "
                "replay_events WHERE turn_sequence=21 LIMIT 1"));
        if (guard == "digest")
            QVERIFY(database.exec("UPDATE large_event_payloads SET digest_sha256='bad'"));
        if (guard == "revision")
            QVERIFY(database.exec("UPDATE large_event_payloads SET revision=revision+1"));
        if (guard == "missing-chunk")
            QVERIFY(database.exec("DELETE FROM large_event_payload_chunks WHERE chunk_offset=0"));
        if (guard == "corrupt-bytes")
            QVERIFY(database.exec("UPDATE large_event_payload_chunks SET "
                                  "payload=CAST(replace(CAST(payload AS TEXT),'xxx','yyy') AS "
                                  "BLOB) WHERE chunk_offset=0"));
        if (guard == "oversize-chunk")
            QVERIFY(database.exec("UPDATE large_event_payload_chunks SET payload=zeroblob(131073) "
                                  "WHERE chunk_offset=0"));
        if (guard == "image")
            QVERIFY(database.exec("UPDATE replay_events SET "
                                  "payload=json_set(payload,'$.item_type','imageGeneration') WHERE "
                                  "event_type='item.completed'"));
        if (guard == "question")
            QVERIFY(database.exec("UPDATE replay_events SET event_type='question.opened' WHERE "
                                  "event_type='usage.context'"));
        if (guard == "open-turn")
            QVERIFY(database.exec("UPDATE replay_events SET event_type='turn.started' WHERE "
                                  "event_type='turn.completed'"));
        if (guard == "old-generation")
            QVERIFY(database.exec("UPDATE sessions SET generation=2"));
        if (guard == "unknown-manifest")
            QVERIFY(database.exec(
                "UPDATE replay_events SET payload=json_set(payload,'$.future_field','keep') WHERE "
                "event_type='item.completed'"));
        const auto payloads = database.scalar("SELECT count(*) FROM large_event_payloads");
        const auto chunks = database.scalar("SELECT count(*) FROM large_event_payload_chunks");
        QVERIFY(sweep(journal) >= 0);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "json_type(payload,'$.item.compacted')='object'"),
                 0);
        QCOMPARE(database.scalar("SELECT count(*) FROM large_event_payloads"), payloads);
        QCOMPARE(database.scalar("SELECT count(*) FROM large_event_payload_chunks"), chunks);
    }
    void sharedLargePayloadRemainsReadable() {
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, true, 21, QString(300000, 'x'), true));
        QString transfer;
        for (const auto& value : allReplay(journal)) {
            const auto event = value.toObject();
            if (event.value("event_type") == "item.completed" &&
                event.value("turn_sequence").toInteger() == 1)
                transfer = event.value("payload")
                               .toObject()
                               .value("large_event")
                               .toObject()
                               .value("transfer_id")
                               .toString();
        }
        QVERIFY(!transfer.isEmpty());
        Database database(path);
        QVERIFY(
            database.exec("UPDATE replay_events SET item_id='item-1', payload=(SELECT payload FROM "
                          "replay_events WHERE event_type='item.completed' AND turn_sequence=1) "
                          "WHERE event_type='item.completed' AND turn_sequence=21"));
        QVERIFY(sweep(journal) > 0);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "json_type(payload,'$.item.compacted')='object'"),
                 1);
        QVERIFY(journal.largeEventFragment("session", 1, transfer, 0).errorCode.isEmpty());
    }
    void retainsLastSamplesMessagesOrderingAndFencesContinuations() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        const auto before = journal.compactedEventsAfter("session", 1, 0, 500);
        const auto window = journal.transcriptWindow("session", 1);
        QVERIFY(window.hasMoreBefore);
        QCOMPARE(sweep(journal), 24); // 4 old turns * 3 types * 2 intermediate samples
        const auto after = journal.compactedEventsAfter("session", 1, 0, 500);
        QCOMPARE(withoutUsage(after.events), withoutUsage(before.events));
        QCOMPARE(after.nextCursor, before.nextCursor);
        auto page = journal.transcriptPage("session", 1, 0, 20);
        const auto older = journal.transcriptPage("session", 1, page.earliestTurnSequence, 20);
        auto paged = older.events;
        for (const auto& event : page.events)
            paged.append(event);
        QCOMPARE(paged, after.events);
        Database database(path);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE turn_sequence<=4 AND "
                                 "event_type LIKE 'usage.%'"),
                 12);
        QCOMPARE(
            database.scalar("SELECT count(*) FROM replay_events WHERE turn_sequence<=4 AND "
                            "event_type LIKE 'usage.%' AND json_extract(payload,'$.sample')!=3"),
            0);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE turn_sequence>4 AND "
                                 "event_type LIKE 'usage.%'"),
                 180);
        QCOMPARE(journal
                     .transcriptWindow("session", 1, window.beforeCursor, window.snapshotCursor,
                                       window.projectionRevision)
                     .errorCode,
                 QString("snapshot_changed"));
        const auto revision = journal.transcriptWindow("session", 1).projectionRevision;
        QCOMPARE(sweep(journal), 0);
        QCOMPARE(journal.transcriptWindow("session", 1).projectionRevision, revision);
    }

    void protectsTurns_data() {
        QTest::addColumn<QString>("guard");
        QTest::addColumn<bool>("commands");
        for (const char* guard :
             {"running", "waiting_for_input", "starting", "stopping", "unacknowledged", "recent",
              "open", "approval", "question", "generation", "sequence-gap"})
            for (bool commands : {false, true})
                QTest::newRow(qPrintable(QString(guard) + (commands ? "-commands" : "-usage")))
                    << QString(guard) << commands;
    }

    void protectsTurns() {
        QFETCH(QString, guard);
        QFETCH(bool, commands);
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, guard != "unacknowledged", 24,
                     commands ? QString(50000, QLatin1Char('x')) : QString()));
        Database database(path);
        if (guard == "unacknowledged") {
            QVERIFY(journal.acknowledgeCursor("slow", "session", 1, 0));
            const auto raw = journal.eventsAfter("session", 1, 0, 500);
            QVERIFY(journal.acknowledgeCursor("fast", "session", 1,
                                              raw.last().toObject().value("cursor").toInteger()));
        }
        if (guard == "running" || guard == "waiting_for_input" || guard == "starting" ||
            guard == "stopping")
            QVERIFY(database.exec(QString("UPDATE sessions SET state='%1'").arg(guard)));
        else if (guard == "recent")
            QVERIFY(database.exec(
                "UPDATE replay_events SET sent_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE "
                "event_type='turn.completed'"));
        else if (guard == "open")
            QVERIFY(database.exec("DELETE FROM replay_events WHERE event_type='turn.completed'"));
        else if (guard == "approval" || guard == "question")
            QVERIFY(database.exec(QString("UPDATE replay_events SET event_type='%1.requested' "
                                          "WHERE event_type='item.started'")
                                      .arg(guard)));
        else if (guard == "generation")
            QVERIFY(database.exec("UPDATE sessions SET generation=2"));
        else if (guard == "sequence-gap") {
            // Only 19 actual newer turns, despite a large numeric sequence gap.
            QVERIFY(database.exec("DELETE FROM replay_events WHERE turn_sequence BETWEEN 2 AND 5"));
            QVERIFY(database.exec("UPDATE replay_events SET turn_sequence=turn_sequence*100"));
            QVERIFY(database.exec("UPDATE sessions SET current_turn_sequence=2400"));
        }
        const auto before = journal.compactedEventsAfter("session", 1, 0, 500).events;
        QCOMPARE(sweep(journal), 0);
        QCOMPARE(journal.compactedEventsAfter("session", 1, 0, 500).events, before);
        if (guard == "unacknowledged") {
            const auto raw = journal.eventsAfter("session", 1, 0, 500);
            QCOMPARE(raw.size(), before.size());
            QVERIFY(journal.acknowledgeCursor("slow", "session", 1,
                                              raw.last().toObject().value("cursor").toInteger()));
            QCOMPARE(sweep(journal), commands ? 32 : 24); // ineligible rows are revisited
        }
    }

    void boundsTheScanAndMakesProgressPastUninterestingRows() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        // The first two rows are user.message and turn.started: no deletion yet.
        QCOMPARE(journal.pruneReplayDetails(cutoff(), 1), std::optional<qint64>(0));
        QCOMPARE(journal.pruneReplayDetails(cutoff(), 1), std::optional<qint64>(0));
        QCOMPARE(journal.pruneReplayDetails(cutoff(), 1), std::optional<qint64>(1));
        qint64 removed = 1;
        for (int pass = 0; pass < 700; ++pass) {
            const auto result = journal.pruneReplayDetails(cutoff(), 1);
            QVERIFY(result.has_value());
            QVERIFY(*result <= 1);
            removed += *result;
        }
        QCOMPARE(removed, 24);
    }

    void rollsBackDeletesRevisionAndScanProgressTogether() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        Database database(path);
        const auto before = journal.compactedEventsAfter("session", 1, 0, 500).events;
        const auto revision = journal.transcriptWindow("session", 1).projectionRevision;
        QVERIFY(database.exec(
            "CREATE TRIGGER fail_retention BEFORE UPDATE OF revision ON replay_projection_state "
            "BEGIN SELECT RAISE(ABORT, 'synthetic revision failure'); END"));
        QVERIFY(!journal.pruneReplayDetails(cutoff()).has_value());
        QVERIFY(journal.errorString().contains("synthetic revision failure"));
        QCOMPARE(journal.compactedEventsAfter("session", 1, 0, 500).events, before);
        QCOMPARE(journal.transcriptWindow("session", 1).projectionRevision, revision);
        QVERIFY(database.exec("DROP TRIGGER fail_retention"));
        QCOMPARE(journal.pruneReplayDetails(cutoff()), std::optional<qint64>(24));
        QCOMPARE(sweep(journal), 0);
    }

    void yieldsToAnotherWriterAndKeepsWorkingAfterward() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        QSemaphore locked;
        QSemaphore release;
        bool writerReady = false;
        std::thread writer([&] {
            Database database(path);
            writerReady = database.exec("BEGIN IMMEDIATE");
            locked.release();
            release.acquire();
            QThread::msleep(200);
            database.exec("ROLLBACK");
        });
        locked.acquire();
        QElapsedTimer timer;
        timer.start();
        const auto maintenance = journal.pruneReplayDetails(cutoff());
        const auto elapsed = timer.elapsed();
        release.release();
        // The normal connection timeout must be restored: this ordinary write waits for
        // the writer's delayed release and succeeds, instead of failing with SQLITE_BUSY.
        const bool written = journal.markSessionSeen("session", 1, 1);
        writer.join();
        QVERIFY(writerReady);
        QVERIFY(!maintenance.has_value());
        QVERIFY(elapsed < 1000);
        QVERIFY(written);
        QCOMPARE(sweep(journal), 24);
    }

    void rawBacklogUsesTurnLookup_data() {
        QTest::addColumn<bool>("upgrade");
        QTest::newRow("fresh") << false;
        QTest::newRow("existing-journal") << true;
    }

    void rawBacklogUsesTurnLookup() {
        QFETCH(bool, upgrade);
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        {
            LocalJournal journal(path);
            QVERIFY(seed(journal, path));
        }
        Database database(path);
        // A later unacknowledged turn must not make each older-turn ACK probe scan
        // the entire raw backlog. This is synthetic storage, never a provider call.
        QVERIFY(database.exec(
            "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<300000) "
            "INSERT INTO "
            "events(session_id,generation,cursor,event_type,payload,sent_at,turn_sequence) "
            "SELECT 'session',1,1000+x,'usage.context','{}','2020-01-01T00:00:00.000Z',24 FROM n"));
        if (upgrade)
            QVERIFY(database.exec("DROP INDEX IF EXISTS events_turn"));
        LocalJournal journal(path); // also installs the index on pre-existing journals
        Database migrated(path);    // fresh reader sees the migrated schema, not a cached plan
        const auto plan = migrated.queryPlan(
            "SELECT 1 FROM events WHERE session_id='session' AND generation=1 AND turn_sequence=1");
        QElapsedTimer timer;
        timer.start();
        const auto removed = journal.pruneReplayDetails(cutoff(), 128);
        const auto elapsed = timer.elapsed();
        qInfo().noquote() << "300000 raw events, 128 replay rows:" << elapsed << "ms;" << plan;
        QCOMPARE(removed, std::optional<qint64>(24));
        QCOMPARE(database.scalar("SELECT count(*) FROM events"), 300000);
        QVERIFY2(plan.contains("COVERING INDEX events_turn") &&
                     plan.contains("session_id=? AND generation=? AND turn_sequence=?"),
                 qPrintable(plan));
        // Wide headroom for slow runners; query-plan assertion is the deterministic guard.
        QVERIFY2(elapsed < 1000, "Idle retention stalled on unrelated raw backlog");
    }

    void commandOutputRetainsMetadataUtf8AndExplicitNotice() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        const QString output = QStringLiteral("HEAD\n") + QString(20000, QChar(0x754c)) + "\n" +
                               QStringLiteral("tail-line\n").repeated(100) + "TAIL";
        QVERIFY(seed(journal, path, true, 24, output));
        const auto before = allReplay(journal);
        const auto window = journal.transcriptWindow("session", 1);
        QCOMPARE(sweep(journal), 32); // usage, four starts and four output previews
        const auto after = allReplay(journal);
        QJsonArray expected;
        for (const auto& value : withoutUsage(before)) {
            const auto event = value.toObject();
            if (event.value("event_type") == "item.started" &&
                event.value("turn_sequence").toInteger() <= 4)
                continue;
            expected.append(value);
        }
        const auto actual = withoutUsage(after);
        QCOMPARE(actual.size(), expected.size());
        int trimmed = 0;
        for (qsizetype i = 0; i < actual.size(); ++i) {
            auto event = actual.at(i).toObject();
            auto payload = event.value("payload").toObject();
            // Replay envelopes store provider data under payload.
            auto item = payload.value("item").toObject();
            if (!item.contains("compacted")) {
                QCOMPARE(event, expected.at(i).toObject());
                continue;
            }
            ++trimmed;
            const auto marker = item.value("compacted").toObject();
            QCOMPARE(marker.value("original_bytes").toInteger(), output.toUtf8().size());
            QVERIFY(marker.value("retained_bytes").toInteger() <= 20 * 1024);
            const QString retained = item.value("aggregatedOutput").toString();
            QVERIFY(retained.startsWith("[Output trimmed"));
            QVERIFY(retained.contains("HEAD\n"));
            QVERIFY(retained.endsWith("\nTAIL"));
            QVERIFY(!retained.contains(QChar::ReplacementCharacter));
            item.remove("compacted");
            item.insert("aggregatedOutput", output);
            payload.insert("item", item);
            event.insert("payload", payload);
            QCOMPARE(event, expected.at(i).toObject());
        }
        QCOMPARE(trimmed, 4);
        QCOMPARE(after.last().toObject().value("cursor"), before.last().toObject().value("cursor"));
        QCOMPARE(journal
                     .transcriptWindow("session", 1, window.beforeCursor, window.snapshotCursor,
                                       window.projectionRevision)
                     .errorCode,
                 QString("snapshot_changed"));
        QCOMPARE(sweep(journal), 0);
        const auto receipt = journal.commandOutput("session", 1, 1, "item-1");
        QVERIFY(receipt.value("ok").toBool());
        QVERIFY(receipt.value("truncated").toBool());
        QVERIFY(receipt.value("text").toString().startsWith("[Output trimmed"));
        QVERIFY(receipt.value("text").toString().split('\n').size() <= 40);
        QCOMPARE(receipt.value("compacted").toObject().value("original_bytes").toInteger(),
                 output.toUtf8().size());
    }

    void commandBudgetResumesAndRollbackRestoresPayloads() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, true, 30, QString(230000, QLatin1Char('x'))));
        Database database(path);
        const auto before = allReplay(journal);
        QVERIFY(database.exec(
            "CREATE TRIGGER fail_detail BEFORE UPDATE OF revision ON replay_projection_state "
            "BEGIN SELECT RAISE(ABORT, 'synthetic detail failure'); END"));
        QVERIFY(!journal.pruneReplayDetails(cutoff()).has_value());
        QCOMPARE(allReplay(journal), before);
        QVERIFY(database.exec("DROP TRIGGER fail_detail"));
        QElapsedTimer passTimer;
        passTimer.start();
        QVERIFY(journal.pruneReplayDetails(cutoff()).has_value());
        qInfo() << "Warm command retention pass, 230000-byte outputs:" << passTimer.elapsed()
                << "ms";
        const auto first = database.scalar("SELECT count(*) FROM replay_events WHERE "
                                           "json_type(payload,'$.item.compacted')='object'");
        QCOMPARE(first, 1); // one start/completion pair plus its completion consumes 690 KiB
        QVERIFY(sweep(journal) > 0);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "json_type(payload,'$.item.compacted')='object'"),
                 9);
        // Pair parsing shares the budget, so ten large turns need more than eight passes.
        QVERIFY(sweep(journal) > 0);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "json_type(payload,'$.item.compacted')='object'"),
                 10);
        QCOMPARE(sweep(journal), 0);
    }

    void preservesUnsupportedCommandPayloads_data() {
        QTest::addColumn<QString>("replacement");
        QTest::newRow("small") << QString("json_set(payload,'$.item.aggregatedOutput','small')");
        QTest::newRow("near-limit")
            << QString("json_set(payload,'$.item.aggregatedOutput',printf('%.*c',21504,'x'))");
        QTest::newRow("nonterminal") << QString("json_set(payload,'$.item.status','inProgress')");
        QTest::newRow("marked") << QString("json_set(payload,'$.item.compacted',json('{}'))");
        QTest::newRow("oversize") << QString(
            "json_set(payload,'$.item.aggregatedOutput',printf('%.*c',1100000,'x'))");
    }

    void preservesUnsupportedCommandPayloads() {
        QFETCH(QString, replacement);
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, true, 24, QString(50000, QLatin1Char('x'))));
        Database database(path);
        QVERIFY(database.exec("UPDATE replay_events SET payload=" + replacement +
                              " WHERE event_type='item.completed'"));
        const auto before = withoutUsage(allReplay(journal));
        QCOMPARE(sweep(journal), 24); // only intermediate usage is changed
        QCOMPARE(withoutUsage(allReplay(journal)), before);
    }

    void structuredRetentionUsesTurnGuards_data() {
        QTest::addColumn<QString>("previous");
        QTest::addColumn<QString>("guard");
        for (const QString previous :
             {QString(), QString("tool-result-text-v1"), QString("tool-arguments-v1")})
            for (const QString guard : {QString("eligible"), QString("unacknowledged"),
                                        QString("running"), QString("recent"), QString("question"),
                                        QString("approval"), QString("generation")})
                QTest::newRow(qPrintable(guard + ("-" + previous))) << previous << guard;
    }
    void structuredRetentionUsesTurnGuards() {
        QFETCH(QString, previous);
        QFETCH(QString, guard);
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, guard != "unacknowledged"));
        Database database(path);
        QJsonObject fields{{"type", "mcpToolCall"},
                           {"status", "completed"},
                           {"arguments", QJsonObject{{"query", "keep"}}},
                           {"result", QJsonObject{{"structuredContent",
                                                   QJsonObject{{"rows", QString(16000, 'x')}}}}}};
        if (!previous.isEmpty())
            fields.insert("compacted", QJsonObject{{"policy", previous}});
        QVERIFY(database.patchItems(fields));
        if (guard == "running")
            QVERIFY(database.exec("UPDATE sessions SET state='running'"));
        if (guard == "recent")
            QVERIFY(database.exec(
                "UPDATE replay_events SET sent_at=strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
        if (guard == "question" || guard == "approval")
            QVERIFY(database.exec(QString("UPDATE replay_events SET event_type='%1.requested' "
                                          "WHERE event_type='item.started'")
                                      .arg(guard)));
        if (guard == "generation")
            QVERIFY(database.exec("UPDATE sessions SET generation=2"));
        const auto before = allReplay(journal);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(sweep(journal), guard == "eligible" ? 28 : 0);
        if (guard == "eligible")
            qInfo() << "Structured retention eight passes; previous=" << previous << timer.elapsed()
                    << "ms";
        if (guard != "eligible") {
            QCOMPARE(allReplay(journal), before);
            return;
        }
        QCOMPARE(
            database.scalar("SELECT count(*) FROM replay_events WHERE "
                            "json_type(payload,'$.item.result.structuredContent_excerpt')='text'"),
            4);
        QCOMPARE(
            database.scalar("SELECT count(*) FROM replay_events WHERE turn_sequence>4 AND "
                            "json_type(payload,'$.item.result.structuredContent_excerpt')='text'"),
            0);
        QCOMPARE(sweep(journal), 0);
    }
    void argumentRetentionUpgradesTextMarkersWithTurnGuards_data() {
        QTest::addColumn<bool>("previous");
        QTest::addColumn<QString>("guard");
        for (bool previous : {false, true})
            for (const QString guard : {QString("eligible"), QString("unacknowledged"),
                                        QString("running"), QString("recent"), QString("question"),
                                        QString("approval"), QString("generation")})
                QTest::newRow(qPrintable(guard + (previous ? "-previous" : "-fresh")))
                    << previous << guard;
    }
    void argumentRetentionUpgradesTextMarkersWithTurnGuards() {
        QFETCH(bool, previous);
        QFETCH(QString, guard);
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, guard != "unacknowledged"));
        Database database(path);
        QJsonObject fields{{"type", "mcpToolCall"},
                           {"status", "completed"},
                           {"arguments", QJsonObject{{"query", QString(16000, 'x')}}},
                           {"result", "keep result"}};
        if (previous)
            fields.insert("compacted", QJsonObject{{"policy", "tool-result-text-v1"}});
        QVERIFY(database.patchItems(fields));
        if (guard == "running")
            QVERIFY(database.exec("UPDATE sessions SET state='running'"));
        if (guard == "recent")
            QVERIFY(database.exec(
                "UPDATE replay_events SET sent_at=strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
        if (guard == "question" || guard == "approval")
            QVERIFY(database.exec(QString("UPDATE replay_events SET event_type='%1.requested' "
                                          "WHERE event_type='item.started'")
                                      .arg(guard)));
        if (guard == "generation")
            QVERIFY(database.exec("UPDATE sessions SET generation=2"));
        const auto before = allReplay(journal);
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(sweep(journal), guard == "eligible" ? 28 : 0);
        if (guard == "eligible")
            qInfo() << "Argument retention eight passes; previous=" << previous << timer.elapsed()
                    << "ms";
        if (guard != "eligible") {
            QCOMPARE(allReplay(journal), before);
            return;
        }
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "json_type(payload,'$.item.arguments_excerpt')='text'"),
                 4);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE turn_sequence>4 AND "
                                 "json_type(payload,'$.item.arguments_excerpt')='text'"),
                 0);
        QCOMPARE(sweep(journal), 0);
    }
    void toolAndReasoningRetentionUsesTurnGuards_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("guard");
        for (const QString& kind : {QString("mcpToolCall"), QString("dynamicToolCall"),
                                    QString("reasoning"), QString("fileChange")})
            for (const QString& guard : {QString("eligible"), QString("unacknowledged"),
                                         QString("running"), QString("recent")})
                QTest::newRow(qPrintable(kind + "-" + guard)) << kind << guard;
    }
    void toolAndReasoningRetentionUsesTurnGuards() {
        QFETCH(QString, kind);
        QFETCH(QString, guard);
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, guard != "unacknowledged"));
        Database database(path);
        QJsonObject fields{{"type", kind}};
        if (kind == "fileChange") {
            fields.insert("status", "completed");
            fields.insert("changes", QJsonArray{QJsonObject{{"path", "synthetic.cpp"},
                                                            {"kind", QJsonObject{{"type", "add"}}},
                                                            {"diff", QString(80000, 'x')}}});
        } else if (kind == "reasoning") {
            fields.insert("summary", QJsonArray{"summary stays"});
            fields.insert("content", QJsonArray{QString(10000, 'x')});
        } else {
            fields.insert("status", "completed");
            fields.insert("arguments", QJsonObject{{"keep", "arguments"}});
            fields.insert(
                "result",
                QJsonObject{
                    {"isError", false},
                    {"content",
                     QJsonArray{
                         QJsonObject{{"type", "text"}, {"text", QString(10000, 'x')}},
                         QJsonObject{{"type", "image"}, {"media_ref", "sha256:synthetic"}}}}});
        }
        QVERIFY(database.patchItems(fields));
        if (guard == "running")
            QVERIFY(database.exec("UPDATE sessions SET state='running'"));
        if (guard == "recent")
            QVERIFY(database.exec(
                "UPDATE replay_events SET sent_at=strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
        const auto before = allReplay(journal);
        QElapsedTimer sweepTimer;
        sweepTimer.start();
        QCOMPARE(sweep(journal), guard == "eligible" ? 28 : 0);
        if (guard == "eligible")
            qInfo().noquote() << kind << "eight bounded replay passes:" << sweepTimer.elapsed()
                              << "ms";
        if (guard != "eligible") {
            QCOMPARE(allReplay(journal), before);
            return;
        }
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "json_type(payload,'$.item.compacted')='object'"),
                 4);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE turn_sequence>4 AND "
                                 "json_type(payload,'$.item.compacted')='object'"),
                 0);
        QCOMPARE(sweep(journal), 0);
    }

    void supersededStartsKeepGuardsAndIncompleteSnapshots_data() {
        QTest::addColumn<QString>("guard");
        for (const char* guard :
             {"eligible", "failed", "empty-progress", "unacknowledged", "running", "recent",
              "unknown-start-field", "changed-field", "marked", "oversized", "nonterminal",
              "wrong-id", "wrong-type", "large-manifest", "question"})
            QTest::newRow(guard) << QString(guard);
    }
    void supersededStartsKeepGuardsAndIncompleteSnapshots() {
        QFETCH(QString, guard);
        QTemporaryDir directory;
        const auto path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path, guard != "unacknowledged"));
        Database database(path);
        QJsonObject fields{
            {"type", "mcpToolCall"},    {"server", "synthetic"},
            {"tool", "lookup"},         {"arguments", QJsonObject{{"query", QString(80000, 'x')}}},
            {"result", "small result"}, {"status", guard == "failed" ? "failed" : "completed"}};
        if (guard == "oversized")
            fields.insert("arguments", QJsonObject{{"query", QString(550000, 'x')}});
        QVERIFY(database.patchItems(fields));
        fields.insert("status", "inProgress");
        if (guard == "empty-progress")
            fields.insert("result", QJsonObject{});
        QVERIFY(database.patchItems(fields, "item.started"));
        if (guard == "running")
            QVERIFY(database.exec("UPDATE sessions SET state='running'"));
        if (guard == "recent")
            QVERIFY(database.exec(
                "UPDATE replay_events SET sent_at=strftime('%Y-%m-%dT%H:%M:%fZ','now')"));
        if (guard == "unknown-start-field")
            QVERIFY(database.exec(
                "UPDATE replay_events SET payload=json_set(payload,'$.item.unknown','keep') WHERE "
                "event_type='item.started'"));
        if (guard == "changed-field")
            QVERIFY(database.exec("UPDATE replay_events SET "
                                  "payload=json_set(payload,'$.item.arguments.query','different') "
                                  "WHERE event_type='item.completed'"));
        if (guard == "marked")
            QVERIFY(database.exec(
                "UPDATE replay_events SET payload=json_set(payload,'$.item.compacted',json('{}')) "
                "WHERE event_type='item.completed'"));
        if (guard == "nonterminal")
            QVERIFY(database.exec(
                "UPDATE replay_events SET payload=json_set(payload,'$.item.status','inProgress') "
                "WHERE event_type='item.completed'"));
        if (guard == "wrong-id")
            QVERIFY(database.exec(
                "UPDATE replay_events SET payload=json_set(payload,'$.item.id','different') WHERE "
                "event_type='item.completed'"));
        if (guard == "wrong-type")
            QVERIFY(database.exec(
                "UPDATE replay_events SET payload=json_set(payload,'$.item.type','unknown') WHERE "
                "event_type='item.completed'"));
        if (guard == "large-manifest")
            QVERIFY(database.exec(
                "UPDATE replay_events SET payload=json_set(payload,'$.large_event',json('{}')) "
                "WHERE event_type='item.completed'"));
        if (guard == "question")
            QVERIFY(database.exec("UPDATE replay_events SET event_type='question.requested' WHERE "
                                  "event_type='usage.limits'"));
        const auto before = allReplay(journal);
        QElapsedTimer timer;
        timer.start();
        QVERIFY(sweep(journal) >= 0);
        if (guard == "eligible")
            qInfo() << "Adjacent-start retention, 80000-byte arguments:" << timer.elapsed() << "ms";
        const bool eligible = guard == "eligible" || guard == "failed" || guard == "empty-progress";
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "event_type='item.started' AND turn_sequence<=4"),
                 eligible ? 0 : 4);
        QCOMPARE(database.scalar("SELECT count(*) FROM replay_events WHERE "
                                 "event_type='item.started' AND turn_sequence>4"),
                 20);
        QJsonArray expected;
        for (const auto& value : withoutUsage(before)) {
            const auto event = value.toObject();
            if (eligible && event.value("event_type") == "item.started" &&
                event.value("turn_sequence").toInteger() <= 4)
                continue;
            expected.append(value);
        }
        auto actual = withoutUsage(allReplay(journal));
        QCOMPARE(actual.size(), expected.size());
        for (qsizetype index = 0; index < actual.size(); ++index) {
            auto event = actual.at(index).toObject();
            auto payload = event.value("payload").toObject();
            auto item = payload.value("item").toObject();
            if (!item.contains("arguments_excerpt"))
                continue;
            // Completion detail trimming is independent of whether its start can be removed.
            QVERIFY(eligible || guard == "unknown-start-field" || guard == "oversized" ||
                    guard == "wrong-id");
            QCOMPARE(event.value("event_type"), QJsonValue("item.completed"));
            QVERIFY(event.value("turn_sequence").toInteger() <= 4);
            const auto originalArguments = expected.at(index)
                                               .toObject()
                                               .value("payload")
                                               .toObject()
                                               .value("item")
                                               .toObject()
                                               .value("arguments");
            const auto bytes =
                QJsonDocument(originalArguments.toObject()).toJson(QJsonDocument::Compact);
            const auto excerpt = item.value("arguments_excerpt").toString();
            QVERIFY(excerpt.startsWith("[Arguments trimmed"));
            const auto retained = excerpt.mid(excerpt.indexOf('\n') + 1).toUtf8();
            QVERIFY(!retained.isEmpty() && retained.size() <= 4096);
            QVERIFY(bytes.startsWith(retained));
            QCOMPARE(item.value("compacted")
                         .toObject()
                         .value("arguments")
                         .toObject()
                         .value("original_bytes")
                         .toInteger(),
                     bytes.size());
            QVERIFY(!item.contains("arguments"));
            item.remove("arguments_excerpt");
            item.remove("compacted");
            item.insert("arguments", originalArguments);
            payload.insert("item", item);
            event.insert("payload", payload);
            actual.replace(index, event);
        }
        QCOMPARE(actual, expected); // all other fields, cursor order and protected rows are exact
        QCOMPARE(sweep(journal), 0);
    }

    void invalidLimitsDoNotTouchHistory() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite");
        LocalJournal journal(path);
        QVERIFY(seed(journal, path));
        for (int rows : {-1, 0, 257})
            QVERIFY(!journal.pruneReplayDetails(cutoff(), rows).has_value());
        QVERIFY(!journal.pruneReplayDetails({}).has_value());
        QVERIFY(
            !journal.pruneReplayDetails(QDateTime::currentDateTimeUtc().addDays(1)).has_value());
        QCOMPARE(sweep(journal), 24);
    }
};

QTEST_GUILESS_MAIN(JournalUsageRetentionTest)
#include "JournalUsageRetentionTest.moc"
