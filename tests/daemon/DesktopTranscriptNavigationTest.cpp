#include "TranscriptWindowTestSupport.h"

namespace {
// Every reply still has older history. Opening this fixture must stop fetching
// on its own rather than walking to the end of the journal.
QJsonObject reply(const QString& type, const QJsonObject& envelope, const QString& path) {
    if (type == "session.list") {
        QJsonArray sessions;
        for (const QString& id : {QStringLiteral("a"), QStringLiteral("b")})
            sessions.append(QJsonObject{{"session_id", id},
                                        {"worktree_path", path},
                                        {"session_generation", 1},
                                        {"state", "ready"},
                                        {"last_event_cursor", 10000}});
        return {{"transcript_window_version", 1}, {"sessions", sessions}};
    }
    if (type != "session.transcript_window")
        return {};
    const QString id = envelope.value("session_id").toString();
    const auto before = envelope.value("payload").toObject().value("before_cursor").toInteger();
    const qint64 cursor = before > 0 ? before - 10 : 10000;
    return {{"transcript_window_version", 1},
            {"session_id", id},
            {"session_generation", 1},
            {"events",
             QJsonArray{windowEvent(id, "user.message", {{"text", id}}, cursor, cursor / 10)}},
            {"before_cursor", cursor},
            {"has_more_before", cursor > 10},
            {"snapshot_cursor", 10000},
            {"projection_revision", 1},
            {"current_turn_sequence", 1000},
            {"state", "ready"},
            {"pending_approvals", QJsonArray{}}};
}
} // namespace

class DesktopTranscriptNavigationTest final : public QObject {
    Q_OBJECT
  private slots:
    void cleanup() { transcript_paging::setPageCounts({}); }

    void restoredAssistantReceivesDeltasWithoutAnotherStart_data() {
        QTest::addColumn<QString>("phase");
        QTest::addColumn<QString>("prefix");
        QTest::newRow("empty-answer") << QString("final_answer") << QString();
        QTest::newRow("existing-answer") << QString("final_answer") << QString("Earlier ");
        QTest::newRow("commentary") << QString("commentary") << QString("Progress: ");
    }

