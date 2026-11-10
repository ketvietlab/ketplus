#include "TranscriptWindowTestSupport.h"
#include "agent/AgentTranscriptWindowBudget.h"

#include <QStandardPaths>

class DesktopTranscriptWindowTest final : public QObject {
    Q_OBJECT
    static QString latestAssistantBody(const QVariantList& rows) {
        for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
            const auto row = it->toMap();
            if (row.value("kind") == "assistant")
                return row.value("body").toString();
        }
        return {};
    }
  private slots:
    // DaemonService reads the machine's cloud credentials from the user's data directory, not
    // from the temporary state it is given. Without test paths, a run from a shell that set
    // KETPLUS_CONTROL_URL published these fixtures as live conversations of the real machine.
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    // These tests walk the window one daemon page at a time.
    void init() { transcript_paging::setPageCounts({1, 1, false, false}); }
    void cleanup() { transcript_paging::setPageCounts({}); }

    // One page opens a conversation and five more arrive per lazy load, each
    // batch reaching the model as one change.
    void pagesArriveInBatches() {
        transcript_paging::setPageCounts({});
        Environment env;
        QVERIFY(env.directory.isValid());
        constexpr int turns = 16;
        {
            LocalJournal journal(env.directory.filePath("journal.sqlite3"));
            QVERIFY(journal.upsertSession(
                {"batched", env.directory.path(), "codex", {}, {}, "ready", 1, 0, 0, "batched"}));
            for (int turn = 0; turn < turns; ++turn) {
                QVERIFY(journal.appendEvent("batched", 1, "user.message",
                                            {{"text", QString("prompt-%1").arg(turn)}}) > 0);
                QVERIFY(journal.appendEvent(
                            "batched", 1, "item.completed",
                            {{"item", QJsonObject{{"id", QString::number(turn)},
                                                  {"type", "agentMessage"},
                                                  {"text", QString("answer-%1").arg(turn)}}}}) > 0);
                QVERIFY(journal.appendEvent("batched", 1, "turn.completed",
                                            {{"status", "completed"}}) > 0);
            }
        }
        const auto canonicalPath = QFileInfo(env.directory.path()).canonicalFilePath();
        QSettings().setValue(
            persistedKey(canonicalPath),
            QVariantMap{{"path", canonicalPath},
                        {"activeId", "batched"},
                        {"conversations",
                         QVariantList{QVariantMap{
                             {"id", "batched"}, {"threadId", "batched"}, {"title", "Batched"}}}}});
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"),
                               {}});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        QSignalSpy reset(&desktop, &AgentSessionManager::messagesReset);
        QSignalSpy prepended(&desktop, &AgentSessionManager::messagesPrepended);
        const QString path = env.directory.path();
        desktop.activate(path);
        // The newest turn is published first, then two older turns are added.
        QTRY_COMPARE(desktop.messages(path).size(), 9);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        // Selecting the conversation clears the view first; the history itself
        // arrives in one reset.
        int historyResets = 0;
        for (const auto& arguments : reset)
            historyResets += arguments.at(1).toList().isEmpty() ? 0 : 1;
        QCOMPARE(historyResets, 1);
        QCOMPARE(desktop.messages(path).first().toMap().value("body"), "prompt-13");
        QVERIFY(desktop.historyState(path).value("hasOlder").toBool());
        prepended.clear(); // Ignore the automatic backfill when testing a lazy page.

        // Each page is published separately, after the reader stops scrolling.
        desktop.setTranscriptInputActive(true);
        desktop.loadOlderMessages(path);
        QTest::qWait(1000);
        QVERIFY(desktop.historyState(path).value("loading").toBool());
        QCOMPARE(desktop.messages(path).size(), 9);
        QCOMPARE(prepended.size(), 0);
        desktop.setTranscriptInputActive(false);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QCOMPARE(desktop.messages(path).size(), 18);
        QCOMPARE(prepended.size(), 1);
        while (desktop.historyState(path).value("hasOlder").toBool()) {
            desktop.loadOlderMessages(path);
            QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        }
        QCOMPARE(desktop.messages(path).size(), turns * 3);
        QCOMPARE(desktop.messages(path).first().toMap().value("body"), "prompt-0");
        QVERIFY(!desktop.historyState(path).value("hasOlder").toBool());
        QSet<QString> keys;
        for (const auto& value : desktop.messages(path))
            keys.insert(value.toMap().value("messageKey").toString());
        QCOMPARE(keys.size(), turns * 3);
    }

    void restoresCommentaryAndRetainsCommandLifecycle() {
        const QJsonObject commentary{{"id", "comment"},
                                     {"type", "agentMessage"},
                                     {"phase", "commentary"},
                                     {"text", "Internal commentary"}};
        const QJsonObject command{
            {"id", "command"}, {"type", "commandExecution"}, {"command", "git status --short"}};
        const auto rows = daemonWindowMessages(
            {{"events",
              QJsonArray{transcriptEvent("item.started", {{"item", commentary}}, 1),
                         transcriptEvent("assistant.delta",
                                         {{"item_id", "comment"}, {"delta", " hidden"}}, 2),
                         transcriptEvent("item.completed", {{"item", commentary}}, 3),
                         transcriptEvent("item.started", {{"item", command}}, 4)}}});
        QCOMPARE(rows.size(), 2);
        const auto commentaryRow = rows.first().toMap();
        QCOMPARE(commentaryRow.value("eventType").toString(), "assistant.commentary");
        QCOMPARE(commentaryRow.value("body").toString(), "Internal commentary");
        const auto row = rows.last().toMap();
        QCOMPARE(row.value("subtype").toString(), "command");
        QCOMPARE(row.value("phase").toString(), "started");
        QVERIFY(row.value("running").toBool());
        QCOMPARE(row.value("command").toString(), "git status --short");
    }

    void largeEventReassemblyDeduplicatesExactRetryAndRejectsConflictOrGap() {
        const QJsonObject payload{
            {"item_id", "answer"},
            {"item_type", "agentMessage"},
            {"item", QJsonObject{{"id", "answer"},
                                 {"type", "agentMessage"},
                                 {"text", QString(300 * 1024, QLatin1Char('x'))}}}};
        const QByteArray source = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex());
        const QJsonObject reference{{"version", 1},
                                    {"transfer_id", "transfer"},
                                    {"event_type", "item.completed"},
                                    {"item_id", "answer"},
                                    {"revision", 42},
                                    {"encoding", "json-utf8"},
                                    {"digest_sha256", digest},
                                    {"total_bytes", source.size()}};
        remote::LargeEventReassembler assembler(reference);
        QVERIFY(assembler.isValid());
        const auto fragment = [&](qint64 offset, QByteArray data) {
            const qint64 next = offset + data.size();
            return QJsonObject{
                {"large_event_fragment_version", 1},
                {"transfer_id", "transfer"},
                {"event_type", "item.completed"},
                {"item_id", "answer"},
                {"revision", 42},
                {"digest_sha256", digest},
                {"total_bytes", source.size()},
                {"offset", offset},
                {"next_offset", next},
                {"complete", next == source.size()},
                {"data", QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding |
                                                           QByteArray::OmitTrailingEquals))}};
        };
        const QByteArray first = source.first(128 * 1024);
        QString error;
        QVERIFY(assembler.accept(fragment(0, first), &error));
        QVERIFY(assembler.accept(fragment(0, first), &error));
        auto conflict = fragment(0, first);
        conflict.insert("data", QString::fromLatin1(QByteArray(first.size(), 'z')
                                                        .toBase64(QByteArray::Base64UrlEncoding |
                                                                  QByteArray::OmitTrailingEquals)));
        QVERIFY(!assembler.accept(conflict, &error));
        QVERIFY(error.contains("Conflicting duplicate"));
        // A retried fragment of another length at the same offset conflicts too.
        QVERIFY(!assembler.accept(fragment(0, first.first(1024)), &error));
        QVERIFY(error.contains("Conflicting duplicate"));
        QVERIFY(!assembler.accept(fragment(assembler.nextOffset() + 1, QByteArray("gap")), &error));
        QVERIFY(error.contains("gap or reordering"));
        while (!assembler.isComplete()) {
            const qint64 offset = assembler.nextOffset();
            QVERIFY(assembler.accept(fragment(offset, source.mid(offset, 128 * 1024)), &error));
        }
        QCOMPARE(assembler.payload(&error), payload);

        remote::LargeEventReassembler missing(reference);
        QVERIFY(missing.accept(fragment(0, first), &error));
        QVERIFY(missing.payload(&error).isEmpty());
        QVERIFY(error.contains("incomplete"));
    }

    // A sender splitting a transfer into tiny fragments cannot grow the bookkeeping without
    // bound (#327): the 4,097th fragment is refused, while retries and conflicts behave as before.
    void largeEventReassemblyBoundsTheFragmentCount() {
        const QJsonObject payload{
            {"item_id", "answer"},
            {"item_type", "agentMessage"},
            {"item", QJsonObject{{"id", "answer"},
                                 {"type", "agentMessage"},
                                 {"text", QString(16 * 1024, QLatin1Char('x'))}}}};
        const QByteArray source = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex());
        const QJsonObject reference{{"version", 1},
                                    {"transfer_id", "tiny"},
                                    {"event_type", "item.completed"},
                                    {"item_id", "answer"},
                                    {"revision", 7},
                                    {"encoding", "json-utf8"},
                                    {"digest_sha256", digest},
                                    {"total_bytes", source.size()}};
        const auto fragment = [&](qint64 offset, qint64 size) {
            const QByteArray data = source.mid(offset, size);
            const qint64 next = offset + data.size();
            return QJsonObject{
                {"large_event_fragment_version", 1},
                {"transfer_id", "tiny"},
                {"event_type", "item.completed"},
                {"item_id", "answer"},
                {"revision", 7},
                {"digest_sha256", digest},
                {"total_bytes", source.size()},
                {"offset", offset},
                {"next_offset", next},
                {"complete", next == source.size()},
                {"data", QString::fromLatin1(data.toBase64(QByteArray::Base64UrlEncoding |
                                                           QByteArray::OmitTrailingEquals))}};
        };
        QVERIFY(source.size() > 4096);
        remote::LargeEventReassembler assembler(reference);
        QString error;
        for (qint64 offset = 0; offset < 4096; ++offset)
            QVERIFY2(assembler.accept(fragment(offset, 1), &error), qPrintable(error));
        // An exact retry of an accepted fragment still succeeds; a conflict and a gap still fail.
        QVERIFY(assembler.accept(fragment(4095, 1), &error));
        QVERIFY(!assembler.accept(fragment(4095, 2), &error));
        QVERIFY(error.contains("Conflicting duplicate"));
        QVERIFY(!assembler.accept(fragment(4097, 1), &error));
        QVERIFY(error.contains("gap or reordering"));
        QVERIFY(!assembler.accept(fragment(4096, 1), &error));
        QVERIFY(error.contains("too many fragments"));
        QVERIFY(!assembler.isComplete());
        QVERIFY(assembler.payload(&error).isEmpty());

        // Whole chunks, as the daemon sends them, are far below the bound.
        remote::LargeEventReassembler chunked(reference);
        for (qint64 offset = 0; !chunked.isComplete(); offset = chunked.nextOffset())
            QVERIFY2(chunked.accept(fragment(offset, 96 * 1024), &error), qPrintable(error));
        QCOMPARE(chunked.payload(&error), payload);
    }

    void daemonMediaRefsRenderAsImagesWithoutInlineBytes() {
        Environment env;
        QImage pixel(3, 2, QImage::Format_ARGB32);
        pixel.fill(Qt::blue);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(pixel.save(&buffer, "PNG"));
        const QString base64 = QString::fromLatin1(png.toBase64());
        const QJsonObject capture{
            {"id", "capture"},
            {"type", "mcpToolCall"},
            {"server", "ketplus_browser"},
            {"tool", "browser_capture"},
            {"status", "completed"},
            {"arguments", QJsonObject{{"fullPage", false}}},
            {"result",
             QJsonObject{{"content",
                          QJsonArray{QJsonObject{
                              {"type", "image"}, {"data", base64}, {"mimeType", "image/png"}}}}}}};
        // Same derivation the daemon uses from its journal path (KETPLUS_DAEMON_STATE).
        McpMediaStore store(
            McpMediaStore::directoryForJournal(qEnvironmentVariable("KETPLUS_DAEMON_STATE")));
        QCOMPARE(QFileInfo(store.directory()).absoluteFilePath(),
                 QFileInfo(mcp_media::daemonMediaDirectory()).absoluteFilePath());
        const QJsonObject journaled = store.externalizeItem(capture, "daemon-session");
        QVERIFY(!QJsonDocument(journaled).toJson().contains(base64.left(24).toLatin1()));

        const auto rows = daemonWindowMessages(
            {{"events",
              QJsonArray{transcriptEvent(
                  "item.completed",
                  {{"item_id", "capture"}, {"item_type", "mcpToolCall"}, {"item", journaled}},
                  1)}}});
        QCOMPARE(rows.size(), 1);
        const QVariantList images = rows.first().toMap().value("images").toList();
        QCOMPARE(images.size(), 1);
        const QString file = QUrl(images.first().toString()).toLocalFile();
        QVERIFY(file.startsWith(QFileInfo(store.directory()).absoluteFilePath()));
        QCOMPARE(QFileInfo(file).size(), png.size());
        QVERIFY(QImage(file).size() == QSize(3, 2));

        // A reference whose file is gone (pruned) keeps its slot as a non-loading placeholder
        // source, so the activity gallery shows "Screenshot unavailable" instead of nothing.
        QVERIFY(QFile::remove(file));
        const auto pruned = daemonWindowMessages(
            {{"events",
              QJsonArray{transcriptEvent(
                  "item.completed",
                  {{"item_id", "capture"}, {"item_type", "mcpToolCall"}, {"item", journaled}},
                  1)}}});
        const QVariantList placeholders = pruned.first().toMap().value("images").toList();
        QCOMPARE(placeholders.size(), 1);
        QVERIFY(placeholders.first().toString().startsWith(
            QLatin1String(mcp_media::unavailableSourcePrefix)));
        QVERIFY(QUrl(placeholders.first().toString()).toLocalFile().isEmpty());

        // Pixels removed by history retention (ADR 0099) keep their slot the same way.
        QJsonObject retained = journaled;
        retained.insert(
            "result",
            QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "image"},
                                                           {"omitted", true},
                                                           {"reason", "history_retention"}}}}});
        const auto trimmed = daemonWindowMessages(
            {{"events",
              QJsonArray{transcriptEvent(
                  "item.completed",
                  {{"item_id", "capture"}, {"item_type", "mcpToolCall"}, {"item", retained}},
                  1)}}});
        const QVariantList keptSlots = trimmed.first().toMap().value("images").toList();
        QCOMPARE(keptSlots.size(), 1);
        QVERIFY(keptSlots.first().toString().startsWith(
            QLatin1String(mcp_media::unavailableSourcePrefix)));
    }

    void projectionMergesPartialItemsAndOutputWithoutApprovalSideEffects() {
        const auto older = daemonWindowMessages(
            {{"events",
              QJsonArray{
                  transcriptEvent("assistant.delta", {{"item_id", "a"}, {"delta", "prefix "}}, 1),
                  transcriptEvent("approval.requested", {{"item_id", "approval"}}, 2),
                  transcriptEvent("command.output_delta",
                                  {{"item_id", "cmd"}, {"delta", "SECRET_OUTPUT"}}, 3)}}});
        const auto newer = daemonWindowMessages(
            {{"partial_items", QJsonArray{QJsonObject{{"item_id", "a"}, {"turn_sequence", 1}}}},
             {"events", QJsonArray{transcriptEvent("assistant.delta",
                                                   {{"item_id", "a"}, {"delta", "suffix"}}, 4)}}});
        QCOMPARE(older.size(), 2);
        QCOMPARE(older.last().toMap().value("details").toMap().value("output"), "SECRET_OUTPUT");
        QVERIFY(newer.first().toMap().value("partialBody").toBool());
        const auto merged = mergeDaemonHistory(older, newer);
        QCOMPARE(merged.size(), 2);
        QCOMPARE(merged.first().toMap().value("body"), "prefix suffix");
        QVERIFY(!merged.first().toMap().value("partialBody").toBool());
        QCOMPARE(merged.first().toMap().value("bodyPrefixCharacters").toInt(), 7);
        QVERIFY(!transcriptText(merged).contains("SECRET_OUTPUT"));
        const auto complete = daemonWindowMessages(
            {{"events",
              QJsonArray{transcriptEvent(
                  "item.completed",
                  {{"item",
                    QJsonObject{{"id", "a"}, {"type", "agentMessage"}, {"text", "authoritative"}}}},
                  5)}}});
        QCOMPARE(mergeDaemonHistory(older, complete).first().toMap().value("body"),
                 "authoritative");
        const auto anotherTurn = daemonWindowMessages(
            {{"events",
              QJsonArray{transcriptEvent("assistant.delta",
                                         {{"item_id", "a"}, {"delta", "different turn"}}, 6, 2)}}});
        QCOMPARE(mergeDaemonHistory(merged, anotherTurn).size(), 3);
        QCOMPARE(daemonDisplayState("failed").status, "Error");
        QVERIFY(daemonDisplayState("stopping").running);
        QVERIFY(daemonDisplayState("waiting_for_input").running);
        // Nothing resumes an old daemon's "recovering" turn; it must not look like it is running.
        QVERIFY(!daemonDisplayState("recovering").running);
        QVERIFY(!daemonDisplayState("interrupted").running);
    }

    void declinedProviderRequestRestoresAsMutedNote() {
        const auto rows = daemonWindowMessages(
            {{"events", QJsonArray{transcriptEvent("provider.request_declined",
                                                   {{"method", "mcpServer/elicitation/request"},
                                                    {"reason", "unsupported_elicitation"},
                                                    {"server_name", "docs"}},
                                                   1),
                                   transcriptEvent("provider.request_declined",
                                                   {{"method", "item/tool/call"},
                                                    {"reason", "unsupported_request"}},
                                                   2)}}});
        QCOMPARE(rows.size(), 2);
        const auto note = rows.first().toMap();
        QCOMPARE(note.value("kind").toString(), "tool");
        QCOMPARE(note.value("category").toString(), "system");
        QCOMPARE(note.value("subtype").toString(), "notice");
        QVERIFY(note.value("body").toString().contains("docs MCP server"));
        QVERIFY(rows.last().toMap().value("body").toString().contains("item/tool/call"));
        QVERIFY(!transcriptText(rows).contains("The provider stopped with an error"));
    }

    void protocolErrorRestoresAsNoticeAndUnknownItemAsGenericRow() {
        const auto rows = daemonWindowMessages(
            {{"events",
              QJsonArray{transcriptEvent("provider.protocol_error",
                                         {{"provider", "codex"},
                                          {"method", "item/completed"},
                                          {"missing", QJsonArray{"item.id", "item.type"}}},
                                         1),
                         transcriptEvent(
                             "item.completed",
                             {{"item_id", "future-1"},
                              {"item_type", "futureItem"},
                              {"item", QJsonObject{{"id", "future-1"}, {"type", "futureItem"}}}},
                             2)}}});
        QCOMPARE(rows.size(), 2);
        const auto note = rows.first().toMap();
        QCOMPARE(note.value("subtype").toString(), "notice");
        QCOMPARE(note.value("title").toString(), "Protocol error");
        QVERIFY(
            note.value("body").toString().contains("item/completed without item.id, item.type"));
        const auto oversized = daemonWindowMessages(
            {{"events", QJsonArray{transcriptEvent("provider.protocol_error",
                                                   {{"provider", "codex"},
                                                    {"method", "item/completed"},
                                                    {"oversized_bytes", 17 * 1024 * 1024}},
                                                   1)}}});
        QCOMPARE(oversized.size(), 1);
        QVERIFY(oversized.first().toMap().value("body").toString().contains(
            "skipped a 17 MiB Codex message (item/completed)"));
        const auto unknown = rows.last().toMap();
        QCOMPARE(unknown.value("eventType").toString(), "unknown");
        QCOMPARE(unknown.value("title").toString(), "Used futureItem");
        QVERIFY(!transcriptText(rows).contains("The provider stopped with an error"));
    }

    void restoresSteerTextButKeepsEmptySteersAsMetadata() {
        const auto rows = daemonWindowMessages(
            {{"events",
              QJsonArray{transcriptEvent("user.message", {{"text", "Fix the sidebar"}}, 1),
                         transcriptEvent("turn.steered",
                                         {{"text", "Also update tests"}, {"provider", "claude"}},
                                         2),
                         transcriptEvent("turn.steered", {}, 3)}}});
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.first().toMap().value("body").toString(), "Fix the sidebar");
        QCOMPARE(rows.last().toMap().value("body").toString(), "Also update tests");
        // The steer keeps the provider of the turn it joined.
        QCOMPARE(rows.last().toMap().value("provider").toString(), "claude");
    }

    void restoresSteerImages() {
        Environment env;
        QImage pixel(3, 2, QImage::Format_ARGB32);
        pixel.fill(Qt::green);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(pixel.save(&buffer, "PNG"));
        McpMediaStore store(
            McpMediaStore::directoryForJournal(qEnvironmentVariable("KETPLUS_DAEMON_STATE")));
        const QJsonObject image = store.storeImage(png, "image/png", "daemon-session");
        QVERIFY(!image.isEmpty());
        const auto rows = daemonWindowMessages(
            {{"events",
              QJsonArray{transcriptEvent("user.message", {{"text", "Fix the sidebar"}}, 1),
                         transcriptEvent("turn.steered",
                                         {{"text", "Match this"}, {"images", QJsonArray{image}}},
                                         2)}}});
        QCOMPARE(rows.size(), 2);
        const QVariantList images = rows.last().toMap().value("images").toList();
        QCOMPARE(images.size(), 1);
        QVERIFY(QImage(QUrl(images.first().toString()).toLocalFile()).size() == QSize(3, 2));
    }

    void providerUserMessageItemDoesNotDuplicatePrompt() {
        const QJsonObject item{
            {"type", "userMessage"},
            {"id", "user-item"},
            {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "Draw a diagram"}}}}};
        const auto rows = daemonWindowMessages(
            {{"events", QJsonArray{transcriptEvent("user.message", {{"text", "Draw a diagram"}}, 1),
                                   transcriptEvent("item.started", {{"item", item}}, 2),
                                   transcriptEvent("item.completed", {{"item", item}}, 3)}}});
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value("kind").toString(), "user");
    }

    void pendingApprovalRestoresSeparatelyFromItsActivityGroup() {
        Environment env;
        QVERIFY(env.directory.isValid());
        const auto providerLog = env.directory.filePath("provider.log");
        const auto executable = env.directory.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, providerLog));
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"), executable});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        {
            AgentSessionManager first;
            first.activate(env.directory.path());
            first.submitPrompt(env.directory.path(), "approve-started");
            QTRY_COMPARE(first.status(env.directory.path()), "Waiting for your answer…");
        }
        AgentSessionManager reopened;
        reopened.activate(env.directory.path());
        QTRY_COMPARE(reopened.status(env.directory.path()), "Waiting for your answer…");
        QTRY_VERIFY(!reopened.isRestoring());
        QVariantMap question;
        QVariantMap group;
        const auto restoredMessages = reopened.messages(env.directory.path());
        for (const auto& value : restoredMessages) {
            const auto entry = value.toMap();
            if (entry.value("subtype") == "approval")
                question = entry;
            if (entry.value("subtype") == "daemonGroup")
                group = entry;
        }
        QVERIFY(!group.isEmpty());
        QCOMPARE(group.value("groupSize").toInt(), 1);
        const auto path = env.directory.path();
        reopened.setActivityGroupOpen(
            path, QFileInfo(path).canonicalFilePath() + ":" + reopened.activeConversationId(path),
            group.value("messageKey").toString(), true);
        const auto details = [&]() {
            for (const auto& value : reopened.messages(path)) {
                const auto row = value.toMap();
                if (row.value("messageKey") == group.value("messageKey"))
                    return row.value("details").toMap().value("groupItems").toList();
            }
            return QVariantList{};
        };
        QTRY_COMPARE(details().size(), 1);
        QCOMPARE(details().first().toMap().value("subtype").toString(), "command");
        QVERIFY(!question.isEmpty());
        QVERIFY(question.value("itemId").toString().startsWith("approval:"));
        QFile log(providerLog);
        QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY(!log.readAll().contains("approval:"));
        log.close();
        const QString acceptOptionId = question.value("questions")
                                           .toList()
                                           .first()
                                           .toMap()
                                           .value("options")
                                           .toList()
                                           .first()
                                           .toMap()
                                           .value("id")
                                           .toString();
        QVERIFY(!acceptOptionId.isEmpty());
        reopened.answerQuestion(
            env.directory.path(), question.value("messageKey").toString(),
            {{"decision", QVariantMap{{"option_ids", QStringList{acceptOptionId}},
                                      {"custom_answer", QString{}}}}});
        QTRY_VERIFY(!reopened.isRunning(env.directory.path()));
        QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY(log.readAll().contains("approval:accept"));
        for (const auto& value : reopened.messages(env.directory.path())) {
            const auto entry = value.toMap();
            if (entry.value("subtype") == "question")
                QVERIFY(!entry.value("running").toBool());
        }
    }

    void emptyStartedSuffixDoesNotDiscardItsUnloadedPrefix() {
        const QJsonArray partial{QJsonObject{{"item_id", "a"}, {"turn_sequence", 1}}};
        const auto suffix = daemonWindowMessages(
            {{"partial_items", partial},
             {"events",
              QJsonArray{transcriptEvent(
                  "item.started",
                  {{"item", QJsonObject{{"id", "a"}, {"type", "agentMessage"}, {"text", ""}}}},
                  2)}}});
        QCOMPARE(suffix.size(), 1);
        QVERIFY(suffix.first().toMap().value("partialBody").toBool());
        const auto prefix = daemonWindowMessages(
            {{"events", QJsonArray{transcriptEvent(
                            "assistant.delta", {{"item_id", "a"}, {"delta", "preserved"}}, 1)}}});
        QCOMPARE(mergeDaemonHistory(prefix, suffix).first().toMap().value("body"), "preserved");
        const auto finished = daemonWindowMessages(
            {{"partial_items", partial},
             {"events",
              QJsonArray{
                  transcriptEvent("assistant.delta", {{"item_id", "a"}, {"delta", "suffix"}}, 1),
                  transcriptEvent(
                      "item.completed",
                      {{"item",
                        QJsonObject{{"id", "a"}, {"type", "agentMessage"}, {"text", "full"}}}},
                      2),
                  transcriptEvent("item.completed",
                                  {{"item", QJsonObject{{"id", "a"}, {"type", "agentMessage"}}}},
                                  3)}}});
        QCOMPARE(finished.first().toMap().value("body"), "full");
        QVERIFY(!finished.first().toMap().value("partialBody").toBool());
    }

    void coldDiscoveryLoadsOnlySelectedThreeTurnsAndOlderPagesAreLossless() {
        Environment env;
        QVERIFY(env.directory.isValid());
        constexpr int conversations = 12;
        constexpr int turns = 40;
        {
            LocalJournal journal(env.directory.filePath("journal.sqlite3"));
            for (int session = 0; session < conversations; ++session) {
                const auto id = QStringLiteral("session-%1").arg(session);
                QVERIFY(journal.upsertSession(
                    {id, env.directory.path(), "codex", {}, {}, "ready", 1, 0, 0, id}));
                for (int turn = 0; turn < turns; ++turn) {
                    QVERIFY(journal.appendEvent(id, 1, "user.message",
                                                {{"text", QString("prompt-%1").arg(turn)}}) > 0);
                    QVERIFY(journal.appendEvent(
                                id, 1, "item.completed",
                                {{"item",
                                  QJsonObject{
                                      {"id", QString::number(turn)},
                                      {"type", "agentMessage"},
                                      {"text", QString("answer-%1-%2").arg(session).arg(turn)}}}}) >
                            0);
                    QVERIFY(journal.appendEvent(id, 1, "turn.completed",
                                                {{"status", "completed"}}) > 0);
                }
            }
        }
        const auto canonicalPath = QFileInfo(env.directory.path()).canonicalFilePath();
        QSettings().setValue(
            persistedKey(canonicalPath),
            QVariantMap{{"path", canonicalPath},
                        {"activeId", "session-0"},
                        {"conversations", QVariantList{QVariantMap{{"id", "session-0"},
                                                                   {"threadId", "session-0"},
                                                                   {"title", "Selected"}}}}});
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"),
                               {}});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        QSignalSpy replay(&server, &CodexAppServer::daemonSessionRestoreStarted);
        QSignalSpy copied(&desktop, &AgentSessionManager::conversationCopyReady);
        desktop.activate(env.directory.path());
        QTRY_COMPARE(desktop.conversations(env.directory.path()).size(), conversations);
        QTRY_COMPARE(desktop.messages(env.directory.path()).size(), 9);
        QVERIFY(server.hasDaemonTranscriptWindow());
        QCOMPARE(replay.size(), 0); // No all-session replay queue.
        QCOMPARE(latestAssistantBody(desktop.messages(env.directory.path())), "answer-0-39");
        QVERIFY(desktop.historyState(env.directory.path()).value("hasOlder").toBool());
        desktop.copyConversation(env.directory.path());
        QTRY_COMPARE(copied.size(), 1);
        QVERIFY(copied.first().at(2).toString().contains("answer-0-0\n"));
        QCOMPARE(desktop.messages(env.directory.path()).size(), 9); // Export does not hydrate UI.
        QSignalSpy restoreState(&desktop, &AgentSessionManager::restoreStateChanged);
        for (int page = 0; desktop.historyState(env.directory.path()).value("hasOlder").toBool();
             ++page) {
            QVERIFY(page < 20);
            desktop.loadOlderMessages(env.directory.path());
            QTRY_VERIFY(!desktop.historyState(env.directory.path()).value("loading").toBool());
            QVERIFY2(
                desktop.historyState(env.directory.path()).value("error").toString().isEmpty(),
                qPrintable(desktop.historyState(env.directory.path()).value("error").toString()));
        }
        QCOMPARE(restoreState.size(), 0); // History paging must not close scoped previews.
        QCOMPARE(desktop.messages(env.directory.path()).size(), turns * 3);
        QSet<QString> keys;
        for (const auto& value : desktop.messages(env.directory.path()))
            keys.insert(value.toMap().value("messageKey").toString());
        QCOMPARE(keys.size(), turns * 3);
        // A -> B while A's older request is in flight must never publish A into B.
        for (int session = 1; session < conversations; ++session) {
            desktop.selectConversation(env.directory.path(), QString("session-%1").arg(session));
            QTRY_VERIFY(!desktop.historyState(env.directory.path()).value("loading").toBool());
            QCOMPARE(desktop.messages(env.directory.path()).size(), 9);
            QCOMPARE(latestAssistantBody(desktop.messages(env.directory.path())),
                     QString("answer-%1-39").arg(session));
        }
        desktop.selectConversation(env.directory.path(), "session-0");
        QTRY_VERIFY(!desktop.historyState(env.directory.path()).value("loading").toBool());
        QCOMPARE(desktop.messages(env.directory.path()).size(),
                 9); // Evicted, reconstructed from newest window.
        desktop.loadOlderMessages(env.directory.path());
        desktop.selectConversation(env.directory.path(), "session-11");
        QTest::qWait(100);
        QCOMPARE(latestAssistantBody(desktop.messages(env.directory.path())), "answer-11-39");
    }

    void leavingDuringOlderPageKeepsLoadedWindow() {
        Environment env;
        QVERIFY(env.directory.isValid());
        {
            LocalJournal journal(env.directory.filePath("journal.sqlite3"));
            for (int session = 0; session < 2; ++session) {
                const auto id = QStringLiteral("session-%1").arg(session);
                QVERIFY(journal.upsertSession(
                    {id, env.directory.path(), "codex", {}, {}, "ready", 1, 0, 0, id}));
                for (int turn = 0; turn < 40; ++turn) {
                    QVERIFY(journal.appendEvent(id, 1, "user.message",
                                                {{"text", QString("prompt-%1").arg(turn)}}) > 0);
                    QVERIFY(journal.appendEvent(
                                id, 1, "item.completed",
                                {{"item", QJsonObject{{"id", QString::number(turn)},
                                                      {"type", "agentMessage"},
                                                      {"text", QString("answer-%1").arg(turn)}}}}) >
                            0);
                    QVERIFY(journal.appendEvent(id, 1, "turn.completed",
                                                {{"status", "completed"}}) > 0);
                }
            }
        }
        const auto canonicalPath = QFileInfo(env.directory.path()).canonicalFilePath();
        QSettings().setValue(
            persistedKey(canonicalPath),
            QVariantMap{{"path", canonicalPath},
                        {"activeId", "session-0"},
                        {"conversations", QVariantList{QVariantMap{{"id", "session-0"},
                                                                   {"threadId", "session-0"},
                                                                   {"title", "Selected"}}}}});
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"),
                               {}});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        const auto path = env.directory.path();
        desktop.activate(path);
        QTRY_COMPARE(desktop.conversations(path).size(), 2);
        QTRY_COMPARE(desktop.messages(path).size(), 9);
        for (int page = 0; page < 3; ++page) {
            desktop.loadOlderMessages(path);
            QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        }
        const auto loaded = desktop.messages(path);
        QVERIFY(loaded.size() > 9);

        // A pending older page is dropped on switch, but the window already
        // loaded is intact and nothing happened while away. Returning shows it
        // as it was: re-reading the newest turn would rebuild the transcript
        // twice and move the reader to the bottom of a different window.
        desktop.loadOlderMessages(path);
        QVERIFY(desktop.historyState(path).value("loading").toBool());
        desktop.selectConversation(path, "session-1");
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QSignalSpy resets(&desktop, &AgentSessionManager::messagesReset);
        QSignalSpy prepended(&desktop, &AgentSessionManager::messagesPrepended);
        QSignalSpy windows(&desktop, &AgentSessionManager::messagesWindowReset);
        desktop.selectConversation(path, "session-0");
        QTest::qWait(200);
        QVERIFY(!desktop.historyState(path).value("loading").toBool());
        QCOMPARE(resets.size(), 1); // The selection itself.
        QCOMPARE(prepended.size(), 0);
        QCOMPARE(windows.size(), 0);
        QCOMPARE(desktop.messages(path).size(), loaded.size());
        QCOMPARE(desktop.messages(path).first().toMap().value("messageKey"),
                 loaded.first().toMap().value("messageKey"));
        // Paging continues from where the dropped request started.
        desktop.loadOlderMessages(path);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QVERIFY(desktop.messages(path).size() > loaded.size());
    }

    void coldRestorePublishesEightMiBItemOnlyAfterVerifiedReassembly() {
        Environment env;
        QVERIFY(env.directory.isValid());
        const QString text(3 * 1024 * 1024, QChar(0x20ac));
        {
            LocalJournal journal(env.directory.filePath("journal.sqlite3"));
            QVERIFY(journal.upsertSession(
                {"giant", env.directory.path(), "codex", {}, {}, "ready", 1, 0, 0, "Giant"}));
            QVERIFY(journal.appendEvent("giant", 1, "user.message", {{"text", "prompt"}}) > 0);
            QVERIFY(journal.appendEvent("giant", 1, "item.completed",
                                        {{"item", QJsonObject{{"id", "answer"},
                                                              {"type", "agentMessage"},
                                                              {"text", text}}}}) > 0);
        }
        const auto canonicalPath = QFileInfo(env.directory.path()).canonicalFilePath();
        QSettings().setValue(
            persistedKey(canonicalPath),
            QVariantMap{{"path", canonicalPath},
                        {"activeId", "giant"},
                        {"conversations",
                         QVariantList{QVariantMap{
                             {"id", "giant"}, {"threadId", "giant"}, {"title", "Giant"}}}}});
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"),
                               {}});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(env.directory.path());
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.isRestoring(), 30'000);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(env.directory.path()).size(), 2, 30'000);
        const QVariantMap answer = desktop.messages(env.directory.path()).last().toMap();
        QCOMPARE(answer.value("body").toString().size(), text.size());
        QCOMPARE(answer.value("body").toString(), text);
        QVERIFY(!answer.value("partialBody").toBool());
    }

    void repeatedOlderPagingEvictsAndReloadsFromJournal() {
        Environment env;
        QVERIFY(env.directory.isValid());
        constexpr int turns = 180;
        {
            LocalJournal journal(env.directory.filePath("journal.sqlite3"));
            QVERIFY(journal.upsertSession(
                {"bounded", env.directory.path(), "codex", {}, {}, "ready", 1, 0, 0, "Bounded"}));
            for (int turn = 0; turn < turns; ++turn) {
                QVERIFY(journal.appendEvent("bounded", 1, "user.message",
                                            {{"text", QString("prompt-%1").arg(turn)}}) > 0);
                QVERIFY(journal.appendEvent(
                            "bounded", 1, "item.completed",
                            {{"item", QJsonObject{{"id", QString::number(turn)},
                                                  {"type", "agentMessage"},
                                                  {"text", QString("answer-%1").arg(turn)}}}}) > 0);
                QVERIFY(journal.appendEvent("bounded", 1, "turn.completed",
                                            {{"status", "completed"}}) > 0);
            }
        }
        const auto canonicalPath = QFileInfo(env.directory.path()).canonicalFilePath();
        QSettings().setValue(
            persistedKey(canonicalPath),
            QVariantMap{{"path", canonicalPath},
                        {"activeId", "bounded"},
                        {"conversations",
                         QVariantList{QVariantMap{
                             {"id", "bounded"}, {"threadId", "bounded"}, {"title", "Bounded"}}}}});
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"),
                               {}});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(env.directory.path());
        QTRY_COMPARE(desktop.messages(env.directory.path()).size(), 9);

        int pages = 0;
        while (!desktop.historyState(env.directory.path()).value("hasNewer").toBool()) {
            QVERIFY2(++pages < 80, "active window never reached its hard bound");
            desktop.loadOlderMessages(env.directory.path());
            QTRY_VERIFY_WITH_TIMEOUT(
                !desktop.historyState(env.directory.path()).value("loading").toBool(), 5000);
        }
        QVERIFY(desktop.messages(env.directory.path()).size() <= 300);
        const QString historicalTail = latestAssistantBody(desktop.messages(env.directory.path()));
        QVERIFY(historicalTail != QStringLiteral("answer-179"));
        QVERIFY(desktop.historyState(env.directory.path()).value("hasOlder").toBool());

        desktop.loadLatestMessages(env.directory.path());
        QTRY_VERIFY_WITH_TIMEOUT(
            !desktop.historyState(env.directory.path()).value("loading").toBool(), 5000);
        QCOMPARE(desktop.messages(env.directory.path()).size(), 9);
        QCOMPARE(latestAssistantBody(desktop.messages(env.directory.path())),
                 QStringLiteral("answer-179"));
        QVERIFY(!desktop.historyState(env.directory.path()).value("hasNewer").toBool());
        QVERIFY(desktop.historyState(env.directory.path()).value("hasOlder").toBool());
    }

    void newerPagesAppendAfterTrimWithoutGapsOrDuplicates() {
        Environment env;
        QVERIFY(env.directory.isValid());
        constexpr int turns = 180;
        {
            LocalJournal journal(env.directory.filePath("journal.sqlite3"));
            QVERIFY(journal.upsertSession(
                {"bounded", env.directory.path(), "codex", {}, {}, "ready", 1, 0, 0, "Bounded"}));
            for (int turn = 0; turn < turns; ++turn) {
                QVERIFY(journal.appendEvent("bounded", 1, "user.message",
                                            {{"text", QString("prompt-%1").arg(turn)}}) > 0);
                QVERIFY(journal.appendEvent(
                            "bounded", 1, "item.completed",
                            {{"item", QJsonObject{{"id", QString::number(turn)},
                                                  {"type", "agentMessage"},
                                                  {"text", QString("answer-%1").arg(turn)}}}}) > 0);
                QVERIFY(journal.appendEvent("bounded", 1, "turn.completed",
                                            {{"status", "completed"}}) > 0);
            }
        }
        rememberConversation(env.directory.path(), "bounded");
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"),
                               {}});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        const QString path = env.directory.path();
        desktop.activate(path);
        QTRY_COMPARE(desktop.messages(path).size(), 9);
        QVERIFY(server.hasDaemonTranscriptAfterCursor());
        QCOMPARE(server.daemonTranscriptWindowMaxTurns(), 20);
        const auto state = [&] { return desktop.historyState(path); };
        const auto settle = [&] {
            QTRY_VERIFY_WITH_TIMEOUT(!state().value("loading").toBool(), 5000);
            QVERIFY2(state().value("error").toString().isEmpty(),
                     qPrintable(state().value("error").toString()));
        };
        // Every loaded window is one contiguous, duplicate-free run of whole turns.
        const auto window = [&](int* first, int* last) {
            const auto rows = desktop.messages(path);
            QSet<QString> keys;
            int expected = -1;
            for (int index = 0; index < rows.size(); ++index) {
                const auto row = rows.at(index).toMap();
                if (keys.contains(row.value("messageKey").toString()))
                    return false;
                keys.insert(row.value("messageKey").toString());
                if (index % 3 == 2) {
                    if (row.value("subtype") != "turnEnd" || row.value("status") != "completed")
                        return false;
                    continue;
                }
                const QString body = row.value("body").toString();
                const bool user = index % 3 == 0;
                const int number = body.section('-', 1).toInt() *
                                   (body.startsWith(user ? "prompt-" : "answer-") ? 1 : -100000);
                if (number < 0 || (expected >= 0 && number != expected + (user ? 1 : 0)))
                    return false;
                expected = number;
                if (index == 0)
                    *first = number;
            }
            *last = expected;
            return rows.size() % 3 == 0;
        };
        int first = 0;
        int last = 0;

        // Three viewports at 100 px per row and three rows per turn ask for 7 turns.
        desktop.loadOlderMessages(path, 700, 100);
        settle();
        QCOMPARE(desktop.messages(path).size(), 9 + 7 * 3);
        int pages = 0;
        while (!state().value("hasNewer").toBool()) {
            QVERIFY(++pages < 40);
            desktop.loadOlderMessages(path, 700, 100);
            settle();
            QVERIFY(window(&first, &last));
        }
        QVERIFY(desktop.messages(path).size() <= 300);
        QVERIFY(last < turns - 1);
        QVERIFY(state().value("canLoadNewer").toBool());

        // Scroll back down past the trim, then up and down again.
        for (int round = 0; round < 2; ++round) {
            pages = 0;
            int previousLast = last;
            while (state().value("hasNewer").toBool()) {
                QVERIFY(++pages < 40);
                desktop.loadNewerMessages(path, 700, 100);
                settle();
                QVERIFY2(window(&first, &last), "newer page left a gap or duplicate");
                QVERIFY(last > previousLast);
                previousLast = last;
                QVERIFY(desktop.messages(path).size() <= 300);
            }
            QCOMPARE(last, turns - 1);
            QVERIFY(!state().value("canLoadNewer").toBool());
            QVERIFY(state().value("hasOlder").toBool());
            pages = 0;
            while (round == 0 && !state().value("hasNewer").toBool()) {
                QVERIFY(++pages < 40);
                int previousFirst = first;
                desktop.loadOlderMessages(path, 700, 100);
                settle();
                QVERIFY2(window(&first, &last), "older page after newer trim left a gap");
                QVERIFY(first < previousFirst);
            }
        }
    }

    // Pages that land while the reader scrolls only ever add rows, so the window
    // keeps growing ahead of them. Once scrolling stops it is trimmed back to its
    // budget around the row being read, and paging continues from there in both
    // directions without gaps.
    void idleTrimFollowingTheTailKeepsNewestRowsOverTheByteBound() {
        Environment env;
        QVERIFY(env.directory.isValid());
        // 240 answers of 80 KB: a window grown while scrolling passes the 16 MiB bound.
        constexpr int turns = 240;
        const QString bulk(80 * 1000, QLatin1Char('x'));
        {
            LocalJournal journal(env.directory.filePath("journal.sqlite3"));
            QVERIFY(journal.upsertSession(
                {"bulk", env.directory.path(), "codex", {}, {}, "ready", 1, 0, 0, "Bulk"}));
            for (int turn = 0; turn < turns; ++turn) {
                QVERIFY(journal.appendEvent("bulk", 1, "user.message",
                                            {{"text", QString("prompt-%1").arg(turn)}}) > 0);
                QVERIFY(journal.appendEvent(
                            "bulk", 1, "item.completed",
                            {{"item",
                              QJsonObject{{"id", QString::number(turn)},
                                          {"type", "agentMessage"},
                                          {"text", QString("answer-%1 ").arg(turn) + bulk}}}}) > 0);
                QVERIFY(journal.appendEvent("bulk", 1, "turn.completed",
                                            {{"status", "completed"}}) > 0);
            }
        }
        rememberConversation(env.directory.path(), "bulk");
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"),
                               {}});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        QSignalSpy windows(&desktop, &AgentSessionManager::messagesWindowReset);
        const QString path = env.directory.path();
        desktop.activate(path);
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.messages(path).isEmpty(), 5000);
        const auto state = [&] { return desktop.historyState(path); };
        const auto newestBody = [&] { return latestAssistantBody(desktop.messages(path)); };
        const QString newest = newestBody();
        QVERIFY(newest.startsWith(QString("answer-%1 ").arg(turns - 1)));

        desktop.setTranscriptScrolling(true);
        int pages = 0;
        while (state().value("hasOlder").toBool() && desktop.messages(path).size() < 420) {
            QVERIFY(++pages < 80);
            desktop.setTranscriptScrolling(false);
            desktop.loadOlderMessages(path, 700, 100);
            QTRY_VERIFY_WITH_TIMEOUT(!state().value("loading").toBool(), 5000);
            desktop.setTranscriptScrolling(true);
        }
        QVERIFY(desktop.messages(path).size() > 300);

        windows.clear();
        desktop.setTranscriptScrolling(false);
        QTRY_VERIFY_WITH_TIMEOUT(windows.size() == 1, 5000);
        QVERIFY(desktop.messages(path).size() <= 300);
        QCOMPARE(newestBody(), newest);
        QVERIFY2(!state().value("hasNewer").toBool(),
                 "the idle trim evicted the tail under a reader following it");
    }

    void windowGrowsWhileScrollingAndTrimsAroundReaderWhenIdle() {
        Environment env;
        QVERIFY(env.directory.isValid());
        constexpr int turns = 240;
        {
            LocalJournal journal(env.directory.filePath("journal.sqlite3"));
            QVERIFY(journal.upsertSession(
                {"grow", env.directory.path(), "codex", {}, {}, "ready", 1, 0, 0, "Grow"}));
            for (int turn = 0; turn < turns; ++turn) {
                QVERIFY(journal.appendEvent("grow", 1, "user.message",
                                            {{"text", QString("prompt-%1").arg(turn)}}) > 0);
                QVERIFY(journal.appendEvent(
                            "grow", 1, "item.completed",
                            {{"item", QJsonObject{{"id", QString::number(turn)},
                                                  {"type", "agentMessage"},
                                                  {"text", QString("answer-%1").arg(turn)}}}}) > 0);
                QVERIFY(journal.appendEvent("grow", 1, "turn.completed",
                                            {{"status", "completed"}}) > 0);
            }
        }
        rememberConversation(env.directory.path(), "grow");
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"),
                               {}});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        QSignalSpy windows(&desktop, &AgentSessionManager::messagesWindowReset);
        const QString path = env.directory.path();
        desktop.activate(path);
        QTRY_COMPARE(desktop.messages(path).size(), 9);
        const auto state = [&] { return desktop.historyState(path); };
        const auto settle = [&] {
            QTRY_VERIFY_WITH_TIMEOUT(!state().value("loading").toBool(), 5000);
            QVERIFY2(state().value("error").toString().isEmpty(),
                     qPrintable(state().value("error").toString()));
        };
        const auto contiguous = [&] {
            const auto rows = desktop.messages(path);
            int expected = -1;
            for (int index = 0; index < rows.size(); ++index) {
                const auto row = rows.at(index).toMap();
                if (index % 3 == 2) {
                    if (row.value("subtype") != "turnEnd" || row.value("status") != "completed")
                        return false;
                    continue;
                }
                const QString body = row.value("body").toString();
                const int number = body.section('-', 1).toInt();
                if (!body.startsWith(index % 3 == 0 ? "prompt-" : "answer-"))
                    return false;
                if (expected >= 0 && number != expected + (index % 3 == 0 ? 1 : 0))
                    return false;
                expected = number;
            }
            return true;
        };

        desktop.setTranscriptScrolling(true);
        int pages = 0;
        while (state().value("hasOlder").toBool() && desktop.messages(path).size() < 400) {
            QVERIFY(++pages < 60);
            // Scrolling flickers to stopped between wheel bursts; a page that
            // lands in such a gap must not trim either.
            desktop.setTranscriptScrolling(false);
            desktop.loadOlderMessages(path, 700, 100);
            settle();
            desktop.setTranscriptScrolling(true);
        }
        QVERIFY2(desktop.messages(path).size() > 300,
                 qPrintable(QString("window stopped at %1 rows while scrolling")
                                .arg(desktop.messages(path).size())));
        QVERIFY(!state().value("hasNewer").toBool());
        QVERIFY(contiguous());

        const auto rows = desktop.messages(path);
        const QString readingKey = rows.at(rows.size() / 2).toMap().value("messageKey").toString();
        windows.clear();
        desktop.setTranscriptScrolling(false, readingKey);
        QTRY_VERIFY_WITH_TIMEOUT(desktop.messages(path).size() <= 300, 5000);
        QCOMPARE(windows.size(), 1);
        bool found = false;
        for (const auto& value : desktop.messages(path))
            found = found || value.toMap().value("messageKey").toString() == readingKey;
        QVERIFY2(found, "the idle trim dropped the row being read");
        QVERIFY(contiguous());
        QVERIFY(state().value("hasNewer").toBool());
        QVERIFY(state().value("hasOlder").toBool());

        // An older request can finish after the reader has moved back toward the
        // newer end of the current window. Publication must retain that row.
        const auto beforePage = desktop.messages(path);
        const QString movedReadingKey =
            beforePage.at(beforePage.size() - 2).toMap().value("messageKey").toString();
        desktop.setTranscriptReadingKey(movedReadingKey);
        desktop.loadOlderMessages(path, 700, 100);
        settle();
        bool movedReaderRetained = false;
        for (const auto& row : desktop.messages(path))
            movedReaderRetained |= row.toMap().value("messageKey").toString() == movedReadingKey;
        QVERIFY2(movedReaderRetained, "a completed page evicted the current reading row");

        // Explicitly return toward the live tail for the following paging walk.
        desktop.setTranscriptReadingKey({});
        // Paging resumes from both trimmed edges without gaps.
        pages = 0;
        while (state().value("hasNewer").toBool()) {
            QVERIFY(++pages < 60);
            desktop.loadNewerMessages(path, 700, 100);
            settle();
            QVERIFY2(contiguous(), "newer page after an idle trim left a gap");
        }
        QCOMPARE(latestAssistantBody(desktop.messages(path)), QStringLiteral("answer-239"));
    }

    void newerPagingFallsBackWithoutAfterCursorCapability() {
        Environment env;
        QVERIFY(env.directory.isValid());
        QVERIFY(writeToken(env.directory));
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = "legacy";
        rememberConversation(path, id);
        QList<QJsonObject> windowRequests;
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                if (type == "session.list")
                    return {{"transcript_window_version", 1},
                            {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "ready"},
                                                                {"last_event_cursor", 4}}}}};
                if (type == "session.transcript_window") {
                    const auto payload = envelope.value("payload").toObject();
                    windowRequests.append(payload);
                    const bool older = payload.value("before_cursor").toInteger() > 0;
                    QJsonArray events;
                    if (!older)
                        events = {windowEvent(id, "user.message", {{"text", "second"}}, 3, 2),
                                  windowEvent(id, "item.completed",
                                              {{"item", QJsonObject{{"id", "b"},
                                                                    {"type", "agentMessage"},
                                                                    {"phase", "final_answer"},
                                                                    {"text", "newer"}}}},
                                              4, 2)};
                    return {{"transcript_window_version", 1},
                            {"session_id", id},
                            {"session_generation", 1},
                            {"events", events},
                            {"before_cursor", older ? 0 : 3},
                            {"has_more_before", !older},
                            {"snapshot_cursor", 4},
                            {"projection_revision", 1},
                            {"current_turn_sequence", 2},
                            {"state", "ready"},
                            {"pending_approvals", QJsonArray{}}};
                }
                return {};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 2, 5000);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QVERIFY(server.hasDaemonTranscriptWindow());
        QVERIFY(!server.hasDaemonTranscriptAfterCursor());
        QCOMPARE(server.daemonTranscriptWindowMaxTurns(), 3);
        QVERIFY(!desktop.historyState(path).value("canLoadNewer").toBool());
        QCOMPARE(windowRequests.size(), 1);
        desktop.loadNewerMessages(path, 700, 20, 7);
        QTest::qWait(100);
        QCOMPARE(windowRequests.size(), 1); // No after_cursor request to a legacy daemon.
        // The declined request still completes for its token, without loading.
        QCOMPARE(desktop.historyState(path).value("requestToken").toLongLong(), 7);
        QVERIFY(!desktop.historyState(path).value("loading").toBool());
        // Adaptive sizing respects the legacy three-turn ceiling.
        desktop.loadOlderMessages(path, 700, 20);
        QTRY_COMPARE(windowRequests.size(), 2);
        QCOMPARE(windowRequests.last().value("turn_limit").toInt(), 3);
        QVERIFY(!windowRequests.last().contains("after_cursor"));
    }

    // Pages beyond the window are read in the background, so a lazy load finds
    // them in memory and sends the daemon nothing.
    void lazyLoadsServeReadAheadPages() {
        transcript_paging::setPageCounts({1, 1, true, false});
        Environment env;
        QVERIFY(env.directory.isValid());
        QVERIFY(writeToken(env.directory));
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = "ahead";
        rememberConversation(path, id);
        QList<QJsonObject> windowRequests;
        const auto answer = [&](const QString& item, const QString& text, qint64 cursor,
                                qint64 turn) {
            return windowEvent(id, "item.completed",
                               {{"item", QJsonObject{{"id", item},
                                                     {"type", "agentMessage"},
                                                     {"phase", "final_answer"},
                                                     {"text", text}}}},
                               cursor, turn);
        };
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                if (type == "session.list")
                    return {{"transcript_window_version", 1},
                            {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "ready"},
                                                                {"last_event_cursor", 4}}}}};
                if (type == "session.transcript_window") {
                    const auto payload = envelope.value("payload").toObject();
                    windowRequests.append(payload);
                    const bool older = payload.value("before_cursor").toInteger() > 0;
                    const QJsonArray events =
                        older
                            ? QJsonArray{windowEvent(id, "user.message", {{"text", "first"}}, 1, 1),
                                         answer("a", "older", 2, 1)}
                            : QJsonArray{
                                  windowEvent(id, "user.message", {{"text", "second"}}, 3, 2),
                                  answer("b", "newer", 4, 2)};
                    return {{"transcript_window_version", 1},
                            {"session_id", id},
                            {"session_generation", 1},
                            {"events", events},
                            {"before_cursor", older ? 1 : 3},
                            {"has_more_before", !older},
                            {"snapshot_cursor", 4},
                            {"projection_revision", 1},
                            {"current_turn_sequence", 2},
                            {"state", "ready"},
                            {"pending_approvals", QJsonArray{}}};
                }
                return {};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 4, 5000);
        // The preceding turns are loaded after the latest turn reaches the model.
        QTRY_COMPARE_WITH_TIMEOUT(windowRequests.size(), 2, 5000);
        QCOMPARE(windowRequests.last().value("before_cursor").toInteger(), 3);
        QTest::qWait(100);
        desktop.loadOlderMessages(path, 700, 20);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 4, 5000);
        QCOMPARE(desktop.messages(path).constFirst().toMap().value("body").toString(),
                 QStringLiteral("first"));
        QCOMPARE(windowRequests.size(), 2);
    }

    // A turn streamed live after the initial window has no window-page cursors. When an
    // older page trims exactly that turn, newer paging must still resume after it.
    void liveTurnCursorsResumeNewerPagingAfterTrim() {
        Environment env;
        QVERIFY(env.directory.isValid());
        QVERIFY(writeToken(env.directory));
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = "live-trim";
        rememberConversation(path, id);
        const auto turnEvents = [&](int first, int last) {
            QJsonArray events;
            for (int turn = first; turn <= last; ++turn) {
                events.append(windowEvent(id, "user.message",
                                          {{"text", QString("prompt-%1").arg(turn)}}, 2 * turn + 1,
                                          turn));
                events.append(
                    windowEvent(id, "item.completed",
                                {{"item", QJsonObject{{"id", QString("answer-%1").arg(turn)},
                                                      {"type", "agentMessage"},
                                                      {"phase", "final_answer"},
                                                      {"text", QString("answer-%1").arg(turn)}}}},
                                2 * turn + 2, turn));
            }
            return events;
        };
        QList<QJsonObject> windowRequests;
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                if (type == "session.list")
                    return {{"transcript_window_version", 1},
                            {"transcript_window_max_turns", 20},
                            {"transcript_window_after_cursor", true},
                            {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "ready"},
                                                                {"last_event_cursor", 300}}}}};
                if (type != "session.transcript_window")
                    return {};
                const auto payload = envelope.value("payload").toObject();
                windowRequests.append(payload);
                const QJsonObject base{{"transcript_window_version", 1},
                                       {"session_id", id},
                                       {"session_generation", 1},
                                       {"snapshot_cursor", 300},
                                       {"projection_revision", 1},
                                       {"current_turn_sequence", 149},
                                       {"state", "ready"},
                                       {"pending_approvals", QJsonArray{}}};
                auto page = base;
                if (payload.contains("after_cursor")) {
                    page.insert("events", QJsonArray{});
                    page.insert("after_cursor", payload.value("after_cursor"));
                    page.insert("has_more_after", false);
                    return page;
                }
                // Initial: turns 147-149. Older pages: seven of 20 turns, then turns 0-6.
                const qint64 before = payload.value("before_cursor").toInteger();
                const int end = before > 0 ? int((before - 1) / 2) : 150;
                const int first = before > 0 ? std::max(0, end - (end > 7 ? 20 : 7)) : 147;
                page.insert("events", turnEvents(first, end - 1));
                page.insert("before_cursor", 2 * first + 1);
                page.insert("has_more_before", first > 0);
                return page;
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 6, 5000);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QVERIFY(server.hasDaemonTranscriptAfterCursor());

        // Turn 150 streams live after the initial window (cursors 301-302).
        daemon.push(liveEvent(id, "user.message", {{"text", "live prompt"}}, 301, 150));
        daemon.push(liveEvent(id, "item.completed",
                              {{"item", QJsonObject{{"id", "live-answer"},
                                                    {"type", "agentMessage"},
                                                    {"phase", "final_answer"},
                                                    {"text", "live answer"}}}},
                              302, 150));
        QTRY_COMPARE(desktop.messages(path).size(), 8);

        // 8 + 7 * 40 + 14 = 302 rows: the last older page trims exactly the live turn.
        for (int pages = 0; desktop.historyState(path).value("hasOlder").toBool(); ++pages) {
            QVERIFY(pages < 12);
            desktop.loadOlderMessages(path, 700, 100);
            QTRY_VERIFY_WITH_TIMEOUT(!desktop.historyState(path).value("loading").toBool(), 5000);
            QVERIFY2(desktop.historyState(path).value("error").toString().isEmpty(),
                     qPrintable(desktop.historyState(path).value("error").toString()));
        }
        QCOMPARE(desktop.messages(path).size(), 300);
        QCOMPARE(desktop.messages(path).last().toMap().value("body").toString(), "answer-149");
        QVERIFY(desktop.historyState(path).value("hasNewer").toBool());
        QVERIFY(desktop.historyState(path).value("canLoadNewer").toBool());
        desktop.loadNewerMessages(path, 700, 100, 11);
        QTRY_VERIFY(windowRequests.last().contains("after_cursor"));
        QCOMPARE(windowRequests.last().value("after_cursor").toInteger(), 300);
        QTRY_COMPARE(desktop.historyState(path).value("requestToken").toLongLong(), 11);
    }

    // While a turn runs, older pages grow the window rather than evict its newest
    // rows: that would cut the live tail, and the next live row would reload the
    // latest window. The idle trim then bounds it around the reader.
    void olderPageDuringRunningTurnKeepsTheLiveTail() {
        Environment env;
        QVERIFY(env.directory.isValid());
        QVERIFY(writeToken(env.directory));
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = "running-older";
        rememberConversation(path, id);
        const auto turnEvents = [&](int first, int last) {
            QJsonArray events;
            for (int turn = first; turn <= last; ++turn) {
                events.append(windowEvent(id, "user.message",
                                          {{"text", QString("prompt-%1").arg(turn)}}, 2 * turn + 1,
                                          turn));
                events.append(
                    windowEvent(id, "item.completed",
                                {{"item", QJsonObject{{"id", QString("answer-%1").arg(turn)},
                                                      {"type", "agentMessage"},
                                                      {"phase", "final_answer"},
                                                      {"text", QString("answer-%1").arg(turn)}}}},
                                2 * turn + 2, turn));
            }
            return events;
        };
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                if (type == "session.list")
                    return {{"transcript_window_version", 1},
                            {"transcript_window_max_turns", 20},
                            {"transcript_window_after_cursor", true},
                            {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "running"},
                                                                {"last_event_cursor", 400}}}}};
                if (type != "session.transcript_window")
                    return {};
                const auto payload = envelope.value("payload").toObject();
                QJsonObject page{{"transcript_window_version", 1},
                                 {"session_id", id},
                                 {"session_generation", 1},
                                 {"snapshot_cursor", 400},
                                 {"projection_revision", 1},
                                 {"current_turn_sequence", 199},
                                 {"state", "running"},
                                 {"pending_approvals", QJsonArray{}}};
                // Initial: turns 197-199. Older pages: 20 turns each.
                const qint64 before = payload.value("before_cursor").toInteger();
                const int end = before > 0 ? int((before - 1) / 2) : 200;
                const int first = before > 0 ? std::max(0, end - 20) : 197;
                page.insert("events", turnEvents(first, end - 1));
                page.insert("before_cursor", 2 * first + 1);
                page.insert("has_more_before", first > 0);
                return page;
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 6, 5000);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());

        daemon.push(liveEvent(id, "user.message", {{"text", "live prompt"}}, 401, 200));
        QTRY_COMPARE(desktop.messages(path).size(), 7);

        const auto state = [&] { return desktop.historyState(path); };
        const auto olderPage = [&] {
            desktop.loadOlderMessages(path, 700, 100);
            QTRY_VERIFY_WITH_TIMEOUT(!state().value("loading").toBool(), 5000);
            QVERIFY2(state().value("error").toString().isEmpty(),
                     qPrintable(state().value("error").toString()));
        };
        const auto body = [&](int index) {
            const auto rows = desktop.messages(path);
            return rows.at(index < 0 ? rows.size() + index : index)
                .toMap()
                .value("body")
                .toString();
        };

        // 7 + 8 * 40 rows passes the 300-row window without evicting either edge.
        for (int pages = 0; pages < 8; ++pages)
            olderPage();
        QCOMPARE(desktop.messages(path).size(), 327);
        QCOMPARE(body(-1), "live prompt");
        QCOMPARE(body(0), "prompt-37");
        QVERIFY(!state().value("hasNewer").toBool());

        // Following the tail, the idle trim keeps the newest rows.
        QTRY_VERIFY_WITH_TIMEOUT(desktop.messages(path).size() <= 300, 5000);
        QCOMPARE(body(-1), "live prompt");
        QVERIFY(!state().value("hasNewer").toBool());
        QVERIFY(state().value("hasOlder").toBool());

        // A reader who scrolled up keeps the rows being read, and the live tail:
        // while the turn runs, the idle trim never evicts rows newer than them.
        const QString reading = desktop.messages(path).at(2).toMap().value("messageKey").toString();
        desktop.setTranscriptScrolling(false, reading);
        olderPage();
        const auto grown = desktop.messages(path).size();
        QVERIFY(grown > 300);
        QTest::qWait(2000);
        QCOMPARE(desktop.messages(path).size(), grown);
        bool found = false;
        for (const auto& value : desktop.messages(path))
            found = found || value.toMap().value("messageKey").toString() == reading;
        QVERIFY2(found, "an older page evicted the row being read");
        QCOMPARE(body(-1), "live prompt");
        QVERIFY(!state().value("hasNewer").toBool());
    }

    void idleByteTrimDuringRunningTurnKeepsReaderAndLiveTail() {
        Environment env;
        QVERIFY(env.directory.isValid());
        QVERIFY(writeToken(env.directory));
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = "running-bytes";
        rememberConversation(path, id);
        // 160 answers of 55 KB, sized twice as body and sanitized payload, pass the
        // 16 MiB bound in 320 rows; the rows from the reader on stay under it.
        // Pages hold six turns to stay under the 1 MiB frame.
        const QString bulk(55 * 1000, QLatin1Char('x'));
        const auto turnEvents = [&](int first, int last) {
            QJsonArray events;
            for (int turn = first; turn <= last; ++turn) {
                events.append(windowEvent(id, "user.message",
                                          {{"text", QString("prompt-%1").arg(turn)}}, 2 * turn + 1,
                                          turn));
                events.append(windowEvent(
                    id, "item.completed",
                    {{"item", QJsonObject{{"id", QString("answer-%1").arg(turn)},
                                          {"type", "agentMessage"},
                                          {"phase", "final_answer"},
                                          {"text", QString("answer-%1 ").arg(turn) + bulk}}}},
                    2 * turn + 2, turn));
            }
            return events;
        };
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                if (type == "session.list")
                    return {{"transcript_window_version", 1},
                            {"transcript_window_max_turns", 20},
                            {"transcript_window_after_cursor", true},
                            {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "running"},
                                                                {"last_event_cursor", 320}}}}};
                if (type != "session.transcript_window")
                    return {};
                const auto payload = envelope.value("payload").toObject();
                QJsonObject page{{"transcript_window_version", 1},
                                 {"session_id", id},
                                 {"session_generation", 1},
                                 {"snapshot_cursor", 320},
                                 {"projection_revision", 1},
                                 {"current_turn_sequence", 159},
                                 {"state", "running"},
                                 {"pending_approvals", QJsonArray{}}};
                const qint64 before = payload.value("before_cursor").toInteger();
                const int end = before > 0 ? int((before - 1) / 2) : 160;
                const int first = std::max(0, end - (before > 0 ? 6 : 3));
                page.insert("events", turnEvents(first, end - 1));
                page.insert("before_cursor", 2 * first + 1);
                page.insert("has_more_before", first > 0);
                return page;
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 6, 5000);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        daemon.push(liveEvent(id, "user.message", {{"text", "live prompt"}}, 321, 160));
        QTRY_COMPARE(desktop.messages(path).size(), 7);

        const auto state = [&] { return desktop.historyState(path); };
        const auto keyOf = [&](const QString& body) {
            for (const auto& value : desktop.messages(path))
                if (value.toMap().value("body").toString() == body)
                    return value.toMap().value("messageKey").toString();
            return QString();
        };
        // The reader stays in the older half of the window, where the byte bound
        // once evicted the newer rows, the live tail and then the reader's own.
        while (state().value("hasOlder").toBool()) {
            desktop.loadOlderMessages(path, 700, 100);
            QTRY_VERIFY_WITH_TIMEOUT(!state().value("loading").toBool(), 5000);
            QVERIFY2(state().value("error").toString().isEmpty(),
                     qPrintable(state().value("error").toString()));
            if (const auto key = keyOf("prompt-50"); !key.isEmpty())
                desktop.setTranscriptReadingKey(key);
        }
        QCOMPARE(desktop.messages(path).size(), 321);
        QTRY_VERIFY_WITH_TIMEOUT(desktop.messages(path).size() < 321, 5000);

        const auto rows = desktop.messages(path);
        qsizetype bytes = 0;
        for (const auto& value : rows)
            bytes += transcript_window::sourceBytes(value.toMap());
        QVERIFY(bytes <= transcript_window::maximumUtf8Bytes);
        QVERIFY2(!keyOf("prompt-50").isEmpty(), "the byte bound evicted the row being read");
        QCOMPARE(rows.constLast().toMap().value("body").toString(), "live prompt");
        QVERIFY(!state().value("hasNewer").toBool());
        QVERIFY(state().value("hasOlder").toBool());
    }

    // A live turn completing rebuilds the projection under the reader's window. The
    // older page is then refused as snapshot_changed and paging must continue against
    // the new head and revision, which the daemon fences exactly as the journal does.
    void olderPagingRefencesAfterLiveProjectionChange() {
        Environment env;
        QVERIFY(env.directory.isValid());
        QVERIFY(writeToken(env.directory));
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = "live-refence";
        rememberConversation(path, id);
        const auto turnEvents = [&](int first, int last) {
            QJsonArray events;
            for (int turn = first; turn <= last; ++turn) {
                events.append(windowEvent(id, "user.message",
                                          {{"text", QString("prompt-%1").arg(turn)}}, 2 * turn + 1,
                                          turn));
                events.append(
                    windowEvent(id, "item.completed",
                                {{"item", QJsonObject{{"id", QString("answer-%1").arg(turn)},
                                                      {"type", "agentMessage"},
                                                      {"phase", "final_answer"},
                                                      {"text", QString("answer-%1").arg(turn)}}}},
                                2 * turn + 2, turn));
            }
            return events;
        };
        qint64 head = 300;
        qint64 revision = 1;
        QList<QJsonObject> windowRequests;
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject& envelope) -> QJsonObject {
                if (type == "session.list")
                    return {{"transcript_window_version", 1},
                            {"transcript_window_max_turns", 20},
                            {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "ready"},
                                                                {"last_event_cursor", head}}}}};
                if (type != "session.transcript_window")
                    return {};
                const auto payload = envelope.value("payload").toObject();
                windowRequests.append(payload);
                const qint64 before = payload.value("before_cursor").toInteger();
                const qint64 snapshot =
                    before > 0 ? payload.value("snapshot_cursor").toInteger() : head;
                // LocalJournal's fence order: range first, then the projection revision.
                if (before > 0 && (snapshot > head || before > snapshot + 1))
                    return {{"ok", false},
                            {"error_code", "invalid_page"},
                            {"error_message", "Window cursor is out of range."}};
                if (before > 0 && payload.value("projection_revision").toInteger() != revision)
                    return {{"ok", false},
                            {"error_code", "snapshot_changed"},
                            {"error_message", "Transcript compaction changed this snapshot."}};
                const int end = before > 0 ? int((before - 1) / 2) : 150;
                const int first = before > 0 ? std::max(0, end - 7) : 147;
                return {{"transcript_window_version", 1},
                        {"session_id", id},
                        {"session_generation", 1},
                        {"snapshot_cursor", snapshot},
                        {"projection_revision", revision},
                        {"current_turn_sequence", 150},
                        {"state", "ready"},
                        {"pending_approvals", QJsonArray{}},
                        {"events", turnEvents(first, end - 1)},
                        {"before_cursor", 2 * first + 1},
                        {"has_more_before", first > 0}};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 6, 5000);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());

        // Turn 150 completes live; its completion rebuilds the projection.
        daemon.push(liveEvent(id, "user.message", {{"text", "live prompt"}}, 301, 150));
        daemon.push(liveEvent(id, "item.completed",
                              {{"item", QJsonObject{{"id", "live-answer"},
                                                    {"type", "agentMessage"},
                                                    {"phase", "final_answer"},
                                                    {"text", "live answer"}}}},
                              302, 150));
        QTRY_COMPARE(desktop.messages(path).size(), 8);
        head = 302;
        revision = 2;

        desktop.loadOlderMessages(path, 700, 100);
        QTRY_VERIFY_WITH_TIMEOUT(!desktop.historyState(path).value("loading").toBool(), 5000);
        QVERIFY2(desktop.historyState(path).value("error").toString().isEmpty(),
                 qPrintable(desktop.historyState(path).value("error").toString()));
        QCOMPARE(desktop.messages(path).size(), 22);
        QCOMPARE(desktop.messages(path).first().toMap().value("body").toString(), "prompt-140");
        const auto& last = windowRequests.last();
        QCOMPARE(last.value("before_cursor").toInteger(), 295);
        QCOMPARE(last.value("snapshot_cursor").toInteger(), 302);
        QCOMPARE(last.value("projection_revision").toInteger(), 2);
    }

    // T20: a legacy replay whose cursor was pruned must not be reported as restored.
    void attachCursorExpiredRequiresResyncInsteadOfSilentGap() {
        Environment env;
        QVERIFY(env.directory.isValid());
        QVERIFY(writeToken(env.directory));
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "expired");
        int attaches = 0;
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject&) -> QJsonObject {
                if (type == "session.list")
                    return {{"sessions", QJsonArray{QJsonObject{{"session_id", "expired"},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "ready"},
                                                                {"last_event_cursor", 40}}}}};
                if (type == "session.attach") {
                    ++attaches;
                    return {{"ok", false},
                            {"error_code", "cursor_expired"},
                            {"error_message", "after_cursor precedes range."},
                            {"events_pruned", true},
                            {"earliest_available_cursor", 25},
                            {"last_event_cursor", 40}};
                }
                return {};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        QSignalSpy failed(&server, &CodexAppServer::daemonReplayFailed);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
        QVERIFY(failed.first().at(2).toBool());
        QTRY_VERIFY(!server.isRestoring());
        const auto state = desktop.historyState(path);
        QVERIFY(state.value("resyncRequired").toBool());
        QVERIFY(!state.value("error").toString().isEmpty());
        QVERIFY(!state.value("loading").toBool());
        QTest::qWait(100);
        QCOMPARE(attaches, 1); // No retry loop against the stale cursor.
        QVERIFY(!server.isDaemonSessionRestoring("expired"));
    }

    void attachFailureIsReportedWithoutResync() {
        Environment env;
        QVERIFY(env.directory.isValid());
        QVERIFY(writeToken(env.directory));
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        rememberConversation(path, "broken");
        FakeDaemon daemon(env.directory.filePath("daemon.sock"),
                          [&](const QString& type, const QJsonObject&) -> QJsonObject {
                              if (type == "session.list")
                                  return {
                                      {"sessions", QJsonArray{QJsonObject{{"session_id", "broken"},
                                                                          {"worktree_path", path},
                                                                          {"session_generation", 1},
                                                                          {"state", "ready"}}}}};
                              if (type == "session.attach")
                                  return {{"ok", false},
                                          {"error_code", "journal_error"},
                                          {"error_message", "disk unavailable"}};
                              return {};
                          });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        QSignalSpy failed(&server, &CodexAppServer::daemonReplayFailed);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
        QVERIFY(!failed.first().at(2).toBool());
        QTRY_COMPARE(desktop.historyState(path).value("error").toString(), "disk unavailable");
        QVERIFY(!desktop.historyState(path).value("resyncRequired").toBool());
    }

    // T7, T8 and T6 against a scripted transcript-window daemon.
    void liveEventsReuseLoadedRowsAndCanonicalKeys() {
        Environment env;
        QVERIFY(env.directory.isValid());
        QVERIFY(writeToken(env.directory));
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = "windowed";
        rememberConversation(path, id);
        const QJsonObject oldAnswer{
            {"id", "a"}, {"type", "agentMessage"}, {"phase", "final_answer"}, {"text", "old"}};
        const QJsonArray events{windowEvent(id, "user.message", {{"text", "first"}}, 1, 1),
                                windowEvent(id, "item.completed", {{"item", oldAnswer}}, 2, 1),
                                windowEvent(id, "user.message", {{"text", "second"}}, 3, 2),
                                windowEvent(id, "item.completed",
                                            {{"item", QJsonObject{{"id", "b"},
                                                                  {"type", "agentMessage"},
                                                                  {"phase", "final_answer"},
                                                                  {"text", "newer"}}}},
                                            4, 2)};
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject&) -> QJsonObject {
                if (type == "session.list")
                    return {{"transcript_window_version", 1},
                            {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "ready"},
                                                                {"last_event_cursor", 4}}}}};
                if (type == "session.transcript_window")
                    return {{"transcript_window_version", 1},
                            {"session_id", id},
                            {"session_generation", 1},
                            {"events", events},
                            {"before_cursor", 1},
                            {"has_more_before", false},
                            {"snapshot_cursor", 4},
                            {"projection_revision", 1},
                            {"current_turn_sequence", 2},
                            {"state", "ready"},
                            {"pending_approvals", QJsonArray{}}};
                if (type == "session.start_turn")
                    return {{"session_id", id}, {"session_generation", 1}};
                return {};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 4, 5000);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());
        QSignalSpy reset(&desktop, &AgentSessionManager::messagesReset);

        // T7: a late update for turn 1 edits the loaded row instead of appending a copy.
        auto updated = oldAnswer;
        updated.insert("text", "updated");
        daemon.push(liveEvent(id, "item.completed", {{"item", updated}}, 5, 1));
        QTRY_COMPARE(desktop.messages(path).at(1).toMap().value("body").toString(), "updated");
        QCOMPARE(desktop.messages(path).size(), 4);

        // T7: with the newer turn current again, a late update for the turn-1 row that
        // arrives without its own turn scope still edits that row in place.
        daemon.push(liveEvent(id, "item.completed",
                              {{"item", QJsonObject{{"id", "b"},
                                                    {"type", "agentMessage"},
                                                    {"phase", "final_answer"},
                                                    {"text", "newer again"}}}},
                              6, 2));
        QTRY_COMPARE(desktop.messages(path).at(3).toMap().value("body").toString(), "newer again");
        auto unscoped = liveEvent(id, "item.completed", {}, 7, 0);
        unscoped.remove("turn_sequence");
        auto late = oldAnswer;
        late.insert("text", "late");
        unscoped.insert("payload", QJsonObject{{"event_type", "item.completed"},
                                               {"data", QJsonObject{{"item", late}}}});
        daemon.push(unscoped);
        QTRY_COMPARE(desktop.messages(path).at(1).toMap().value("body").toString(), "late");
        QCOMPARE(desktop.messages(path).size(), 4);

        // T8: a live echo and the same event replayed from a window share one key.
        daemon.push(liveEvent(id, "user.message", {{"text", "from mobile"}}, 8, 3));
        QTRY_COMPARE(desktop.messages(path).size(), 5);
        const auto replayed = daemonWindowMessages(
            {{"events",
              QJsonArray{transcriptEvent("user.message", {{"text", "from mobile"}}, 8, 3)}}});
        QCOMPARE(replayed.size(), 1);
        QCOMPARE(desktop.messages(path).last().toMap().value("messageKey").toString(),
                 replayed.first().toMap().value("messageKey").toString());
        QCOMPARE(replayed.first().toMap().value("messageKey").toString(),
                 daemonMessageKey(3, daemonUserItemId(8)));

        // T6: an id-less echo (another client) never claims a local prompt with the same text.
        desktop.submitPrompt(
            path, "same text",
            {{"provider", "codex"}, {"model", "gpt-5.6-luna"}, {"effort", "medium"}});
        QTRY_VERIFY_WITH_TIMEOUT(daemon.received.contains("session.start_turn"), 5000);
        QString clientMessageId;
        for (const auto& envelope : daemon.envelopes)
            if (envelope.value("message_type") == "session.start_turn")
                clientMessageId =
                    envelope.value("payload").toObject().value("client_message_id").toString();
        QVERIFY(!clientMessageId.isEmpty());
        const auto sizeWithLocal = desktop.messages(path).size();
        daemon.push(liveEvent(id, "user.message", {{"text", "same text"}}, 9, 4));
        QTRY_COMPARE(desktop.messages(path).size(), sizeWithLocal + 1);
        const auto remote = desktop.messages(path).last().toMap();
        QCOMPARE(remote.value("itemId").toString(), daemonUserItemId(9));
        QVERIFY(remote.value("clientMessageId").toString().isEmpty());
        // The echo carrying the local id acknowledges the local row in place.
        daemon.push(liveEvent(id, "user.message",
                              {{"text", "same text"}, {"client_message_id", clientMessageId}}, 10,
                              5));
        QTRY_VERIFY([&] {
            for (const auto& value : desktop.messages(path)) {
                const auto row = value.toMap();
                if (row.value("clientMessageId") == clientMessageId)
                    return row.value("itemId").toString() == daemonUserItemId(10);
            }
            return false;
        }());
        QCOMPARE(desktop.messages(path).size(), sizeWithLocal + 1);
        QCOMPARE(reset.size(), 0);
    }

    // Live transcript events reach only the daemon session that holds the restore
    // priority. Starting a conversation elsewhere takes that priority away, and the
    // conversation on screen used to freeze silently until it was selected again.
    void liveEventsSurviveRestorePriorityMovingElsewhere() {
        Environment env;
        QVERIFY(env.directory.isValid());
        QVERIFY(writeToken(env.directory));
        qputenv("KETPLUS_DISABLE_DAEMON_AUTOSTART", "1");
        const QString path = QFileInfo(env.directory.path()).canonicalFilePath();
        const QString id = "windowed";
        rememberConversation(path, id);
        QJsonArray events{windowEvent(id, "user.message", {{"text", "first"}}, 1, 1)};
        FakeDaemon daemon(
            env.directory.filePath("daemon.sock"),
            [&](const QString& type, const QJsonObject&) -> QJsonObject {
                if (type == "session.list")
                    return {{"transcript_window_version", 1},
                            {"sessions", QJsonArray{QJsonObject{{"session_id", id},
                                                                {"worktree_path", path},
                                                                {"session_generation", 1},
                                                                {"state", "ready"},
                                                                {"last_event_cursor", 1}}}}};
                if (type == "session.transcript_window")
                    return {
                        {"transcript_window_version", 1},
                        {"session_id", id},
                        {"session_generation", 1},
                        {"events", events},
                        {"before_cursor", 1},
                        {"has_more_before", false},
                        {"snapshot_cursor", events.last().toObject().value("cursor").toInteger()},
                        {"projection_revision", 1},
                        {"current_turn_sequence", 1},
                        {"state", "ready"},
                        {"pending_approvals", QJsonArray{}}};
                return {};
            });
        QVERIFY(daemon.listening);
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 1, 5000);
        QTRY_VERIFY(!desktop.historyState(path).value("loading").toBool());

        // Starting a conversation in another worktree leaves this one on screen.
        server.prioritizeDaemonRestore(env.directory.filePath("elsewhere"));

        // The reply the daemon streams for the visible conversation must still arrive.
        const QJsonObject answer{
            {"id", "a"}, {"type", "agentMessage"}, {"phase", "final_answer"}, {"text", "streamed"}};
        events.append(windowEvent(id, "item.completed", {{"item", answer}}, 2, 1));
        daemon.push(liveEvent(id, "item.completed", {{"item", answer}}, 2, 1));
        QTRY_COMPARE_WITH_TIMEOUT(desktop.messages(path).size(), 2, 5000);
        QCOMPARE(desktop.messages(path).last().toMap().value("body").toString(), "streamed");
    }
};

QTEST_GUILESS_MAIN(DesktopTranscriptWindowTest)
#include "DesktopTranscriptWindowTest.moc"
