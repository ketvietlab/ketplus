#include "TranscriptWindowTestSupport.h"

#include <algorithm>

class DesktopTranscriptLargeEventFetchTest final : public QObject {
    Q_OBJECT
  private slots:
    void reopeningAdvancedConversationReplacesCachedRowsInOneRead() {
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "a");
        int latestCursor = 10;
        FakeDaemon daemon(env.directory.filePath("daemon.sock"), [&](const QString& type,
                                                                     const QJsonObject& envelope) {
            if (type == "session.list") {
                QJsonArray sessions;
                for (const QString& id : {QStringLiteral("a"), QStringLiteral("b")})
                    sessions.append(QJsonObject{{"session_id", id},
                                                {"worktree_path", path},
                                                {"session_generation", 1},
                                                {"state", id == "a" ? "running" : "ready"},
                                                {"last_event_cursor", 1000}});
                return QJsonObject{{"transcript_window_version", 1}, {"sessions", sessions}};
            }
            if (type != "session.transcript_window")
                return QJsonObject{};
            const QString id = envelope.value("session_id").toString();
            const auto params = envelope.value("payload").toObject();
            const qint64 before = params.value("before_cursor").toInteger();
            const int cursor = id == "b"    ? 5
                               : before > 0 ? latestCursor == 10 ? 1 : 10
                                            : latestCursor;
            const QString text = id == "b"        ? QStringLiteral("other")
                                 : cursor == 1000 ? QStringLiteral("newest")
                                 : cursor == 10   ? QStringLiteral("previous")
                                                  : QStringLiteral("oldest");
            return QJsonObject{
                {"transcript_window_version", 1},
                {"session_id", id},
                {"session_generation", 1},
                {"events",
                 QJsonArray{windowEvent(id, "user.message", {{"text", text}}, cursor, cursor)}},
                {"before_cursor", cursor},
                {"has_more_before", id == "a" && before == 0},
                {"snapshot_cursor", id == "b" ? 5 : latestCursor},
                {"projection_revision", 1},
                {"current_turn_sequence", cursor},
                {"state", id == "a" ? "running" : "ready"}};
        });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 2, 5000);
        desktop.selectConversation(path, "b");
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 1, 5000);
        // The running conversation advances while inactive. The daemon broadcasts
        // its events to every client, which marks the retained window stale.
        QSignalSpy invalidated(&server, &CodexAppServer::daemonTranscriptInvalidated);
        latestCursor = 1000;
        daemon.push(liveEvent("a", "user.message", {{"text", "newest"}}, 1000, 1000));
        QTRY_COMPARE(invalidated.size(), 1);
        const qsizetype requestStart = daemon.envelopes.size();
        QSignalSpy resets(&desktop, &AgentSessionManager::messagesReset);
        QSignalSpy prepended(&desktop, &AgentSessionManager::messagesPrepended);
        desktop.selectConversation(path, "a");
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).last().toMap().value("body").toString(),
                                  "newest", 5000);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QTest::qWait(200);
        QList<QJsonObject> reads;
        for (qsizetype i = requestStart; i < daemon.envelopes.size(); ++i) {
            const auto request = daemon.envelopes.at(i);
            if (request.value("message_type") == "session.transcript_window" &&
                request.value("session_id") == "a")
                reads.append(request.value("payload").toObject());
        }
        QVERIFY(!reads.isEmpty());
        QCOMPARE(reads.at(0).value("before_cursor").toInteger(), 0);
        QVERIFY(!reads.at(0).contains("after_cursor"));
        // The cached rows stay visible while the refresh reads, so the newest
        // window is read whole and replaces them once. No backfill prepends
        // older turns afterwards and shifts the transcript a second time.
        QCOMPARE(reads.at(0).value("turn_limit").toInteger(), transcript_paging::minimumTurns);
        QCOMPARE(resets.size(), 2); // The cached rows, then the newest window.
        QCOMPARE(prepended.size(), 0);
    }

    void independentItemsFetchConcurrently() {
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = QStringLiteral("heavy");
        rememberConversation(path, id);
        const QStringList ids{QStringLiteral("answer-0"), QStringLiteral("answer-1")};
        QList<QByteArray> sources;
        QStringList digests;
        QJsonArray events;
        for (int i = 0; i < ids.size(); ++i) {
            const QJsonObject payload{
                {"item", QJsonObject{{"id", ids.at(i)},
                                     {"type", "agentMessage"},
                                     {"phase", "final_answer"},
                                     {"text", QString(150 * 1024, QChar('a' + i))}}}};
            sources.append(QJsonDocument(payload).toJson(QJsonDocument::Compact));
            digests.append(QString::fromLatin1(
                QCryptographicHash::hash(sources.last(), QCryptographicHash::Sha256).toHex()));
            const QJsonObject reference{{"version", 1},
                                        {"transfer_id", ids.at(i)},
                                        {"event_type", "item.completed"},
                                        {"item_id", ids.at(i)},
                                        {"revision", 1},
                                        {"encoding", "json-utf8"},
                                        {"digest_sha256", digests.last()},
                                        {"total_bytes", sources.last().size()}};
            events.append(
                windowEvent(id, "item.completed", {{"large_event", reference}}, i + 1, i + 1));
        }
        const auto fragmentFor = [&](const QJsonObject& request) {
            const auto params = request.value("payload").toObject();
            const int i = ids.indexOf(params.value("transfer_id").toString());
            if (i < 0)
                return QJsonObject{};
            const qint64 offset = params.value("offset").toInteger();
            const auto bytes = sources.at(i).mid(offset, 128 * 1024);
            const qint64 next = offset + bytes.size();
            return QJsonObject{
                {"large_event_fragment_version", 1},
                {"session_id", id},
                {"session_generation", 1},
                {"transfer_id", ids.at(i)},
                {"event_type", "item.completed"},
                {"item_id", ids.at(i)},
                {"revision", 1},
                {"digest_sha256", digests.at(i)},
                {"total_bytes", sources.at(i).size()},
                {"offset", offset},
                {"next_offset", next},
                {"complete", next == sources.at(i).size()},
                {"data", QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                                            QByteArray::OmitTrailingEquals))}};
        };
        FakeDaemon daemon(env.directory.filePath("daemon.sock"), [&](const QString& type,
                                                                     const QJsonObject& envelope) {
            if (type == "session.list")
                return QJsonObject{{"transcript_window_version", 1},
                                   {"transcript_large_event_version", 1},
                                   {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                       {"worktree_path", path},
                                                                       {"session_generation", 1},
                                                                       {"state", "ready"},
                                                                       {"last_event_cursor", 2}}}}};
            if (type == "session.transcript_window")
                return QJsonObject{{"transcript_window_version", 1},
                                   {"session_id", id},
                                   {"session_generation", 1},
                                   {"events", events},
                                   {"before_cursor", 1},
                                   {"has_more_before", false},
                                   {"snapshot_cursor", 2},
                                   {"projection_revision", 1},
                                   {"current_turn_sequence", 2},
                                   {"state", "ready"}};
            if (type == "session.transcript_large_event") {
                const auto params = envelope.value("payload").toObject();
                if (params.value("transfer_id") == ids.first() &&
                    params.value("offset").toInteger() == 0)
                    return QJsonObject{{"__defer_test_response", true}};
                return fragmentFor(envelope);
            }
            return QJsonObject{};
        });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        // The second item's request must be issued while the first is stalled.
        QTRY_VERIFY_WITH_TIMEOUT(std::count(daemon.received.cbegin(), daemon.received.cend(),
                                            QStringLiteral("session.transcript_large_event")) >= 2,
                                 3000);
        QJsonObject firstRequest;
        for (const auto& envelope : daemon.envelopes)
            if (envelope.value("message_type") == "session.transcript_large_event" &&
                envelope.value("payload").toObject().value("transfer_id") == ids.first()) {
                firstRequest = envelope;
                break;
            }
        QVERIFY(!firstRequest.isEmpty());
        auto firstFragment = fragmentFor(firstRequest);
        firstFragment.insert("ok", true);
        daemon.push({{"message_type", "session.transcript_large_event.result"},
                     {"correlation_id", firstRequest.value("message_id")},
                     {"payload", firstFragment}});
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 2, 5000);
        for (int i = 0; i < ids.size(); ++i)
            QCOMPARE(desktop.messages(path).at(i).toMap().value("body").toString().size(),
                     150 * 1024);
    }
};

QTEST_GUILESS_MAIN(DesktopTranscriptLargeEventFetchTest)
#include "DesktopTranscriptLargeEventFetchTest.moc"
