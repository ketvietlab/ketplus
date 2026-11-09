#include "TranscriptWindowTestSupport.h"

class DesktopTranscriptCatchupTest final : public QObject {
    Q_OBJECT
  private slots:
    void cleanup() { transcript_paging::setPageCounts({}); }

    // A card that waited while another client decided it becomes that decision when history
    // is read again, never a second row beside it.
    void restoredReceiptSettlesAWaitingApprovalCard() {
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "a");
        const QJsonObject request{{"approval_id", "a1"},
                                  {"kind", "approval"},
                                  {"request_type", "command"},
                                  {"reason", "Codex wants network access."},
                                  {"command", "curl https://example.test"},
                                  {"revision", 1}};
        bool decided = false;
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                if (type == "session.list") {
                    QJsonArray sessions;
                    for (const auto* id : {"a", "b"})
                        sessions.append(QJsonObject{{"session_id", id},
                                                    {"worktree_path", path},
                                                    {"session_generation", 1},
                                                    {"state", "running"},
                                                    {"last_event_cursor", decided ? 4 : 2}});
                    return {{"transcript_window_version", 1}, {"sessions", sessions}};
                }
                if (type != "session.transcript_window")
                    return {};
                const auto id = envelope.value("session_id").toString();
                QJsonArray events{windowEvent(id, "user.message", {{"text", "prompt"}}, 1, 1)};
                QJsonArray pending;
                if (id == "a") {
                    events.append(windowEvent(id, "approval.requested", request, 2, 1));
                    if (decided)
                        events.append(
                            windowEvent(id, "approval.resolved",
                                        {{"approval_id", "a1"},
                                         {"answers", QJsonObject{{"decision", "accept"}}}},
                                        3, 1));
                    else
                        pending.append(request);
                }
                if (id == "a" && decided)
                    events.append(windowEvent(
                        id, "item.completed",
                        {{"item_id", "thought"},
                         {"item", QJsonObject{{"id", "thought"}, {"type", "reasoning"}}}},
                        4, 1));
                const qint64 head = id == "a" ? (decided ? 4 : 2) : 1;
                return {{"transcript_window_version", 1},
                        {"session_id", id},
                        {"session_generation", 1},
                        {"events", events},
                        {"before_cursor", 1},
                        {"after_cursor", head},
                        {"snapshot_cursor", head},
                        {"projection_revision", decided ? 2 : 1},
                        {"current_turn_sequence", 1},
                        {"active_turn_id", "turn-1"},
                        {"state", "running"},
                        {"has_more_before", false},
                        {"has_more_after", false},
                        {"pending_approvals", pending}};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        const auto approvals = [&] {
            QVariantList rows;
            for (const auto& row : desktop.messages(path))
                if (row.toMap().value("subtype") == "approval")
                    rows.append(row);
            return rows;
        };
        QTRY_COMPARE(approvals().size(), 1);
        QTRY_VERIFY(approvals().first().toMap().value("running").toBool());
        QTRY_COMPARE(desktop.status(path), QStringLiteral("Waiting for your answer…"));