    void restoredAssistantReceivesDeltasWithoutAnotherStart() {
        QFETCH(QString, phase);
        QFETCH(QString, prefix);
        transcript_paging::setPageCounts({1, 1, false, false});
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = "running";
        rememberConversation(path, id);
        const QJsonObject item{
            {"id", "answer"}, {"type", "agentMessage"}, {"phase", phase}, {"text", prefix}};
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject&) -> QJsonObject {
                if (type == "session.list")
                    return {{"transcript_window_version", 1},
                            {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "running"},
                                                                {"last_event_cursor", 2}}}}};
                if (type == "session.transcript_window")
                    return {{"transcript_window_version", 1},
                            {"session_id", id},
                            {"session_generation", 1},
                            {"events",
                             QJsonArray{windowEvent(id, "user.message", {{"text", "prompt"}}, 1, 1),
                                        windowEvent(id, "item.started", {{"item", item}}, 2, 1)}},
                            {"before_cursor", 1},
                            {"has_more_before", false},
                            {"snapshot_cursor", 2},
                            {"projection_revision", 1},
                            {"current_turn_sequence", 1},
                            {"active_turn_id", "turn-1"},
                            {"state", "running"},
                            {"pending_approvals", QJsonArray{}}};
                return {};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE(desktop.messages(path).size(), 2);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QCOMPARE(desktop.messages(path).last().toMap().value("body"), prefix);
        QVERIFY(desktop.isRunning(path));
        daemon.push(liveEvent(id, "assistant.delta",
                              {{"item_id", "answer"}, {"delta", "visible now"}}, 3, 1));
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).last().toMap().value("body"),
                                  prefix + "visible now", 1000);
        // An unknown/unclassified item must still wait for a public message phase.
        daemon.push(liveEvent(id, "assistant.delta",
                              {{"item_id", "unclassified"}, {"delta", "hidden"}}, 4, 1));
        QTest::qWait(50);
        QCOMPARE(desktop.messages(path).size(), 2);
        auto completed = item;
        completed["text"] = prefix + "visible now. Complete.";
        daemon.push(liveEvent(id, "item.completed", {{"item", completed}}, 5, 1));
        QTRY_COMPARE(desktop.messages(path).last().toMap().value("body"),
                     completed.value("text").toString());
    }

    void inactivePermissionSurvivesSelectionAndStopDoesNotAnswerIt_data() {
        QTest::addColumn<bool>("accept");
        QTest::newRow("stop") << false;
        QTest::newRow("confirm") << true;
    }
    void inactivePermissionSurvivesSelectionAndStopDoesNotAnswerIt() {
        QFETCH(bool, accept);
        transcript_paging::setPageCounts({1, 1, false, false});
        Environment env;
        QVERIFY(writeToken(env.directory));
        const auto path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "a");
        const QJsonObject permission{{"approval_id", "permission-1"},
                                     {"kind", "approval"},
                                     {"reason", "Allow command?"},
                                     {"command", "echo test"}};
        bool pending = false;
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                if (type == "session.list") {
                    QJsonArray sessions;
                    for (const auto id : {"a", "b"})
                        sessions.append(QJsonObject{{"session_id", id},
                                                    {"worktree_path", path},
                                                    {"session_generation", 1},
                                                    {"state", "running"},
                                                    {"last_event_cursor", 1}});
                    return {{"transcript_window_version", 1}, {"sessions", sessions}};
                }
                if (type == "session.transcript_window") {
                    const auto id = envelope.value("session_id").toString();
                    return {{"transcript_window_version", 1},
                            {"session_id", id},
                            {"session_generation", 1},
                            {"events", QJsonArray{windowEvent(id, "user.message",
                                                              {{"text", "Prompt"}}, 1, 1)}},
                            {"before_cursor", 1},
                            {"has_more_before", false},
                            {"snapshot_cursor", pending ? 2 : 1},
                            {"projection_revision", 1},
                            {"current_turn_sequence", 1},
                            {"active_turn_id", "turn-1"},
                            {"state", "running"},
                            {"pending_approvals",
                             pending && id == "a" ? QJsonArray{permission} : QJsonArray{}}};
                }
                return {};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_VERIFY(!desktop.messages(path).isEmpty());
        desktop.selectConversation(path, "b");
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        pending = true;
        daemon.push(liveEvent("a", "approval.requested", permission, 2, 1));
        QTest::qWait(50);
        QVERIFY(!daemon.received.contains("approval.resolve"));
        QVERIFY(!daemon.received.contains("question.answer"));
        desktop.selectConversation(path, "a");
        const auto hasCard = [&] {
            for (const auto& row : desktop.messages(path))
                if (row.toMap().value("subtype") == "approval" &&
                    row.toMap().value("running").toBool())
                    return true;
            return false;
        };
        QTRY_VERIFY(hasCard());
        QVERIFY(desktop.isRunning(path));
        if (accept) {
            for (const auto& value : desktop.messages(path)) {
                const auto row = value.toMap();
                if (row.value("subtype") != "approval")
                    continue;
                const auto question = row.value("questions").toList().first().toMap();
                const auto optionId =
                    question.value("options").toList().first().toMap().value("id").toString();
                desktop.answerQuestion(
                    path, row.value("messageKey").toString(),
                    {{"decision", QVariantMap{{"option_ids", QStringList{optionId}}}}});
                break;
            }
            QTRY_VERIFY(daemon.received.contains("approval.resolve"));
            for (const auto& envelope : daemon.envelopes)
                if (envelope.value("message_type") == "approval.resolve")
                    QCOMPARE(envelope.value("payload")
                                 .toObject()
                                 .value("resolution")
                                 .toObject()
                                 .value("decision"),
                             "accept");
            return;
        }
        desktop.cancel(path);
        QTRY_VERIFY(daemon.received.contains("session.interrupt"));
        QVERIFY(!daemon.received.contains("approval.resolve"));
        QVERIFY(!daemon.received.contains("question.answer"));
    }

    void stalledContinuationTimesOutAndCanRetry() {
        transcript_paging::setPageCounts({1, 1, false, false});
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "a");
        bool stall = false;
        FakeDaemon daemon(env.directory.filePath("daemon.sock"),
                          [&](const QString& type, const QJsonObject& envelope) {
                              if (stall && type == "session.transcript_window")
                                  return QJsonObject{{"__defer_test_response", true}};
                              return reply(type, envelope, path);
                          });
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_VERIFY(!desktop.messages(path).isEmpty());
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        stall = true;
        desktop.loadOlderMessages(path);
        QVERIFY(desktop.historyState(path).value("loading").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.historyState(path).value("loading").toBool(), 20000);
        QVERIFY(desktop.historyState(path).value("error").toString().contains("timed out"));
        QCOMPARE(desktop.messages(path).size(), 1);
        stall = false;
        desktop.loadOlderMessages(path);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QCOMPARE(desktop.messages(path).size(), 2);
        QVERIFY(desktop.historyState(path).value("error").toString().isEmpty());
    }

    void openingReadsOnlyOnePageAndItsNeighbour() {
        transcript_paging::setPageCounts({});
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "a");
        QList<QJsonObject> requests;
        FakeDaemon daemon(env.directory.filePath("daemon.sock"),
                          [&](const QString& type, const QJsonObject& envelope) {
                              if (type == "session.transcript_window")
                                  requests.append(envelope.value("payload").toObject());
                              return reply(type, envelope, path);
                          });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_VERIFY(!desktop.messages(path).isEmpty());
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QTRY_COMPARE(desktop.messages(path).size(), 2);
        QCOMPARE(desktop.historyState(path).value("lastBatch").toMap().value("pages"), 1);
        QTRY_COMPARE(requests.size(), 3);
        QTest::qWait(200);
        QCOMPARE(requests.size(), 3);
        QCOMPARE(requests.first().value("turn_limit").toInt(), 1);
        QCOMPARE(requests.at(1).value("turn_limit").toInt(), 2);
        QCOMPARE(requests.last().value("before_cursor").toInteger(), 9990);

        desktop.loadOlderMessages(path);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QCOMPARE(desktop.messages(path).size(), 3);
        QCOMPARE(desktop.historyState(path).value("lastBatch").toMap().value("cachedPages"), 1);
        QTRY_COMPARE(requests.size(), 4);
        QTest::qWait(200);
        QCOMPARE(requests.size(), 4);
    }

    void switchingCancelsHistoryWaitingForIdle() {
        transcript_paging::setPageCounts({1, 1, false, false});
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "a");
        int readsA = 0;
        FakeDaemon daemon(env.directory.filePath("daemon.sock"), [&](const QString& type,
                                                                     const QJsonObject& envelope) {
            if (type == "session.transcript_window" && envelope.value("session_id") == "a")
                ++readsA;
            return reply(type, envelope, path);
        });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.setTranscriptInputActive(true);
        desktop.activate(path);
        QTRY_COMPARE(readsA, 1);
        QTRY_COMPARE(desktop.conversations(path).size(), 2);
        QTest::qWait(100); // A is waiting for idle publication.
        QVERIFY(desktop.historyState(path).value("loading").toBool());
        QVERIFY(desktop.messages(path).isEmpty());
        desktop.selectConversation(path, "b");
        desktop.setTranscriptInputActive(false);
        QTRY_VERIFY(!desktop.messages(path).isEmpty());
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QCOMPARE(desktop.messages(path).last().toMap().value("body"), "b");
        desktop.selectConversation(path, "a");
        QTRY_VERIFY(!desktop.messages(path).isEmpty());
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QCOMPARE(desktop.messages(path).last().toMap().value("body"), "a");
        QCOMPARE(readsA, 2); // A's abandoned batch never became a warm window.
    }

    // A notice raised by anything other than an older page used to be unreachable:
    // the retry control only ever asks for older rows, and that request returns
    // before clearing the notice once the whole history is already loaded.
    void retryClearsANoticeRaisedWhileNothingOlderIsLeftToPage() {
        transcript_paging::setPageCounts({1, 1, false, false});
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = "whole";
        rememberConversation(path, id);
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject&) -> QJsonObject {
                if (type == "session.list")
                    return {{"transcript_window_version", 1},
                            {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "ready"},
                                                                {"last_event_cursor", 2}}}}};
                if (type != "session.transcript_window")
                    return {};
                return {{"transcript_window_version", 1},
                        {"session_id", id},
                        {"session_generation", 1},
                        {"events",
                         QJsonArray{windowEvent(id, "user.message", {{"text", "only"}}, 1, 1)}},
                        {"before_cursor", 1},
                        {"has_more_before", false},
                        {"snapshot_cursor", 2},
                        {"projection_revision", 1},
                        {"current_turn_sequence", 1},
                        {"state", "ready"},
                        {"pending_approvals", QJsonArray{}}};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE(desktop.messages(path).size(), 1);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QVERIFY(!desktop.historyState(path).value("hasOlder").toBool());

        emit server.daemonReplayFailed(id, QStringLiteral("Replay timed out."), false);
        QTRY_COMPARE(desktop.historyState(path).value("error").toString(),
                     QStringLiteral("Replay timed out."));
        // A replay covers the window, so the retry stays on the older page.
        QVERIFY(!desktop.historyState(path).value("errorNewer").toBool());
        QVERIFY(!desktop.historyState(path).value("resyncRequired").toBool());

        desktop.loadOlderMessages(path);
        QTRY_VERIFY(desktop.historyState(path).value("error").toString().isEmpty());
        QCOMPARE(desktop.messages(path).size(), 1);
    }
};

QTEST_GUILESS_MAIN(DesktopTranscriptNavigationTest)
#include "DesktopTranscriptNavigationTest.moc"
