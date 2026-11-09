#include "protocol/DaemonProtocol.h"
#include "providers/CodexAdapter.h"
#include "runtime/ApprovalBroker.h"
#include "runtime/SessionHost.h"
#include "storage/LocalJournal.h"
#include "storage/TranscriptWindowQueries.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace ketplus::daemon;

namespace {

bool seedSession(LocalJournal& journal, const QString& directory, const QString& id = "session") {
    return journal.upsertSession({id, directory, "codex", {}, {}, "ready", 1, 0, 0, "Synthetic"});
}

qint64 appendAnswer(LocalJournal& journal, const QString& id, const QString& text) {
    return journal.appendEvent(
        "session", 1, "item.completed",
        {{"item", QJsonObject{{"id", id}, {"type", "agentMessage"}, {"text", text}}}});
}

// The transfer named by the newest transcript item's large-event manifest.
QString transferIdOf(LocalJournal& journal) {
    return journal.transcriptWindow("session", 1)
        .events.last()
        .toObject()
        .value("payload")
        .toObject()
        .value("large_event")
        .toObject()
        .value("transfer_id")
        .toString();
}

bool execSql(const QString& path, const QStringList& statements) {
    const QString name = QUuid::createUuid().toString();
    bool ok = true;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(path);
        ok = database.open();
        for (const QString& statement : statements) {
            QSqlQuery query(database);
            if (ok && !query.exec(statement)) {
                qWarning("%s", qPrintable(query.lastError().text()));
                ok = false;
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

qint64 scalar(const QString& path, const QString& statement) {
    const QString name = QUuid::createUuid().toString();
    qint64 value = -1;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(path);
        QSqlQuery query(database);
        if (database.open() && query.exec(statement) && query.next())
            value = query.value(0).toLongLong();
        query.finish();
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
    return value;
}

QJsonObject request(const QString& method, const QJsonObject& payload = {}) {
    return {{"message_type", method},
            {"message_id", "synthetic-request"},
            {"session_id", "session"},
            {"session_generation", 1},
            {"payload", payload}};
}

} // namespace

class DaemonTranscriptWindowTest final : public QObject {
    Q_OBJECT

  private slots:
    void productionWindowQueriesUseIndexedSeeksWithoutHistorySorts() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        const QString name = QStringLiteral("window-explain-test");
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            database.setDatabaseName(directory.filePath("journal.sqlite3"));
            QVERIFY(database.open());
            for (const auto* sql :
                 {transcript_window_queries::latestTurn, transcript_window_queries::previousTurn,
                  transcript_window_queries::turnStart, transcript_window_queries::window,
                  transcript_window_queries::previousItemContext,
                  transcript_window_queries::firstTurnAfter, transcript_window_queries::nextTurn,
                  transcript_window_queries::windowAfter,
                  transcript_window_queries::hasEventAfter}) {
                QSqlQuery explain(database);
                const QString statement = QString::fromLatin1(sql);
                QVERIFY(explain.prepare("EXPLAIN QUERY PLAN " + statement));
                for (qsizetype index = 0; index < statement.count(QLatin1Char('?')); ++index) {
                    explain.addBindValue(1);
                }
                QVERIFY2(explain.exec(), qPrintable(explain.lastError().text()));
                QString details;
                while (explain.next()) {
                    details += explain.value(3).toString();
                }
                QVERIFY2(details.contains("SEARCH"), qPrintable(details));
                QVERIFY2(!details.contains("SCAN"), qPrintable(details));
                QVERIFY2(!details.contains("TEMP B-TREE"), qPrintable(details));
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(name);
    }

    void emptyWindowHasAnExplicitLiveBoundary() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        const auto page = journal.transcriptWindow("session", 1);
        QVERIFY2(page.errorCode.isEmpty(), qPrintable(journal.errorString()));
        QCOMPARE(page.snapshotCursor, 0);
        QCOMPARE(page.projectionRevision, 0);
        QVERIFY(page.events.isEmpty());
        QVERIFY(!page.hasMoreBefore);
    }

    void newestThreeTurnsAndBackwardHistoryAreLossless() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        for (int turn = 1; turn <= 5; ++turn) {
            QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
            QVERIFY(appendAnswer(journal, QString::number(turn), "answer") > 0);
            QVERIFY(journal.appendEvent("session", 1, "turn.completed", {{"status", "completed"}}) >
                    0);
        }
        const auto newest = journal.transcriptWindow("session", 1);
        QVERIFY2(newest.errorCode.isEmpty(), qPrintable(journal.errorString()));
        QCOMPARE(newest.snapshotCursor, 15);
        QCOMPARE(newest.events.size(), 9);
        QCOMPARE(newest.beforeCursor, 7);
        QVERIFY(newest.hasMoreBefore);
        QVERIFY(!newest.partialFirstTurn);
        const auto older = journal.transcriptWindow(
            "session", 1, newest.beforeCursor, newest.snapshotCursor, newest.projectionRevision);
        QCOMPARE(older.events.size(), 6);
        QCOMPARE(older.snapshotCursor, 15);
        QCOMPARE(older.beforeCursor, 1);
        QVERIFY(!older.hasMoreBefore);
    }

    void giantTurnContinuesWithinTurnWithoutSkippingOrRepeatingEvents() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
        constexpr int itemCount = 1'050;
        for (int item = 0; item < itemCount; ++item) {
            QVERIFY(appendAnswer(journal, QString::number(item), "answer") > 0);
        }
        auto page = journal.transcriptWindow("session", 1);
        QCOMPARE(page.events.size(), 128);
        QVERIFY(page.partialFirstTurn);
        QCOMPARE(page.events.last().toObject().value("cursor").toInteger(), itemCount + 1);
        QSet<qint64> cursors;
        int pages = 0;
        while (true) {
            QVERIFY2(page.errorCode.isEmpty(), qPrintable(journal.errorString()));
            QVERIFY(page.events.size() <= 128);
            qint64 previous = 0;
            for (const auto& value : page.events) {
                const qint64 cursor = value.toObject().value("cursor").toInteger();
                QVERIFY(cursor > previous);
                QVERIFY(!cursors.contains(cursor));
                cursors.insert(cursor);
                previous = cursor;
            }
            QVERIFY(++pages <= 10);
            if (!page.hasMoreBefore) {
                break;
            }
            page = journal.transcriptWindow("session", 1, page.beforeCursor, page.snapshotCursor,
                                            page.projectionRevision);
        }
        QCOMPARE(cursors.size(), itemCount + 1);
        QCOMPARE(pages, 9);
    }