        // Decided on the phone while this client missed the event; history is read again.
        desktop.selectConversation(path, "b");
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        decided = true;
        daemon.push(liveEvent("a", "item.completed",
                              {{"item_id", "thought"},
                               {"item", QJsonObject{{"id", "thought"}, {"type", "reasoning"}}}},
                              4, 1));
        QTest::qWait(30);
        desktop.selectConversation(path, "a");
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QTRY_COMPARE(approvals().size(), 1);
        QTRY_VERIFY(!approvals().first().toMap().value("running").toBool());
        const auto note = approvals().first().toMap();
        QCOMPARE(note.value("status").toString(), QStringLiteral("approved"));
        QVERIFY(!note.value("running").toBool());
        QVERIFY(!note.value("presentationHidden").toBool());
        QCOMPARE(note.value("command").toString(), QStringLiteral("curl https://example.test"));
        QVERIFY(desktop.status(path) != QStringLiteral("Waiting for your answer…"));
    }

    void compactionStatusFollowsRetainedWindow_data() {
        QTest::addColumn<bool>("restoredStart");
        QTest::addColumn<bool>("activityDuringRead");
        QTest::newRow("live-start") << false << false;
        QTest::newRow("restored-start") << true << false;
        QTest::newRow("live-activity-during-refresh") << false << true;
    }

    void compactionStatusFollowsRetainedWindow() {
        QFETCH(bool, restoredStart);
        QFETCH(bool, activityDuringRead);
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "a");
        const QJsonObject compact{{"id", "compact"}, {"type", "contextCompaction"}};
        const QJsonObject compactPayload{{"item_id", "compact"}, {"item", compact}};
        qint64 head = restoredStart ? 2 : 1;
        bool completed = false;
        CodexAppServer* client = nullptr;
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                if (type == "session.list") {
                    QJsonArray sessions;
                    for (const auto* id : {"a", "b"})
                        sessions.append(QJsonObject{{"session_id", id},
                                                    {"worktree_path", path},
                                                    {"session_generation", 1},
                                                    {"state", "running"},
                                                    {"last_event_cursor", head}});
                    return {{"transcript_window_version", 1}, {"sessions", sessions}};
                }
                if (type != "session.transcript_window")
                    return {};
                const auto id = envelope.value("session_id").toString();
                if (id == "a" && completed && std::exchange(activityDuringRead, false))
                    QTimer::singleShot(0, [&] {
                        emit client->notificationReceived(
                            "item/completed",
                            {{"threadId", "a"},
                             {"item", QJsonObject{{"id", "live-reason"}, {"type", "reasoning"}}}});
                    });
                QJsonArray events{windowEvent(id, "user.message", {{"text", "prompt"}}, 1, 1)};
                if (id == "a" && head >= 2)
                    events.append(windowEvent(id, completed ? "item.completed" : "item.started",
                                              compactPayload, head, 1));
                return {{"transcript_window_version", 1},
                        {"session_id", id},
                        {"session_generation", 1},
                        {"events", events},
                        {"before_cursor", 1},
                        {"after_cursor", head},
                        {"snapshot_cursor", head},
                        {"projection_revision", completed ? 2 : 1},
                        {"current_turn_sequence", 1},
                        {"active_turn_id", "turn-1"},
                        {"state", "running"},
                        {"has_more_before", false},
                        {"has_more_after", false},
                        {"pending_approvals", QJsonArray{}}};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        client = &server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE(desktop.messages(path).size(), restoredStart ? 2 : 1);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        if (!restoredStart) {
            head = 2;
            daemon.push(liveEvent("a", "item.started", compactPayload, head, 1));
        }
        QTRY_COMPARE(desktop.status(path), QStringLiteral("Compacting context…"));

        desktop.selectConversation(path, "b");
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        completed = true;
        head = 3;
        daemon.push(liveEvent("a", "item.completed", compactPayload, head, 1));
        QTest::qWait(30);
        desktop.selectConversation(path, "a");
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QTRY_COMPARE(desktop.messages(path).last().toMap().value("body").toString(),
                     QStringLiteral("Context compacted"));
        QCOMPARE(desktop.messages(path).size(), 2);
        QCOMPARE(desktop.status(path), QStringLiteral("Working…"));

        // Subsequent provider activity must not resurrect the pre-refresh status.
        const QJsonObject reasoning{{"id", "reason"}, {"type", "reasoning"}};
        emit server.notificationReceived("item/completed",
                                         {{"threadId", "a"}, {"item", reasoning}});
        QCOMPARE(desktop.status(path), QStringLiteral("Working…"));
        QVERIFY(desktop.isRunning(path));
        // The reasoning reads as a "Thought" step; the compaction stays one row.
        QCOMPARE(desktop.messages(path).size(), 3);
        QCOMPARE(desktop.messages(path).last().toMap().value("title").toString(),
                 QStringLiteral("Thought"));
        emit server.notificationReceived("item/completed", {{"threadId", "a"}, {"item", compact}});
        QCOMPARE(desktop.messages(path).size(), 3);
        QCOMPARE(desktop.status(path), QStringLiteral("Working…"));

        const auto compactEvent = [&](const QString& method, const QString& id) {
            emit server.notificationReceived(
                method, {{"threadId", "a"},
                         {"item", QJsonObject{{"id", id}, {"type", "contextCompaction"}}}});
        };
        compactEvent("item/started", "next-1");
        compactEvent("item/started", "next-2");
        compactEvent("item/completed", "next-1");
        QCOMPARE(desktop.status(path), QStringLiteral("Compacting context…"));
        compactEvent("item/completed", "next-2");
        QCOMPARE(desktop.status(path), QStringLiteral("Working…"));
        compactEvent("item/started", "next-2"); // A duplicate start cannot reopen a terminal row.
        QCOMPARE(desktop.status(path), QStringLiteral("Working…"));
        server.stop();
    }

    void returningKeepsReaderAndCatchesUp_data() {
        QTest::addColumn<QString>("scenario");
        QTest::newRow("live") << QStringLiteral("live");
    }

    void returningKeepsReaderAndCatchesUp() {
        QFETCH(QString, scenario);
        transcript_paging::setPageCounts({1, 1, false, false});
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "a");
        const auto item = [](const QString& id, const QString& text) {
            return QJsonObject{
                {"id", id}, {"type", "agentMessage"}, {"phase", "final_answer"}, {"text", text}};
        };
        QList<QJsonObject> journal{
            windowEvent("a", "user.message", {{"text", "reader"}, {"item_id", "old"}}, 1, 1),
            windowEvent("a", "user.message", {{"text", "prompt"}, {"item_id", "prompt"}}, 2, 2),
            windowEvent("a", "item.started", {{"item", item("answer", "A")}}, 3, 2),
            windowEvent("a", "assistant.delta", {{"item_id", "answer"}, {"delta", "B"}}, 4, 2),
            windowEvent("a", "assistant.delta", {{"item_id", "answer"}, {"delta", "C"}}, 5, 2),
            windowEvent("a", "item.completed", {{"item", item("answer", "ABC")}}, 6, 2),
            windowEvent("a", "item.started", {{"item", item("next", "Next ")}}, 7, 2),
            windowEvent("a", "assistant.delta", {{"item_id", "next"}, {"delta", "live"}}, 8, 2)};
        if (scenario == "overflow")
            for (int cursor = 9; cursor <= 136; ++cursor)
                journal.append(windowEvent("a", "assistant.delta",
                                           {{"item_id", "next"}, {"delta", "!"}}, cursor, 2));
        int initialReads = 0;
        int headReads = 0;
        qint64 revision = 1;
        QList<qint64> forwardCursors;
        qint64 head = 3;
        bool sendLive = true;
        bool fail = scenario == "retry";
        FakeDaemon* transport = nullptr;
        const auto push = [&](const int cursor) {
            const auto event = journal.at(cursor - 1);
            transport->push(liveEvent("a", event.value("event_type").toString(),
                                      event.value("payload").toObject(), cursor,
                                      event.value("turn_sequence").toInteger()));
        };
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                if (type == "session.list") {
                    QJsonArray sessions;
                    for (const auto* id : {"a", "b"})
                        sessions.append(QJsonObject{{"session_id", id},
                                                    {"worktree_path", path},
                                                    {"session_generation", 1},
                                                    {"state", "running"},
                                                    {"last_event_cursor", 3}});
                    return {{"transcript_window_version", 1},
                            {"transcript_window_after_cursor", scenario != "legacy"},
                            {"sessions", sessions}};
                }
                if (type != "session.transcript_window")
                    return {};
                const auto id = envelope.value("session_id").toString();
                const auto params = envelope.value("payload").toObject();
                const auto after = params.value("after_cursor").toInteger();
                const auto before = params.value("before_cursor").toInteger();
                QJsonArray events;
                qint64 snapshot = head;
                if (id == "b") {
                    snapshot = 3;
                    events.append(windowEvent(id, "user.message", {{"text", "other"}}, 3, 1));
                } else if (after > 0) {
                    forwardCursors.append(after);
                    if (scenario == "unstable" || fail ||
                        (scenario == "compaction" && forwardCursors.size() == 1)) {
                        ++revision;
                        return {{"ok", false},
                                {"error_code", fail ? "unavailable" : "snapshot_changed"},
                                {"error_message", "Scripted history failure"}};
                    }
                    snapshot = params.value("snapshot_cursor").toInteger();
                    if (params.value("projection_revision").toInteger() != revision)
                        return {{"ok", false},
                                {"error_code", "snapshot_changed"},
                                {"error_message", "Stale projection revision"}};
                    if (after < snapshot) {
                        // A completion compacts away the delta at cursor 5.
                        events.append(
                            journal.at(scenario == "compaction" && after == 4 ? 5 : after));
                    }
                    if (sendLive) {
                        sendLive = false;
                        head = journal.size();
                        for (int cursor = 8; cursor <= head; ++cursor)
                            push(cursor); // Arrives before projection/publication has finished.
                    }
                } else if (before > 0) {
                    events.append(journal.first());
                } else if (params.value("turn_limit").toInt() == 1 && initialReads > 0) {
                    ++headReads;
                    events.append(journal.at(6));
                } else if (++initialReads == 1) {
                    events = {journal.at(1), journal.at(2)};
                } else {
                    events.append(journal.at(6)); // Fresh newest window loses the reader.
                    if (head == 8)
                        events.append(journal.at(7));
                    if (sendLive)
                        QTimer::singleShot(0, [&] {
                            head = 8;
                            sendLive = false;
                            push(8); // Arrives while the new window awaits publication.
                        });
                }
                const auto last =
                    events.isEmpty() ? after : events.last().toObject().value("cursor").toInteger();
                return {
                    {"transcript_window_version", 1},
                    {"session_id", id},
                    {"session_generation", 1},
                    {"events", events},
                    {"before_cursor",
                     events.isEmpty() ? 0 : events.first().toObject().value("cursor").toInteger()},
                    {"after_cursor", last},
                    {"has_more_after", after > 0 && last < snapshot},
                    {"has_more_before", id == "a" && before == 0},
                    {"snapshot_cursor", snapshot},
                    {"projection_revision", revision},
                    {"current_turn_sequence", 2},
                    {"active_turn_id", "turn-2"},
                    {"state", "running"},
                    {"pending_approvals", QJsonArray{}},
                    {"partial_items",
                     QJsonArray{QJsonObject{{"turn_sequence", 2}, {"item_id", "answer"}},
                                QJsonObject{{"turn_sequence", 2}, {"item_id", "next"}}}}};
            });
        transport = &daemon;
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QTRY_COMPARE(desktop.messages(path).size(), 2);
        desktop.loadOlderMessages(path);
        QTRY_COMPARE(desktop.messages(path).size(), 3);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        const auto reader = desktop.messages(path).first().toMap().value("messageKey").toString();
        desktop.setTranscriptReadingKey(reader);
        head = 4;
        push(4);
        QTRY_COMPARE(desktop.messages(path).last().toMap().value("body").toString(), QString("AB"));
        desktop.selectConversation(path, "b");
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QSignalSpy invalidated(&server, &CodexAppServer::daemonTranscriptInvalidated);
        head = 7;
        ++revision; // Ordinary completions can advance the projection while inactive.
        for (int cursor = 5; cursor <= 7; ++cursor)
            push(cursor);
        QTRY_COMPARE(invalidated.size(), 1);
        QSignalSpy resets(&desktop, &AgentSessionManager::messagesReset);
        desktop.setTranscriptInputActive(true);
        desktop.selectConversation(path, "a");
        QTRY_VERIFY(!sendLive);
        desktop.setTranscriptInputActive(false);
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.historyState(path).value("loading").toBool(), 30000);
        QTRY_COMPARE(desktop.messages(path).last().toMap().value("body").toString(),
                     QStringLiteral("Next live"));
        QCOMPARE(initialReads, 2);
        QCOMPARE(headReads, 0);
        QVERIFY(forwardCursors.isEmpty());
        QCOMPARE(desktop.messages(path).size(), 1);
        QVERIFY(desktop.messages(path).first().toMap().value("messageKey").toString() != reader);
        QVERIFY(!desktop.historyState(path).value("hasNewer").toBool());
        QVERIFY(desktop.historyState(path).value("error").toString().isEmpty());
        QCOMPARE(resets.size(), 2); // Cached view, then the refreshed latest page.
        desktop.loadOlderMessages(path);
        QTRY_COMPARE(desktop.messages(path).size(), 2);
        server.stop();
    }

    void catchupKeepsReaderInWindowLargerThanIdleBudget() {
        transcript_paging::setPageCounts({1, 1, false, false});
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "a");
        qint64 head = 400;
        const auto row = [](const QString& id, const qint64 cursor) {
            return windowEvent(id, "item.completed",
                               {{"item", QJsonObject{{"id", QString::number(cursor)},
                                                     {"type", "agentMessage"},
                                                     {"phase", "final_answer"},
                                                     {"text", QString("Row %1").arg(cursor)}}}},
                               cursor, cursor <= 400 ? 1 : 2);
        };
        FakeDaemon daemon(env.directory.filePath("daemon.sock"),
                          [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                              if (type == "session.list") {
                                  QJsonArray sessions;
                                  for (const auto* id : {"a", "b"})
                                      sessions.append(QJsonObject{{"session_id", id},
                                                                  {"worktree_path", path},
                                                                  {"session_generation", 1},
                                                                  {"state", "running"},
                                                                  {"last_event_cursor", head}});
                                  return {{"transcript_window_version", 1},
                                          {"transcript_window_after_cursor", true},
                                          {"sessions", sessions}};
                              }
                              if (type != "session.transcript_window")
                                  return {};
                              const auto id = envelope.value("session_id").toString();
                              const auto params = envelope.value("payload").toObject();
                              const auto before = params.value("before_cursor").toInteger();
                              const auto after = params.value("after_cursor").toInteger();
                              const qint64 last = before > 0 ? before - 1 : head;
                              const qint64 first =
                                  after > 0 ? after + 1 : std::max<qint64>(1, last - 127);
                              QJsonArray events;
                              for (auto cursor = first; cursor <= last; ++cursor)
                                  events.append(row(id, cursor));
                              return {{"transcript_window_version", 1},
                                      {"session_id", id},
                                      {"session_generation", 1},
                                      {"events", events},
                                      {"before_cursor", first},
                                      {"after_cursor", last},
                                      {"has_more_before", first > 1},
                                      {"has_more_after", false},
                                      {"snapshot_cursor", head},
                                      {"projection_revision", 1},
                                      {"current_turn_sequence", head <= 400 ? 1 : 2},
                                      {"active_turn_id", "turn-2"},
                                      {"state", "running"},
                                      {"pending_approvals", QJsonArray{}}};
                          });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE(desktop.messages(path).size(), 128);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        for (int page = 0; page < 3; ++page) {
            desktop.loadOlderMessages(path);
            QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        }
        QCOMPARE(desktop.messages(path).size(), 400);
        const auto reader = desktop.messages(path).first().toMap().value("messageKey").toString();
        desktop.setTranscriptReadingKey(reader);
        desktop.selectConversation(path, "b");
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QSignalSpy invalidated(&server, &CodexAppServer::daemonTranscriptInvalidated);
        ++head;
        daemon.push(
            liveEvent("a", "item.completed", row("a", head).value("payload").toObject(), head, 2));
        QTRY_COMPARE(invalidated.size(), 1);
        desktop.selectConversation(path, "a");
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QCOMPARE(desktop.messages(path).size(), 128);
        QVERIFY(desktop.messages(path).first().toMap().value("messageKey").toString() != reader);
        QCOMPARE(desktop.messages(path).last().toMap().value("body").toString(),
                 QString("Row 401"));
        QVERIFY(desktop.historyState(path).value("hasOlder").toBool());
        desktop.loadOlderMessages(path);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QVERIFY(desktop.messages(path).size() > 128);
        QVERIFY(!desktop.historyState(path).value("hasNewer").toBool());
        QVERIFY(desktop.historyState(path).value("error").toString().isEmpty());
    }

    // A turn end delivered late reads as the journal timed it, not as the desktop received it.
    void lateTurnEndUsesTheJournalTimes() {
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "a");
        FakeDaemon daemon(env.directory.filePath("daemon.sock"), [&](const QString& type,
                                                                     const QJsonObject&) {
            if (type == "session.list")
                return QJsonObject{{"transcript_window_version", 1},
                                   {"sessions", QJsonArray{QJsonObject{{"session_id", "a"},
                                                                       {"worktree_path", path},
                                                                       {"session_generation", 1},
                                                                       {"state", "running"},
                                                                       {"last_event_cursor", 1}}}}};
            if (type != "session.transcript_window")
                return QJsonObject{};
            return QJsonObject{{"transcript_window_version", 1},
                               {"session_id", "a"},
                               {"session_generation", 1},
                               {"events", QJsonArray{windowEvent("a", "user.message",
                                                                 {{"text", "earlier"}}, 1, 1)}},
                               {"before_cursor", 1},
                               {"has_more_before", false},
                               {"snapshot_cursor", 1},
                               {"projection_revision", 1},
                               {"current_turn_sequence", 1},
                               {"state", "running"}};
        });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 1, 5000);
        const QDateTime start = QDateTime::currentDateTimeUtc().addSecs(-3600);
        const auto sent = [&](QJsonObject event, int seconds) {
            event.insert("sent_at", start.addSecs(seconds).toString(Qt::ISODateWithMs));
            return event;
        };
        daemon.push(sent(liveEvent("a", "user.message", {{"text", "Fix the spinner"}}, 2, 2), 0));
        daemon.push(sent(
            liveEvent("a", "usage.context",
                      {{"tokenUsage", QJsonObject{{"last", QJsonObject{{"totalTokens", 141200}}},
                                                  {"modelContextWindow", 200000}}}},
                      3, 2),
            200));
        daemon.push(sent(liveEvent("a", "turn.completed", {{"status", "completed"}}, 4, 2), 380));
        const auto turnEnd = [&] {
            QString last;
            for (const auto& row : desktop.messages(path)) {
                const auto entry = row.toMap();
                if (entry.value("subtype") == "turnEnd")
                    last = entry.value("body").toString();
            }
            return last;
        };
        QTRY_COMPARE_WITH_TIMEOUT(turnEnd(), QString::fromUtf8("6m 20s · 141k context"), 5000);
        // A live end the user stopped keeps saying so when it is worded with the session cost.
        daemon.push(
            sent(liveEvent("a", "user.message", {{"text", "Then the sidebar"}}, 5, 3), 400));
        daemon.push(sent(
            liveEvent("a", "turn.completed", {{"status", "interrupted"}, {"reason", "user"}}, 6, 3),
            468));
        QTRY_COMPARE_WITH_TIMEOUT(turnEnd(), QString::fromUtf8("1m 08s · stopped by you"), 5000);
    }

    // A sent message's live states update one note, as a reloaded window shows it.
    void liveAgentMessageStatesUpdateOneNote() {
        Environment env;
        QVERIFY(writeToken(env.directory));
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "a");
        FakeDaemon daemon(env.directory.filePath("daemon.sock"), [&](const QString& type,
                                                                     const QJsonObject&) {
            if (type == "session.list")
                return QJsonObject{{"transcript_window_version", 1},
                                   {"sessions", QJsonArray{QJsonObject{{"session_id", "a"},
                                                                       {"worktree_path", path},
                                                                       {"session_generation", 1},
                                                                       {"state", "running"},
                                                                       {"last_event_cursor", 1}}}}};
            if (type != "session.transcript_window")
                return QJsonObject{};
            return QJsonObject{{"transcript_window_version", 1},
                               {"session_id", "a"},
                               {"session_generation", 1},
                               {"events", QJsonArray{windowEvent("a", "user.message",
                                                                 {{"text", "earlier"}}, 1, 1)}},
                               {"before_cursor", 1},
                               {"has_more_before", false},
                               {"snapshot_cursor", 1},
                               {"projection_revision", 1},
                               {"current_turn_sequence", 1},
                               {"state", "running"}};
        });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 1, 5000);
        const QJsonObject sent{
            {"message_id", "q1"},    {"kind", "request"},       {"state", "sent"},
            {"delivery", "queued"},  {"expect_reply", true},    {"direction", "outgoing"},
            {"peer_number", "8941"}, {"peer_title", "DS Icon"}, {"text", "Is it fine?"}};
        daemon.push(liveEvent("a", "agent_message.state", sent, 2, 1));
        const auto notes = [&] {
            QStringList found;
            for (const auto& row : desktop.messages(path))
                if (row.toMap().value("subtype") == "agentNote")
                    found.append(row.toMap().value("meta").toString());
            return found;
        };
        QTRY_COMPARE_WITH_TIMEOUT(
            notes(), QStringList{"DS Icon · Sent · delivered when its turn ends · reply requested"},
            5000);
        daemon.push(liveEvent("a", "agent_message.state",
                              {{"message_id", "q1"},
                               {"kind", "request"},
                               {"state", "failed"},
                               {"reason", "no_reply"},
                               {"direction", "outgoing"},
                               {"peer_number", "8941"}},
                              3, 1));
        QTRY_COMPARE_WITH_TIMEOUT(notes(), QStringList{"DS Icon · Delivered · no reply"}, 5000);
    }
};

QTEST_GUILESS_MAIN(DesktopTranscriptCatchupTest)
#include "DesktopTranscriptCatchupTest.moc"