    void encodedByteBoundHasWithinTurnContinuation() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
        const QString body(220 * 1024, QLatin1Char('x'));
        for (int item = 0; item < 3; ++item) {
            QVERIFY(appendAnswer(journal, QString::number(item), body) > 0);
        }
        const auto page = journal.transcriptWindow("session", 1);
        QVERIFY2(page.errorCode.isEmpty(), qPrintable(journal.errorString()));
        QCOMPARE(page.events.size(), 2);
        QVERIFY(QJsonDocument(page.events).toJson(QJsonDocument::Compact).size() < 512 * 1024);
        QVERIFY(page.hasMoreBefore);
        QVERIFY(page.partialFirstTurn);
        const auto older = journal.transcriptWindow("session", 1, page.beforeCursor,
                                                    page.snapshotCursor, page.projectionRevision);
        QCOMPARE(older.events.size(), 2);
        QVERIFY(!older.hasMoreBefore);
    }

    void incompleteAssistantPrefixIsExplicitAndCanBeRecovered() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
        for (int index = 0; index < 200; ++index) {
            QVERIFY(journal.appendEvent(
                        "session", 1, "assistant.delta",
                        {{"item_id", "answer"}, {"delta", QString::number(index) + ","}}) > 0);
        }
        const auto page = journal.transcriptWindow("session", 1);
        QCOMPARE(page.events.size(), 128);
        QCOMPARE(page.partialItems.size(), 1);
        QCOMPARE(page.partialItems.first().toObject().value("item_id").toString(), "answer");
        QCOMPARE(page.partialItems.first().toObject().value("turn_sequence").toInteger(), 1);
        QCOMPARE(page.partialItems.first().toObject().value("first_cursor").toInteger(),
                 page.beforeCursor);
        const auto older = journal.transcriptWindow("session", 1, page.beforeCursor,
                                                    page.snapshotCursor, page.projectionRevision);
        QCOMPARE(older.events.size(), 73);
        QVERIFY(older.partialItems.isEmpty());
        QString combined;
        for (const auto& events : {older.events, page.events}) {
            for (const auto& value : events) {
                combined += value.toObject().value("payload").toObject().value("delta").toString();
            }
        }
        QString expected;
        for (int index = 0; index < 200; ++index) {
            expected += QString::number(index) + ",";
        }
        QCOMPARE(combined, expected);
    }

    void appendOnlyRacesKeepTheOriginalSnapshotButCompactionInvalidatesIt() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
        for (int index = 0; index < 200; ++index) {
            QVERIFY(journal.appendEvent("session", 1, "assistant.delta",
                                        {{"item_id", "answer"}, {"delta", "x"}}) > 0);
        }
        const auto page = journal.transcriptWindow("session", 1);
        QVERIFY(journal.appendEvent("session", 1, "assistant.delta",
                                    {{"item_id", "answer"}, {"delta", "y"}}) > 0);
        const auto appended = journal.transcriptWindow(
            "session", 1, page.beforeCursor, page.snapshotCursor, page.projectionRevision);
        QVERIFY(appended.errorCode.isEmpty());
        QCOMPARE(appended.snapshotCursor, 201);
        QVERIFY(appendAnswer(journal, "answer", "final answer") > 0);
        const auto changed = journal.transcriptWindow("session", 1, page.beforeCursor,
                                                      page.snapshotCursor, page.projectionRevision);
        QCOMPARE(changed.errorCode, "snapshot_changed");
        QVERIFY(journal.isOpen());
        QVERIFY(changed.events.isEmpty());
        const auto reopened = journal.transcriptWindow("session", 1);
        QCOMPARE(reopened.events.size(), 2);
        QCOMPARE(reopened.snapshotCursor, 203);
        QCOMPARE(reopened.projectionRevision, page.projectionRevision + 1);
        QVERIFY(reopened.partialItems.isEmpty());
        QCOMPARE(reopened.events.last()
                     .toObject()
                     .value("payload")
                     .toObject()
                     .value("item")
                     .toObject()
                     .value("text")
                     .toString(),
                 "final answer");
    }

    void mailboxRestoreBindsProjectionAtomicallyAtTheAdvertisedHead() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        for (int turn = 1; turn <= 4; ++turn) {
            QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
            QVERIFY(appendAnswer(journal, QString::number(turn), "answer") > 0);
            QVERIFY(journal.appendEvent("session", 1, "turn.completed", {{"status", "completed"}}) >
                    0);
        }
        const qint64 snapshot = journal.sessions().first().lastCursor;
        const auto bound = journal.restoreTranscriptWindow("session", 1, 7, snapshot);
        QVERIFY2(bound.errorCode.isEmpty(), qPrintable(bound.errorMessage));
        QCOMPARE(bound.snapshotCursor, snapshot);
        QCOMPARE(bound.projectionRevision,
                 journal.transcriptWindow("session", 1).projectionRevision);
        QVERIFY(!bound.events.isEmpty());
        QVERIFY(bound.events.last().toObject().value("cursor").toInteger() < 7);

        // Ordinary local IPC remains strict: a continuation must return the
        // projection revision issued by its initial page.
        const auto localWithoutRevision = journal.transcriptWindow("session", 1, 7, snapshot);
        QCOMPARE(localWithoutRevision.errorCode, QStringLiteral("invalid_page"));

        QVERIFY(journal.appendEvent("session", 1, "assistant.delta",
                                    {{"item_id", "racing"}, {"delta", "partial"}}) > 0);
        const qint64 beforeCompaction = journal.sessions().first().lastCursor;
        QVERIFY(appendAnswer(journal, "racing", "final") > beforeCompaction);
        // The relay's head trails a running turn; the window is served as of that head.
        const auto raced = journal.restoreTranscriptWindow("session", 1, 7, beforeCompaction);
        QVERIFY2(raced.errorCode.isEmpty(), qPrintable(raced.errorMessage));
        QCOMPARE(raced.snapshotCursor, beforeCompaction);
        const auto trailing = journal.restoreTranscriptWindow("session", 1, 0, beforeCompaction);
        QVERIFY2(trailing.errorCode.isEmpty(), qPrintable(trailing.errorMessage));
        QCOMPARE(trailing.snapshotCursor, beforeCompaction);
        for (const QJsonValue& event : trailing.events)
            QVERIFY(event.toObject().value("cursor").toInteger() <= beforeCompaction);
        QCOMPARE(journal.restoreTranscriptWindow("session", 1, 0, snapshot + 100).errorCode,
                 QStringLiteral("invalid_page"));
    }

    void completionDoesNotDeleteAnotherTurnsReusedItemIdentity() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "first"}}) > 0);
        QVERIFY(journal.appendEvent("session", 1, "assistant.delta",
                                    {{"item_id", "reused"}, {"delta", "first partial"}}) > 0);
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "second"}}) > 0);
        QVERIFY(journal.appendEvent("session", 1, "assistant.delta",
                                    {{"item_id", "reused"}, {"delta", "second partial"}}) > 0);
        QVERIFY(appendAnswer(journal, "reused", "second final") > 0);
        const auto page = journal.transcriptWindow("session", 1);
        QCOMPARE(page.events.size(), 4);
        QCOMPARE(page.events.at(1).toObject().value("payload").toObject().value("delta").toString(),
                 "first partial");
    }

    void retainedProjectionSurvivesRawAcknowledgementAndRestart() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite3");
        qint64 revision = -1;
        {
            LocalJournal journal(path);
            QVERIFY(seedSession(journal, directory.path()));
            QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
            QVERIFY(journal.appendEvent("session", 1, "assistant.delta",
                                        {{"item_id", "answer"}, {"delta", "partial"}}) > 0);
            QVERIFY(appendAnswer(journal, "answer", "answer") > 0);
            revision = journal.transcriptWindow("session", 1).projectionRevision;
            QVERIFY(journal.acknowledgeCursor("desktop", "session", 1, 3));
            QVERIFY(journal.eventsAfter("session", 1, 0).isEmpty());
        }
        LocalJournal reopened(path);
        const auto page = reopened.transcriptWindow("session", 1);
        QVERIFY2(page.errorCode.isEmpty(), qPrintable(reopened.errorString()));
        QCOMPARE(page.projectionRevision, revision);
        QCOMPARE(page.events.size(), 2);
    }

    void missingCompletionTextCannotDeleteOrHideAnAssistantPrefix() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
        QVERIFY(journal.appendEvent("session", 1, "assistant.delta",
                                    {{"item_id", "answer"}, {"delta", "retain this text"}}) > 0);
        QVERIFY(journal.appendEvent(
                    "session", 1, "item.started",
                    {{"item",
                      QJsonObject{{"id", "answer"}, {"type", "agentMessage"}, {"text", ""}}}}) > 0);
        const auto restarted = journal.transcriptWindow("session", 1, 0, -1, -1, 3, 1);
        QCOMPARE(restarted.partialItems.size(), 1);
        QVERIFY(journal.appendEvent(
                    "session", 1, "item.completed",
                    {{"item", QJsonObject{{"id", "answer"}, {"type", "agentMessage"}}}}) > 0);
        const auto partial = journal.transcriptWindow("session", 1, 0, -1, -1, 3, 1);
        QCOMPARE(partial.events.size(), 1);
        QCOMPARE(partial.events.first().toObject().value("event_type").toString(),
                 "item.completed");
        QCOMPARE(partial.partialItems.size(), 1);
        QCOMPARE(partial.projectionRevision, 0);
        const auto all = journal.transcriptWindow("session", 1);
        QCOMPARE(all.events.size(), 4);
        QVERIFY(all.partialItems.isEmpty());
        // Explicit empty text is authoritative, unlike a missing text field.
        QVERIFY(appendAnswer(journal, "answer", "") > 0);
        const auto completed = journal.transcriptWindow("session", 1, 0, -1, -1, 3, 1);
        QVERIFY(completed.partialItems.isEmpty());
        QCOMPARE(completed.projectionRevision, 1);
        QCOMPARE(journal.transcriptWindow("session", 1).events.size(), 4);
    }

    void eightMiBItemUsesBoundedChunksAndRoundTripsLosslessly() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
        // Three-byte UTF-8 code points force fragment boundaries through the
        // encoded byte stream; decoding is allowed only after full reassembly.
        const QString text(3 * 1024 * 1024, QChar(0x20ac));
        const qint64 manifestCursor = appendAnswer(journal, "giant-answer", text);
        QVERIFY2(manifestCursor > 2, qPrintable(journal.errorString()));

        const auto window = journal.transcriptWindow("session", 1);
        QVERIFY2(window.errorCode.isEmpty(), qPrintable(window.errorMessage));
        QCOMPARE(window.events.size(), 2);
        QVERIFY(QJsonDocument(window.events).toJson(QJsonDocument::Compact).size() < 512 * 1024);
        QCOMPARE(window.partialItems.size(), 1);
        const QJsonObject manifest = window.events.last().toObject().value("payload").toObject();
        QVERIFY(!manifest.contains("item"));
        const QJsonObject reference = manifest.value("large_event").toObject();
        QCOMPARE(reference.value("version").toInt(), 1);
        QCOMPARE(reference.value("item_id").toString(), "giant-answer");
        QVERIFY(reference.value("total_bytes").toInteger() >= 8 * 1024 * 1024);

        QByteArray reconstructed;
        qint64 offset = 0;
        int fragments = 0;
        while (true) {
            const auto fragment = journal.largeEventFragment(
                "session", 1, reference.value("transfer_id").toString(), offset, 128 * 1024);
            QVERIFY2(fragment.errorCode.isEmpty(), qPrintable(fragment.errorMessage));
            QCOMPARE(fragment.offset, offset);
            QVERIFY(!fragment.data.isEmpty());
            reconstructed.append(fragment.data);
            offset = fragment.nextOffset;
            QVERIFY(++fragments < 100);
            if (fragment.complete)
                break;
        }
        QCOMPARE(offset, reference.value("total_bytes").toInteger());
        QCOMPARE(QString::fromLatin1(
                     QCryptographicHash::hash(reconstructed, QCryptographicHash::Sha256).toHex()),
                 reference.value("digest_sha256").toString());
        const QJsonObject decoded = QJsonDocument::fromJson(reconstructed).object();
        QCOMPARE(decoded.value("item").toObject().value("text").toString(), text);

        CodexAdapter adapter("/nonexistent-synthetic-provider");
        SessionHost host(&journal, &adapter);
        const QJsonObject wire =
            host.handleCommand(request("session.transcript_large_event",
                                       {{"transfer_id", reference.value("transfer_id")},
                                        {"offset", 0},
                                        {"maximum_bytes", 128 * 1024}}),
                               "desktop");
        QVERIFY(wire.value("ok").toBool());
        QCOMPARE(wire.value("large_event_fragment_version").toInt(), 1);
        QVERIFY(QJsonDocument(wire).toJson(QJsonDocument::Compact).size() <
                protocol::maximumEventPayloadBytes);

        int cloudChunks = 0;
        qint64 rawCursor = 0;
        while (rawCursor < manifestCursor) {
            const QJsonArray raw = journal.eventsAfter("session", 1, rawCursor, 500);
            QVERIFY(!raw.isEmpty());
            for (const auto& value : raw) {
                const QJsonObject event = value.toObject();
                const QByteArray payload =
                    QJsonDocument(event.value("payload").toObject()).toJson(QJsonDocument::Compact);
                QVERIFY(payload.size() <= protocol::maximumEventPayloadBytes);
                if (event.value("event_type") == "transcript.large_chunk")
                    ++cloudChunks;
                rawCursor = event.value("cursor").toInteger();
            }
        }
        QCOMPARE(cloudChunks, reference.value("chunk_count").toInt());
        QCOMPARE(rawCursor, manifestCursor);
    }

    void missingLargeItemFailsClosedAndDeletePurgesIt() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
        QVERIFY(appendAnswer(journal, "answer", QString(300 * 1024, QLatin1Char('x'))) > 0);
        const QJsonObject reference = journal.transcriptWindow("session", 1)
                                          .events.last()
                                          .toObject()
                                          .value("payload")
                                          .toObject()
                                          .value("large_event")
                                          .toObject();
        QVERIFY(!reference.isEmpty());
        QVERIFY(journal.deleteSession("session", 1));
        const auto fragment =
            journal.largeEventFragment("session", 1, reference.value("transfer_id").toString(), 0);
        QCOMPARE(fragment.errorCode, "large_event_not_found");
        QVERIFY(fragment.data.isEmpty());
    }

    void capabilityScopeLimitsAndLifecycleAreExplicit() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(journal.upsertSession({"session", directory.path(), "codex", "thread", "turn",
                                       "waiting_for_input", 1, 0, 0, "Synthetic"}));
        CodexAdapter adapter("/nonexistent-synthetic-provider");
        SessionHost host(&journal, &adapter);
        const auto catalog = host.handleCommand(request("session.list"), "desktop");
        QCOMPARE(catalog.value("transcript_window_version").toInt(), 1);
        const auto page = host.handleCommand(request("session.transcript_window"), "desktop");
        QVERIFY(page.value("ok").toBool());
        QCOMPARE(page.value("state").toString(), "waiting_for_input");
        QCOMPARE(page.value("active_turn_id").toString(), "turn");
        QCOMPARE(page.value("current_turn_sequence").toInteger(), 0);
        QVERIFY(page.value("pending_approvals").toArray().isEmpty());
        auto stale = request("session.transcript_window");
        stale.insert("session_generation", 2);
        QCOMPARE(host.handleCommand(stale, "desktop").value("error_code").toString(),
                 "stale_generation");
        for (const QJsonObject& invalid :
             {QJsonObject{{"event_limit", 129}}, QJsonObject{{"event_limit", 1.5}},
              QJsonObject{{"event_limit", 3'000'000'000LL}}, QJsonObject{{"before_cursor", 1}},
              QJsonObject{{"snapshot_cursor", 0}}, QJsonObject{{"turn_limit", "3"}}}) {
            QCOMPARE(host.handleCommand(request("session.transcript_window", invalid), "desktop")
                         .value("error_code")
                         .toString(),
                     "invalid_page");
        }
        auto* broker = host.findChild<ApprovalBroker*>();
        QVERIFY(broker);
        const QJsonObject currentQuestion{
            {"questions", QJsonArray{QJsonObject{
                              {"id", "scope"},
                              {"question", "Which scope?"},
                              {"options", QJsonArray{QJsonObject{{"label", "Workspace"}}}}}}}};
        const auto approvalId = broker->request("session", 1, "request", "input", currentQuestion);
        const auto waiting = host.handleCommand(request("session.transcript_window"), "desktop");
        QCOMPARE(waiting.value("pending_approvals").toArray().size(), 1);
        const auto pending = waiting.value("pending_approvals").toArray().first().toObject();
        QCOMPARE(pending.value("approval_id").toString(), approvalId);
        QCOMPARE(pending.value("questions").toArray().size(), 1);
        QVERIFY(pending.contains("expires_at"));
        const QString optionId = pending.value("questions")
                                     .toArray()
                                     .first()
                                     .toObject()
                                     .value("options")
                                     .toArray()
                                     .first()
                                     .toObject()
                                     .value("id")
                                     .toString();
        QVERIFY(broker
                    ->resolve(approvalId, "session", 1, "input",
                              {{"request_id", approvalId},
                               {"revision", 1},
                               {"action", "answer"},
                               {"answers",
                                QJsonArray{QJsonObject{{"question_id", "scope"},
                                                       {"question_revision", 1},
                                                       {"option_ids", QJsonArray{optionId}}}}}})
                    .accepted);
        const auto resolved = host.handleCommand(request("session.pending_approvals"), "desktop");
        QVERIFY(resolved.value("pending_approvals").toArray().isEmpty());
        // The historical event remains; it must not resurrect the current queue.
        QVERIFY(!journal.transcriptWindow("session", 1).events.isEmpty());
    }

    void pendingApprovalsAreCurrentScopedBoundedAndDoNotReplayResolvedRequests() {
        ApprovalBroker broker;
        QStringList ids;
        const QJsonObject questionDetails{
            {"questions", QJsonArray{QJsonObject{
                              {"id", "scope"},
                              {"question", "Choose scope"},
                              {"options", QJsonArray{QJsonObject{{"label", "Workspace"}}}}}}}};
        for (int index = 0; index < 40; ++index) {
            ids.append(broker.request("session", 1, index, "input", questionDetails));
        }
        ids.sort();
        auto page = broker.pendingPage("session", 1);
        QCOMPARE(page.value("pending_approvals").toArray().size(), 32);
        QVERIFY(page.value("pending_approvals_has_more").toBool());
        QCOMPARE(page.value("pending_approvals_next_id").toString(), ids.at(31));
        const auto older = broker.pendingPage("session", 1, ids.at(31));
        QCOMPARE(older.value("pending_approvals").toArray().size(), 8);
        QVERIFY(!older.value("pending_approvals_has_more").toBool());
        QVERIFY(
            broker.pendingPage("other-session", 1).value("pending_approvals").toArray().isEmpty());
        QVERIFY(broker.pendingPage("session", 2).value("pending_approvals").toArray().isEmpty());
        const QJsonObject firstPending =
            page.value("pending_approvals").toArray().first().toObject();
        const QString firstOption = firstPending.value("questions")
                                        .toArray()
                                        .first()
                                        .toObject()
                                        .value("options")
                                        .toArray()
                                        .first()
                                        .toObject()
                                        .value("id")
                                        .toString();
        QVERIFY(broker
                    .resolve(ids.first(), "session", 1, "input",
                             {{"request_id", ids.first()},
                              {"revision", 1},
                              {"action", "answer"},
                              {"answers",
                               QJsonArray{QJsonObject{{"question_id", "scope"},
                                                      {"question_revision", 1},
                                                      {"option_ids", QJsonArray{firstOption}}}}}})
                    .accepted);
        page = broker.pendingPage("session", 1);
        QVERIFY(page.value("pending_approvals")
                    .toArray()
                    .first()
                    .toObject()
                    .value("approval_id")
                    .toString() != ids.first());
        const QString expiring = broker.request("expiry", 1, 100, "input", questionDetails, 1);
        QVERIFY(!expiring.isEmpty());
        QTest::qWait(2);
        QVERIFY(broker.pendingPage("expiry", 1).value("pending_approvals").toArray().isEmpty());
    }

    void startTurnReportsJournalErrorAndDoesNotStartWhenPromptCannotBeJournaled() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite3");
        LocalJournal journal(path);
        QVERIFY(execSql(path, {"CREATE TRIGGER reject_user BEFORE INSERT ON events WHEN "
                               "NEW.event_type = 'user.message' BEGIN "
                               "SELECT RAISE(ABORT, 'synthetic journal failure'); END"}));
        CodexAdapter adapter("/nonexistent-synthetic-provider");
        SessionHost host(&journal, &adapter);
        const QJsonObject result =
            host.handleCommand({{"message_type", "session.start_turn"},
                                {"message_id", "start-fails"},
                                {"payload", QJsonObject{{"worktree_path", directory.path()},
                                                        {"prompt", "<skill>wrapped</skill> typed"},
                                                        {"display_text", "typed"}}}},
                               "desktop");
        QVERIFY(!result.value("ok").toBool());
        QCOMPARE(result.value("error_code").toString(), "journal_error");
        QVERIFY(!host.hasActiveSessions());
        const QJsonArray sessions =
            host.handleCommand({{"message_type", "session.list"}}, "desktop")
                .value("sessions")
                .toArray();
        QCOMPARE(sessions.size(), 1);
        QVERIFY(sessions.first().toObject().value("state").toString() != "starting");
        // Title comes from what the user typed, not the wrapped provider prompt.
        QCOMPARE(sessions.first().toObject().value("title").toString(), "typed");
    }

    void configurationReadIsNeverServedFromTheCommandCache() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        CodexAdapter adapter("/nonexistent-synthetic-provider");
        SessionHost host(&journal, &adapter);
        const QJsonObject read = request("session.configuration.read");
        QCOMPARE(host.handleCommand(read, "desktop").value("revision").toInteger(), 1);
        QJsonObject apply = request("session.execution_policy.apply",
                                    {{"execution_policy", "ask"}, {"expected_revision", 1}});
        apply.insert("message_id", "apply-policy");
        QVERIFY(host.handleCommand(apply, "desktop").value("ok").toBool());
        QCOMPARE(host.handleCommand(read, "desktop").value("revision").toInteger(), 2);
        QVERIFY(journal.commandResult("synthetic-request").isEmpty());
        // The event names what changed, so the transcript does not call it a model change.
        QJsonObject policy = request("session.execution_policy.apply",
                                     {{"execution_policy", "read-only"}, {"expected_revision", 2}});
        policy.insert("message_id", "apply-read-only");
        QVERIFY(host.handleCommand(policy, "desktop").value("ok").toBool());
        const auto events =
            host.handleCommand(request("session.events_after", {{"after_cursor", 0}}), "desktop")
                .value("events")
                .toArray();
        QJsonObject changed;
        for (const auto& event : events) {
            if (event.toObject().value("event_type").toString() == "session.configuration_changed")
                changed = event.toObject().value("payload").toObject();
        }
        QCOMPARE(changed.value("changed").toArray(), QJsonArray{"execution_policy"});
    }

    void replayAfterPrunedCursorReportsExplicitGap() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        QVERIFY(seedSession(journal, directory.path(), "empty"));
        for (int index = 0; index < 3; ++index)
            QVERIFY(journal.appendEvent("session", 1, "runtime.note", {{"index", index}}) > 0);
        QVERIFY(journal.acknowledgeCursor("desktop", "session", 1, 2));
        CodexAdapter adapter("/nonexistent-synthetic-provider");
        SessionHost host(&journal, &adapter);
        // A client that never acknowledged asks from the beginning.
        const QJsonObject expired =
            host.handleCommand(request("session.events_after", {{"after_cursor", 0}}), "new");
        QCOMPARE(expired.value("error_code").toString(), "cursor_expired");
        QVERIFY(expired.value("events_pruned").toBool());
        QCOMPARE(expired.value("earliest_available_cursor").toInteger(), 3);
        const QJsonObject retained =
            host.handleCommand(request("session.events_after", {{"after_cursor", 2}}), "new");
        QVERIFY(retained.value("ok").toBool());
        QVERIFY(!retained.value("events_pruned").toBool());
        QCOMPARE(retained.value("events").toArray().size(), 1);
        // A session with no projected events yet is not a journal error in compacted mode.
        QJsonObject empty =
            request("session.events_after", {{"after_cursor", 0}, {"compact_completed", true}});
        empty.insert("session_id", "empty");
        QVERIFY(host.handleCommand(empty, "new").value("ok").toBool());
    }

    void truncatedTranscriptPageKeepsNewestTail() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
        qint64 last = 0;
        for (int item = 0; item < protocol::maximumReplayEvents + 20; ++item)
            last = appendAnswer(journal, QString::number(item), "answer");
        QVERIFY(last > 0);
        const TranscriptPage page = journal.transcriptPage("session", 1, 0, 3);
        QVERIFY(page.truncated);
        QVERIFY(page.hasMoreBefore);
        QCOMPARE(page.events.last().toObject().value("cursor").toInteger(), last);
        QCOMPARE(page.events.first().toObject().value("cursor").toInteger(), page.firstCursor);
        QVERIFY(page.firstCursor > 1);
        QCOMPARE(page.events.size(), protocol::maximumReplayEvents);
    }

    void truncatedTurnIsFullyRetrievableWithBeforeCursor() {
        QTemporaryDir directory;
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(seedSession(journal, directory.path()));
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "first"}}) > 0);
        appendAnswer(journal, "early", "early answer");
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "big"}}) > 0);
        for (int item = 0; item < protocol::maximumReplayEvents * 2 + 7; ++item)
            appendAnswer(journal, QString::number(item), "answer");
        QList<qint64> expected;
        for (qint64 after = 0;;) {
            const QJsonArray chunk = journal.eventsAfter("session", 1, after, 100000);
            if (chunk.isEmpty())
                break;
            for (const QJsonValue& event : chunk) {
                after = event.toObject().value("cursor").toInteger();
                if (event.toObject().value("turn_sequence").toInteger() > 0)
                    expected.append(after);
            }
        }
        CodexAdapter adapter("/nonexistent-synthetic-provider");
        SessionHost host(&journal, &adapter);

        QList<qint64> collected;
        QJsonObject page =
            host.handleCommand(request("session.transcript_page", {{"limit", 1}}), "desktop");
        QVERIFY(page.value("ok").toBool());
        QVERIFY(page.value("truncated").toBool());
        int pages = 0;
        while (true) {
            QList<qint64> chunk;
            for (const QJsonValue& event : page.value("events").toArray())
                chunk.append(event.toObject().value("cursor").toInteger());
            collected = chunk + collected;
            if (!page.value("has_more_before").toBool())
                break;
            QVERIFY(++pages < 20);
            QJsonObject next{{"limit", 1}};
            if (page.value("truncated").toBool())
                next.insert("before_cursor", page.value("first_event_cursor").toInteger());
            else
                next.insert("before_turn_sequence",
                            page.value("earliest_turn_sequence").toInteger());
            page = host.handleCommand(request("session.transcript_page", next), "desktop");
            QVERIFY(page.value("ok").toBool());
        }
        QVERIFY(pages >= 3);
        QCOMPARE(collected, expected);

        // Old behaviour: before_turn_sequence stays exclusive and skips the cut turn's remainder.
        const QJsonObject first =
            host.handleCommand(request("session.transcript_page", {{"limit", 1}}), "desktop");
        const qint64 cutTurn = first.value("earliest_turn_sequence").toInteger();
        const QJsonObject legacy = host.handleCommand(
            request("session.transcript_page", {{"before_turn_sequence", cutTurn}}), "desktop");
        QVERIFY(legacy.value("ok").toBool());
        QVERIFY(!legacy.value("truncated").toBool());
        for (const QJsonValue& event : legacy.value("events").toArray())
            QVERIFY(event.toObject().value("turn_sequence").toInteger() < cutTurn);

        for (const QJsonObject& bad :
             {QJsonObject{{"before_cursor", 0}}, QJsonObject{{"before_cursor", -3}},
              QJsonObject{{"before_cursor", "5"}},
              QJsonObject{{"before_cursor", 5}, {"before_turn_sequence", 2}}})
            QCOMPARE(host.handleCommand(request("session.transcript_page", bad), "desktop")
                         .value("error_code")
                         .toString(),
                     "invalid_page");
    }

    void afterCursorChainsEveryEventExactlyOnceWithinSnapshot() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite3");
        LocalJournal journal(path);
        QVERIFY(seedSession(journal, directory.path()));
        for (int turn = 0; turn < 30; ++turn) {
            QVERIFY(journal.appendEvent("session", 1, "user.message",
                                        {{"text", QString("prompt-%1").arg(turn)}}) > 0);
            // One giant turn forces event-limit truncation inside a turn.
            const int answers = turn == 12 ? 300 : 1;
            for (int item = 0; item < answers; ++item)
                appendAnswer(journal, QString("%1-%2").arg(turn).arg(item), "answer");
        }
        CodexAdapter adapter("/nonexistent-synthetic-provider");
        SessionHost host(&journal, &adapter);
        const QJsonObject initial = host.handleCommand(
            request("session.transcript_window", {{"turn_limit", 3}}), "desktop");
        QVERIFY2(initial.value("ok").toBool(),
                 qPrintable(initial.value("error_message").toString()));
        QVERIFY(!initial.value("has_more_after").toBool());
        const qint64 snapshot = initial.value("snapshot_cursor").toInteger();
        const qint64 revision = initial.value("projection_revision").toInteger();

        // Reference: the complete backward chain.
        QList<qint64> expected;
        QJsonObject page = initial;
        for (int pages = 0;; ++pages) {
            QVERIFY(pages < 200);
            QList<qint64> chunk;
            for (const QJsonValue& event : page.value("events").toArray())
                chunk.append(event.toObject().value("cursor").toInteger());
            expected = chunk + expected;
            if (!page.value("has_more_before").toBool())
                break;
            page = host.handleCommand(
                request("session.transcript_window",
                        {{"before_cursor", page.value("before_cursor").toInteger()},
                         {"snapshot_cursor", snapshot},
                         {"projection_revision", revision},
                         {"turn_limit", 20}}),
                "desktop");
            QVERIFY2(page.value("ok").toBool(), qPrintable(page.value("error_message").toString()));
        }
        QVERIFY(expected.size() > 300);

        // Live events after the snapshot are never returned by a fenced forward chain.
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "live"}}) > snapshot);

        for (const int turnLimit : {1, 3, 20}) {
            QList<qint64> collected{expected.first()};
            qint64 after = expected.first();
            int pages = 0;
            bool truncatedInsideTurn = false;
            for (bool more = true; more;) {
                QVERIFY(++pages < 400);
                const QJsonObject next = host.handleCommand(
                    request("session.transcript_window", {{"after_cursor", after},
                                                          {"snapshot_cursor", snapshot},
                                                          {"projection_revision", revision},
                                                          {"turn_limit", turnLimit}}),
                    "desktop");
                QVERIFY2(next.value("ok").toBool(),
                         qPrintable(next.value("error_message").toString()));
                const QJsonArray events = next.value("events").toArray();
                QVERIFY(!events.isEmpty());
                QVERIFY(events.size() <= 128);
                QSet<qint64> turns;
                for (const QJsonValue& event : events) {
                    const qint64 cursor = event.toObject().value("cursor").toInteger();
                    QVERIFY(cursor > after && cursor <= snapshot);
                    collected.append(cursor);
                    turns.insert(event.toObject().value("turn_sequence").toInteger());
                }
                QVERIFY(turns.size() <= turnLimit);
                truncatedInsideTurn =
                    truncatedInsideTurn || next.value("partial_first_turn").toBool();
                QCOMPARE(next.value("before_cursor").toInteger(),
                         events.first().toObject().value("cursor").toInteger());
                QCOMPARE(next.value("after_cursor").toInteger(),
                         events.last().toObject().value("cursor").toInteger());
                QVERIFY(next.value("has_more_before").toBool());
                after = next.value("after_cursor").toInteger();
                more = next.value("has_more_after").toBool();
            }
            QCOMPARE(collected, expected);
            QVERIFY(truncatedInsideTurn);
            // At the snapshot head the page is empty and reports no newer events.
            const QJsonObject head = host.handleCommand(
                request("session.transcript_window", {{"after_cursor", snapshot},
                                                      {"snapshot_cursor", snapshot},
                                                      {"projection_revision", revision}}),
                "desktop");
            QVERIFY(head.value("ok").toBool());
            QVERIFY(head.value("events").toArray().isEmpty());
            QVERIFY(!head.value("has_more_after").toBool());
        }

        const auto errorCode = [&](const QJsonObject& payload) {
            return host.handleCommand(request("session.transcript_window", payload), "desktop")
                .value("error_code")
                .toString();
        };
        const QJsonObject fence{{"snapshot_cursor", snapshot}, {"projection_revision", revision}};
        const auto with = [&](QJsonObject payload) {
            for (auto it = fence.begin(); it != fence.end(); ++it)
                payload.insert(it.key(), it.value());
            return payload;
        };
        const auto errorMessage = [&](const QJsonObject& payload) {
            return host.handleCommand(request("session.transcript_window", payload), "desktop")
                .value("error_message")
                .toString();
        };
        QCOMPARE(errorMessage(with({{"after_cursor", 5}, {"before_cursor", 9}})),
                 "after_cursor cannot be combined with before_cursor.");
        QCOMPARE(errorMessage(with({{"after_cursor", 0}})),
                 "after_cursor must be a positive event cursor.");
        QCOMPARE(errorCode(with({{"after_cursor", 5}, {"before_cursor", 9}})), "invalid_page");
        QCOMPARE(errorCode(with({{"after_cursor", 5}, {"before_cursor", 0}})), "invalid_page");
        QCOMPARE(errorCode(with({{"after_cursor", 0}})), "invalid_page");
        QCOMPARE(errorCode(with({{"after_cursor", -2}})), "invalid_page");
        QCOMPARE(errorCode(with({{"after_cursor", "5"}})), "invalid_page");
        QCOMPARE(errorCode(with({{"after_cursor", snapshot + 1}})), "invalid_page");
        QCOMPARE(errorCode({{"after_cursor", 5}}), "invalid_page"); // Unfenced.
        QCOMPARE(errorCode(with({{"after_cursor", 5}, {"turn_limit", 21}})), "invalid_page");
        QVERIFY(host.handleCommand(request("session.transcript_window",
                                           with({{"after_cursor", 5}, {"turn_limit", 20}})),
                                   "desktop")
                    .value("ok")
                    .toBool());
        const QJsonObject restoreForward = host.restoreTranscriptWindow(request(
            "session.transcript_window", {{"after_cursor", 5}, {"snapshot_cursor", snapshot}}));
        QCOMPARE(restoreForward.value("error_code").toString(), "invalid_page");
        QCOMPARE(restoreForward.value("error_message").toString(),
                 "Restore requests cannot page forward with after_cursor.");
        const QJsonObject listed = host.handleCommand(request("session.list"), "desktop");
        QCOMPARE(listed.value("transcript_window_after_cursor").toBool(), true);
        QCOMPARE(listed.value("transcript_window_max_turns").toInt(), 20);

        // Compaction revision fence applies to forward continuations too.
        QVERIFY(execSql(path, {"UPDATE replay_projection_state SET revision = revision + 1"}));
        QCOMPARE(errorCode(with({{"after_cursor", 5}})), "snapshot_changed");
    }

    void largePayloadIsStoredInFragmentRowsAndPrunedWhenUnreferenced() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite3");
        QString transferId;
        {
            LocalJournal journal(path);
            QVERIFY(seedSession(journal, directory.path()));
            QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "prompt"}}) > 0);
            const qint64 manifest = appendAnswer(journal, "answer", QString(600 * 1024, 'x'));
            QVERIFY(manifest > 0);
            QCOMPARE(scalar(path, "SELECT MAX(length(payload)) FROM large_event_payloads"), 0);
            QVERIFY(scalar(path, "SELECT COUNT(*) FROM large_event_payload_chunks") >= 5);
            transferId = transferIdOf(journal);
            const auto middle = journal.largeEventFragment("session", 1, transferId, 200 * 1024);
            QVERIFY2(middle.errorCode.isEmpty(), qPrintable(middle.errorMessage));
            QCOMPARE(middle.nextOffset, 256 * 1024); // ends at the stored slice boundary

            // Raw events are pruned, but the compacted transcript still references the payload.
            QVERIFY(journal.acknowledgeCursor("desktop", "session", 1, manifest));
            QVERIFY(journal.largeEventFragment("session", 1, transferId, 0).errorCode.isEmpty());
            QVERIFY(execSql(path, {"DELETE FROM replay_events"}));
            // Acknowledging the same cursor again changes nothing and scans nothing.
            QVERIFY(journal.acknowledgeCursor("desktop", "session", 1, manifest));
            QVERIFY(journal.largeEventFragment("session", 1, transferId, 0).errorCode.isEmpty());
            // An acknowledgement checks only the transfers its removed events name; this one
            // names none, so the now unreferenced payload waits for the retention pass.
            const qint64 next =
                journal.appendEvent("session", 1, "user.message", {{"text", "next"}});
            QVERIFY(next > manifest);
            QVERIFY(journal.acknowledgeCursor("desktop", "session", 1, next));
            QVERIFY(journal.largeEventFragment("session", 1, transferId, 0).errorCode.isEmpty());
        }
        LocalJournal reopened(path);
        QCOMPARE(reopened.largeEventFragment("session", 1, transferId, 0).errorCode,
                 "large_event_not_found");
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM large_event_payload_chunks"), 0);
    }

    void acknowledgementCollectsThePayloadsItsRemovedEventsName() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite3");
        LocalJournal journal(path);
        QVERIFY(seedSession(journal, directory.path()));
        const qint64 kept = appendAnswer(journal, "kept", QString(300 * 1024, 'k'));
        QVERIFY(kept > 0);
        const QString keptTransfer = transferIdOf(journal);
        const qint64 dropped = appendAnswer(journal, "dropped", QString(300 * 1024, 'd'));
        QVERIFY(dropped > kept);
        const QString droppedTransfer = transferIdOf(journal);
        QVERIFY(keptTransfer != droppedTransfer);
        // Only the second answer's replay manifest is gone; its raw events are removed next.
        QVERIFY(execSql(path, {"DELETE FROM replay_events WHERE item_id = 'dropped'"}));
        QVERIFY(journal.acknowledgeCursor("desktop", "session", 1, dropped));
        QCOMPARE(journal.largeEventFragment("session", 1, droppedTransfer, 0).errorCode,
                 "large_event_not_found");
        QVERIFY(journal.largeEventFragment("session", 1, keptTransfer, 0).errorCode.isEmpty());
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM large_event_payloads"), 1);
    }

    void repeatedAcknowledgementLeavesTheJournalUntouched() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite3");
        LocalJournal journal(path);
        QVERIFY(seedSession(journal, directory.path()));
        const qint64 first = journal.appendEvent("session", 1, "user.message", {{"text", "one"}});
        QVERIFY(journal.appendEvent("session", 1, "user.message", {{"text", "two"}}) > first);
        QVERIFY(journal.acknowledgeCursor("desktop", "session", 1, first));
        QVERIFY(execSql(path, {"UPDATE client_cursors SET acknowledged_at = 'marker'"}));
        // Behind or at the stored cursor: no write, so the marker survives.
        QVERIFY(journal.acknowledgeCursor("desktop", "session", 1, first));
        QVERIFY(journal.acknowledgeCursor("desktop", "session", 1, 0));
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM client_cursors WHERE acknowledged_at = "
                              "'marker'"),
                 1);
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM events"), 1);
        // Ahead of it: recorded, and the acknowledged event is pruned.
        QVERIFY(journal.acknowledgeCursor("desktop", "session", 1, first + 1));
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM client_cursors WHERE acknowledged_at = "
                              "'marker'"),
                 0);
        QCOMPARE(scalar(path, "SELECT COUNT(*) FROM events"), 0);
    }

    void retentionPrunesExpiredCommandResultsAndTombstones() {
        QTemporaryDir directory;
        const QString path = directory.filePath("journal.sqlite3");
        {
            LocalJournal journal(path);
            QVERIFY(seedSession(journal, directory.path()));
            QVERIFY(journal.storeCommandResult("old", {{"ok", true}}));
            QVERIFY(journal.storeCommandResult("fresh", {{"ok", true}}));
            QVERIFY(journal.deleteSession("session", 1));
            QVERIFY(execSql(path,
                            {"UPDATE command_results SET completed_at = '2000-01-01T00:00:00Z' "
                             "WHERE message_id = 'old'",
                             "UPDATE session_tombstones SET deleted_at = '2000-01-01T00:00:00Z'"}));
        }
        LocalJournal reopened(path);
        QVERIFY2(reopened.isOpen(), qPrintable(reopened.errorString()));
        QVERIFY(reopened.commandResult("old").isEmpty());
        QVERIFY(!reopened.commandResult("fresh").isEmpty());
        QVERIFY(reopened.sessionTombstone("session", 1).sessionId.isEmpty());
    }

    void providerCrashRestartsBackOffExponentially() {
        CodexAdapter adapter("/nonexistent-synthetic-provider");
        int failures = 0;
        const auto callback = [&failures](const QJsonObject&, const QString& error) {
            if (!error.isEmpty())
                ++failures;
        };
        adapter.call("model/list", {}, callback);
        QTRY_COMPARE_WITH_TIMEOUT(failures, 1, 2000);
        QCOMPARE(adapter.restartDelayMs(), CodexAdapter::initialRestartDelayMs);
        adapter.call("model/list", {}, callback);
        QVERIFY(adapter.restartScheduled());
        QTest::qWait(200);
        QCOMPARE(failures, 1); // not respawned immediately
        QTRY_COMPARE_WITH_TIMEOUT(failures, 2, 3000);
        QCOMPARE(adapter.restartDelayMs(), 2 * CodexAdapter::initialRestartDelayMs);
    }
};

QTEST_GUILESS_MAIN(DaemonTranscriptWindowTest)
#include "DaemonTranscriptWindowTest.moc"
