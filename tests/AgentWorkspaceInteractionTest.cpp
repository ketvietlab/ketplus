#include "AgentWorkspaceViewTest.h"
#include "CodexBrowserTools.h"
#include "KetAppearance.h"
#include "KetRecipes.h"
#include "KetTokens.h"
#include "agent/AgentMessageModel.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include "agent/DaemonAgentNotes.h"
#include "agent/FileMentions.h"
#include "agent/MermaidLabelContrast.h"
#include "agent/ProviderRuntimeStatus.h"
#include "agent/TranscriptFileLinks.h"
#include "ui/Theme.h"
#include <QQmlComponent>
#include <QScopeGuard>

#include <QAbstractItemModelTester>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QtTest>

#include <cmath>

#include <algorithm>

using namespace agent_workspace_test;

void AgentWorkspaceViewTest::compactionLifecycleAndCommand() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto serverPath = directory.filePath("fake-codex");
    QFile server(serverPath);
    QVERIFY(server.open(QIODevice::WriteOnly));
    const QByteArray source = R"PY(#!/usr/bin/python3
import json, sys
def send(value):
    print(json.dumps(value), flush=True)
compacts = 0
for line in sys.stdin:
    request = json.loads(line)
    method, rid = request.get("method"), request.get("id")
    if method == "initialized": continue
    if method == "initialize": send({"id":rid,"result":{}})
    elif method == "model/list": send({"id":rid,"result":{"data":[]}})
    elif method == "thread/start":
        send({"id":rid,"result":{"thread":{"id":"compact-thread","turns":[]}}})
    elif method == "turn/start":
        assert request["params"]["input"][0]["text"] != "/compact"
        send({"id":rid,"result":{"turn":{"id":"initial"}}})
        send({"method":"turn/completed","params":{"threadId":"compact-thread","turn":{"id":"initial","status":"completed"}}})
    elif method == "thread/compact/start":
        compacts += 1
        assert request["params"] == {"threadId":"compact-thread"}
        if compacts == 2:
            send({"id":rid,"error":{"code":-32601,"message":"Method not found"}})
            continue
        send({"id":rid,"result":{}})
        send({"method":"turn/started","params":{"threadId":"compact-thread","turn":{"id":"manual"}}})
        send({"method":"item/started","params":{"threadId":"compact-thread","turnId":"manual","item":{"id":"c1","type":"contextCompaction"}}})
    elif method == "thread/resume":
        send({"id":rid,"result":{"thread":{"id":"compact-thread","turns":[{"id":"old","status":"completed","items":[{"id":"old-c","type":"contextCompaction"}]}]}}})
    else:
        send({"id":rid,"error":{"code":-32601,"message":"Unexpected RPC: " + method}})
)PY";
    QCOMPARE(server.write(source), source.size());
    server.close();
    QVERIFY(QFile::setPermissions(serverPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
    ketplus::CodexAppServer appServer(serverPath);
    ketplus::AgentSessionManager manager(&appServer);
    QSignalSpy errors(&manager, &ketplus::AgentSessionManager::errorOccurred);
    const auto path = directory.path();
    manager.submitPrompt(path, "/compact");
    QCOMPARE(errors.count(), 1);
    QVERIFY(manager.messages(path).isEmpty());
    manager.submitPrompt(path, "Existing history");
    QTRY_VERIFY(!manager.isRunning(path));
    QCOMPARE(manager.messages(path).size(), 1);

    ketplus::AgentWorkspaceView view;
    view.setAgentAvailable(true);
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    view.resetMessages(manager.messages(path));
    connect(&manager, &ketplus::AgentSessionManager::messageAppended, &view,
            [&](const QString&, int, const QVariantMap& item) { view.appendMessage(item); });
    connect(&manager, &ketplus::AgentSessionManager::messageUpdated, &view,
            [&](const QString&, int index, const QVariantMap& item) {
                view.updateMessage(index, item);
            });
    connect(
        &view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted, &manager,
        [&](const QString& prompt, const QVariantMap& options, const QVariantList& attachments) {
            manager.submitPrompt(path, prompt, options, attachments);
        });
    connect(&manager, &ketplus::AgentSessionManager::sessionStateChanged, &view,
            [&](const QString&, const QString& title, const QString& status, bool running) {
                view.setSessionState(title, status, running);
            });
    auto* root = view.quickView()->rootObject();
    auto* prompt = root->findChild<QQuickItem*>("agentPrompt");
    QVERIFY(prompt);
    prompt->setProperty("text", "/compact");
    QVERIFY(QMetaObject::invokeMethod(root, "submitCurrentPrompt"));
    QTRY_COMPARE(manager.status(path), QStringLiteral("Compacting context…"));
    QCOMPARE(view.sessionStatus(), QStringLiteral("Compacting context…"));
    const auto capture = qEnvironmentVariable("KETPLUS_COMPACTION_CAPTURE");
    if (!capture.isEmpty()) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        QTest::qWait(150);
        QVERIFY(view.quickView()->grabWindow().save(capture + ".compacting.png"));
    }
    QCOMPARE(prompt->property("text").toString(), QString());
    QCOMPARE(manager.messages(path).size(), 2);
    QCOMPARE(manager.messages(path).last().toMap().value("subtype").toString(),
             QStringLiteral("compaction"));

    const auto itemEvent = [&](const QString& method, const QString& turn, const QString& id,
                               const QString& type) {
        emit appServer.notificationReceived(method,
                                            {{"threadId", "compact-thread"},
                                             {"turnId", turn},
                                             {"item", QJsonObject{{"id", id}, {"type", type}}}});
    };
    itemEvent("item/completed", "manual", "reason", "reasoning");
    QCOMPARE(manager.status(path), QStringLiteral("Compacting context…"));
    // The reasoning reads as a "Thought" step after the compaction row.
    QCOMPARE(manager.messages(path).size(), 3);
    QCOMPARE(manager.messages(path).last().toMap().value("title").toString(),
             QStringLiteral("Thought"));
    const auto compactionRow = [&] { return manager.messages(path).at(1).toMap(); };
    itemEvent("item/completed", "stale-turn", "c1", "contextCompaction");
    QCOMPARE(manager.status(path), QStringLiteral("Compacting context…"));
    manager.steerPrompt(path, "/compact");
    QCOMPARE(errors.count(), 2);
    const auto original = manager.activeConversationId(path);
    manager.newSession(path);
    QVERIFY(!manager.isRunning(path));
    itemEvent("item/completed", "manual", "c1", "contextCompaction");
    QCOMPARE(manager.status(path), QStringLiteral("Ready"));
    manager.selectConversation(path, original);
    QCOMPARE(compactionRow().value("title").toString(), QStringLiteral("Context compacted"));
    QCOMPARE(compactionRow().value("body").toString(),
             QStringLiteral("Older steps are summarised; the conversation continues."));
    QVERIFY(manager.isRunning(path));
    itemEvent("item/started", "manual", "c1", "contextCompaction");
    QCOMPARE(manager.messages(path).size(), 3);
    QCOMPARE(manager.status(path), QStringLiteral("Working…"));
    if (!capture.isEmpty()) {
        view.resetMessages(manager.messages(path));
        QTest::qWait(150);
        QVERIFY(view.quickView()->grabWindow().save(capture + ".compacted.png"));
    }
    emit appServer.notificationReceived(
        "turn/completed", {{"threadId", "compact-thread"},
                           {"turn", QJsonObject{{"id", "manual"}, {"status", "completed"}}}});
    QVERIFY(!manager.isRunning(path));
    itemEvent("item/started", "manual", "late", "contextCompaction");
    QCOMPARE(manager.messages(path).size(), 3);

    manager.submitPrompt(path, "/compact");
    QTRY_COMPARE(manager.status(path), QStringLiteral("Error"));
    QVERIFY(!manager.isRunning(path));
    QVERIFY(manager.messages(path).last().toMap().value("body").toString().contains(
        "Method not found"));

    emit appServer.notificationReceived(
        "turn/started",
        {{"threadId", "compact-thread"}, {"turn", QJsonObject{{"id", "automatic"}}}});
    itemEvent("item/started", "automatic", "c1", "contextCompaction");
    QCOMPARE(manager.status(path), QStringLiteral("Compacting context…"));
    emit appServer.notificationReceived(
        "turn/completed", {{"threadId", "compact-thread"},
                           {"turn", QJsonObject{{"id", "automatic"}, {"status", "interrupted"}}}});
    QCOMPARE(manager.status(path), QStringLiteral("Stopped"));
    QVERIFY(!manager.messages(path).last().toMap().value("running").toBool());
    QVERIFY(manager.messages(path).last().toMap().value("title").toString() != "Context compacted");

    ketplus::CodexAppServer restoredServer(serverPath);
    ketplus::AgentSessionManager restored(&restoredServer);
    restored.activate(path);
    QTRY_COMPARE(restored.messages(path).size(), 1);
    QCOMPARE(restored.messages(path).first().toMap().value("title").toString(),
             QStringLiteral("Context compacted"));
}

void AgentWorkspaceViewTest::directModeAutoApprovesOnlyInjectedBrowserTools() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString serverPath = directory.filePath("fake-codex-elicitation");
    const QString logPath = directory.filePath("responses.log");
    QFile server(serverPath);
    QVERIFY(server.open(QIODevice::WriteOnly));
    QByteArray source = R"PY(#!/usr/bin/python3
import json, sys
LOG = @LOG@
def send(value):
    print(json.dumps(value), flush=True)
tagged = {"threadId": "t", "serverName": "ketplus_browser", "mode": "form",
          "_meta": {"codex_approval_kind": "mcp_tool_call"},
          "requestedSchema": {"type": "object", "properties": {}}}
untagged = dict(tagged)
del untagged["_meta"]
names = {"r-tagged": "tagged", "r-untagged": "untagged", "r-permissions": "permissions"}
for line in sys.stdin:
    message = json.loads(line)
    method, rid = message.get("method"), message.get("id")
    if method == "initialize":
        send({"id": rid, "result": {}})
    elif method == "initialized":
        send({"id": "r-tagged", "method": "mcpServer/elicitation/request", "params": tagged})
        send({"id": "r-untagged", "method": "mcpServer/elicitation/request", "params": untagged})
        send({"id": "r-permissions", "method": "item/permissions/requestApproval",
              "params": {"threadId": "t", "turnId": "u", "itemId": "p"}})
    elif method is None and rid in names:
        with open(LOG, "a") as log:
            log.write(names[rid] + ":" + json.dumps(message.get("result"), sort_keys=True) + "\n")
    elif rid is not None:
        send({"id": rid, "result": {}})
)PY";
    source.replace(
        "@LOG@",
        QJsonDocument(QJsonArray{logPath}).toJson(QJsonDocument::Compact).mid(1).chopped(1));
    server.write(source);
    server.close();
    QVERIFY(QFile::setPermissions(serverPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
    const auto logText = [&logPath] {
        QFile log(logPath);
        return log.open(QIODevice::ReadOnly) ? log.readAll() : QByteArray();
    };

    for (const bool injected : {true, false}) {
        QFile::remove(logPath);
        ketplus::CodexAppServer codex(serverPath);
        codex.setBrowserArgumentsProvider([injected] {
            return injected
                       ? QStringList{QStringLiteral("-c"),
                                     QStringLiteral("mcp_servers.ketplus_browser.command=\"node\"")}
                       : QStringList{};
        });
        codex.call(QStringLiteral("thread/list"), {}, [](const QJsonObject&, const QString&) {});
        QTRY_VERIFY_WITH_TIMEOUT(logText().count('\n') == 3, 5000);
        const QByteArray log = logText();
        QVERIFY2(log.contains(injected ? R"(tagged:{"action": "accept", "content": {}})"
                                       : R"(tagged:{"action": "decline"})"),
                 log.constData());
        QVERIFY2(log.contains(R"(untagged:{"action": "decline"})"), log.constData());
        QVERIFY2(log.contains(R"(permissions:{"permissions": {}})"), log.constData());
        QCOMPARE(codex.launchedBrowserArguments().isEmpty(), !injected);
        codex.stop();
    }
}

void AgentWorkspaceViewTest::answersAgentQuestionsSequentially() {
    const auto previousMode = ket::Appearance::instance().mode();
    const auto restoreTheme =
        qScopeGuard([previousMode] { ket::Appearance::instance().setMode(previousMode); });
    ket::Appearance::instance().setMode(ket::Appearance::Mode::Dark);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto serverPath = directory.filePath("fake-codex-question");
    QFile server(serverPath);
    QVERIFY(server.open(QIODevice::WriteOnly));
    const QByteArray source = R"PY(#!/usr/bin/python3
import json, sys
def send(value):
    print(json.dumps(value), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    method, rid = request.get("method"), request.get("id")
    if method == "initialized": continue
    if method == "initialize": send({"id":rid,"result":{}})
    elif method == "thread/start":
        send({"id":rid,"result":{"thread":{"id":"question-thread","turns":[]}}})
    elif method == "turn/start":
        send({"id":rid,"result":{"turn":{"id":"question-turn"}}})
        send({"method":"turn/started","params":{"threadId":"question-thread","turn":{"id":"question-turn"}}})
        send({"id":"invalid-question-request","method":"item/tool/requestUserInput","params":{
            "threadId":"question-thread","turnId":"question-turn","itemId":"invalid-question-item",
            "questions":[{"id":"q" + str(index),"question":"Question","options":[{"label":"One"}]}
                         for index in range(5)]}})
    elif rid == "invalid-question-request":
        assert request["error"]["code"] == -32602, request
        assert request["error"]["data"]["code"] == "invalid_question_request", request
        send({"id":"invalid-option-request","method":"item/tool/requestUserInput","params":{
            "threadId":"question-thread","turnId":"question-turn","itemId":"invalid-option-item",
            "questions":[{"id":"invalid-option","question":"Question","options":[
                {"id":"empty-label","label":""},{"id":"valid","label":"Valid"}]}]}})
    elif rid == "invalid-option-request":
        assert request["error"]["code"] == -32602, request
        assert request["error"]["data"]["code"] == "invalid_question_request", request
        send({"id":"question-request","method":"item/tool/requestUserInput","params":{
            "threadId":"question-thread","turnId":"question-turn","itemId":"question-item",
            "isBlocking":True,"questions":[
                {"id":"placement","header":"Placement","question":"Where should it appear?","options":[
                    {"id":"timeline-primary","label":"In timeline","description":"Keep the question in context."},
                    {"id":"timeline-secondary","label":"In timeline","description":"Use the alternate placement with the same label."}]},
                {"id":"behavior","header":"After submit","question":"How should it finish?","isOther":True,"options":[
                    {"label":"Collapse","description":"Keep a compact receipt."}]}
            ]}})
    elif rid == "question-request":
        answers = request["result"]["answers"]
        assert answers == {
            "placement":{"answers":["In timeline"]},
            "behavior":{"answers":["Keep the compact receipt"]}
        }, answers
        item = {"id":"answer-confirmation","type":"agentMessage","text":"Answers received"}
        send({"method":"item/started","params":{"threadId":"question-thread","turnId":"question-turn","item":item}})
        send({"method":"item/completed","params":{"threadId":"question-thread","turnId":"question-turn","item":item}})
        send({"method":"turn/completed","params":{"threadId":"question-thread","turn":{"id":"question-turn","status":"completed"}}})
    else:
        send({"id":rid,"error":{"code":-32601,"message":"Unexpected RPC: " + str(method)}})
)PY";
    QCOMPARE(server.write(source), source.size());
    server.close();
    QVERIFY(QFile::setPermissions(serverPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));

    ketplus::CodexAppServer appServer(serverPath);
    ketplus::AgentSessionManager manager(&appServer);
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 720);
    view.show();
    const QString path = directory.path();
    connect(&manager, &ketplus::AgentSessionManager::messageAppended, &view,
            [&](const QString&, int, const QVariantMap& message) { view.appendMessage(message); });
    connect(&manager, &ketplus::AgentSessionManager::messageUpdated, &view,
            [&](const QString&, int index, const QVariantMap& message) {
                view.updateMessage(index, message);
            });
    connect(&view, &ketplus::AgentWorkspaceView::questionAnswered, &manager,
            [&](const QString& messageKey, const QVariantMap& answers) {
                manager.answerQuestion(path, messageKey, answers);
            });

    manager.submitPrompt(path, QStringLiteral("Ask me before implementing"));
    auto* root = view.quickView()->rootObject();
    QQuickItem* card = nullptr;
    QTRY_VERIFY((card = findVisualItem(root, QStringLiteral("messageQuestionCard"))));
    QTRY_COMPARE(card->property("currentIndex").toInt(), 0);
    QCOMPARE(manager.status(path), QStringLiteral("Waiting for your answer…"));
    const QString capturePath = qEnvironmentVariable("KETPLUS_QUESTION_CAPTURE");
    if (!capturePath.isEmpty()) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        QTest::qWait(100);
        QVERIFY(view.quickView()->grabWindow().save(capturePath + ".first.png"));
    }

    auto* continueButton = findVisualItem(card, QStringLiteral("questionContinueButton"));
    QVERIFY(continueButton);
    QVERIFY(!continueButton->isEnabled());
    const QVariant duplicateLabelOptionId = QStringLiteral("timeline-secondary");
    QVERIFY(
        QMetaObject::invokeMethod(card, "selectOption", Q_ARG(QVariant, duplicateLabelOptionId)));
    QTRY_VERIFY(continueButton->isEnabled());
    QCOMPARE(card->property("currentAnswers").toList(),
             QVariantList{QStringLiteral("timeline-secondary")});
    auto* questionTranscript = findVisualItem(root, QStringLiteral("agentTranscript"));
    QVERIFY(questionTranscript);
    QTRY_COMPARE(questionTranscript->opacity(), 1.0);
    QTest::qWait(60);
    QTest::mouseClick(
        view.quickView(), Qt::LeftButton, Qt::NoModifier,
        continueButton->mapToScene(continueButton->boundingRect().center()).toPoint());
    QTRY_COMPARE(card->property("currentIndex").toInt(), 1);
    QVERIFY(findVisualItem(card, QStringLiteral("questionBackButton"))->isVisible());
    if (!capturePath.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(view.quickView()->grabWindow().save(capturePath + ".second.png"));
    }

    auto* otherOption = findVisualItem(card, QStringLiteral("questionOtherOption"));
    QVERIFY(otherOption);
    QVERIFY(otherOption->isVisible());
    QVERIFY(otherOption->width() > 0);
    QVERIFY(otherOption->height() > 0);
    QVERIFY(QMetaObject::invokeMethod(card, "selectOther"));
    QTRY_VERIFY(card->property("currentUsesOther").toBool());
    auto* otherInput = findVisualItem(card, QStringLiteral("questionOtherInput"));
    QVERIFY(otherInput);
    otherInput->setProperty("text", QStringLiteral("Keep the compact receipt"));
    QTRY_VERIFY(continueButton->isEnabled());
    QTest::mouseClick(
        view.quickView(), Qt::LeftButton, Qt::NoModifier,
        continueButton->mapToScene(continueButton->boundingRect().center()).toPoint());

    QTRY_VERIFY(!manager.isRunning(path));
    const QVariantList messages = manager.messages(path);
    QCOMPARE(messages.size(), 3);
    const QVariantMap question = messages.at(1).toMap();
    QCOMPARE(question.value("subtype").toString(), QStringLiteral("question"));
    QVERIFY(!question.value("running").toBool());
    QCOMPARE(question.value("questionAnswers").toMap().value("placement").toString(),
             QStringLiteral("In timeline"));
    QCOMPARE(question.value("questionAnswers").toMap().value("behavior").toString(),
             QStringLiteral("Keep the compact receipt"));
    QCOMPARE(messages.last().toMap().value("body").toString(), QStringLiteral("Answers received"));
    // Answered, the card folds into the mock's note: what was asked and what was chosen.
    QQuickItem* answeredNote = nullptr;
    QTRY_VERIFY((answeredNote = findVisualItem(root, QStringLiteral("closedQuestionNote"))));
    QVERIFY(!findVisualItem(root, QStringLiteral("messageQuestionCard")));
    QCOMPARE(answeredNote->property("label").toString(), QStringLiteral("Answered"));
    QVERIFY2(answeredNote->property("details").toString().contains(QStringLiteral("In timeline")),
             qPrintable(answeredNote->property("details").toString()));
    QVERIFY(answeredNote->property("details").toString().contains(
        QStringLiteral("Keep the compact receipt")));
    if (!capturePath.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(view.quickView()->grabWindow().save(capturePath + ".answered.png"));
    }
    // A remote resolution/expiry closes the local card without inventing an answer
    // or permitting a second submission to the provider.
    for (const bool expired : {false, true}) {
        const QString id = expired ? "expired-question" : "remote-question";
        appServer.userInputRequested(
            id,
            {{"threadId", "question-thread"},
             {"itemId", id},
             {"questions",
              QJsonArray{QJsonObject{{"id", "decision"},
                                     {"question", "Proceed?"},
                                     {"options", QJsonArray{QJsonObject{{"label", "Accept"}}}}}}}});
        auto pending = manager.messages(path).last().toMap();
        QVERIFY(pending.value("running").toBool());
        const int visibleBefore = view.messageModel()->count();
        appServer.notificationReceived(
            "userInput/resolved",
            {{"threadId", "question-thread"}, {"requestId", id}, {"expired", expired}});
        const auto closed = manager.messages(path).last().toMap();
        QVERIFY(!closed.value("running").toBool());
        QCOMPARE(closed.value("status").toString(), expired ? "expired" : "resolved");
        QVERIFY(closed.value("questionAnswers").toMap().isEmpty());
        QTRY_COMPARE(view.messageModel()->count(), visibleBefore - (expired ? 1 : 0));
        QSignalSpy errors(&manager, &ketplus::AgentSessionManager::errorOccurred);
        manager.answerQuestion(path, closed.value("messageKey").toString(),
                               {{"decision", "Accept"}});
        QCOMPARE(errors.size(), 1);
    }
    // An approval names what it asked to use, and another client's decision says how it ended.
    appServer.userInputRequested(
        "remote-approval",
        {{"threadId", "question-thread"},
         {"itemId", "remote-approval"},
         {"approvalKind", "approval"},
         {"approvalReason", "Codex wants network access."},
         {"approvalCommand", "curl https://example.test"},
         {"approvalTool", "Command"},
         {"questions",
          QJsonArray{QJsonObject{{"id", "decision"},
                                 {"question", "Allow?"},
                                 {"options", QJsonArray{QJsonObject{{"label", "Accept"}},
                                                        QJsonObject{{"label", "Decline"}}}}}}}});
    QCOMPARE(manager.messages(path).last().toMap().value("target").toString(), "Command");
    appServer.notificationReceived("userInput/resolved", {{"threadId", "question-thread"},
                                                          {"requestId", "remote-approval"},
                                                          {"expired", false},
                                                          {"outcome", "denied"}});
    QCOMPARE(manager.messages(path).last().toMap().value("status").toString(), "denied");
    // A sent agent message is one live note: its expiry and its reply update it in place.
    const auto noteRows = [&] {
        QVariantList notes;
        for (const auto& row : manager.messages(path))
            if (row.toMap().value("subtype") == "agentNote")
                notes.append(row);
        return notes;
    };
    const auto notice = [&](const QString& itemId, const QJsonObject& data) {
        appServer.notificationReceived(
            "daemon/providerNotice",
            {{"threadId", "question-thread"},
             {"itemId", itemId},
             {"presentation",
              QJsonObject::fromVariantMap(ketplus::agent_message::agentNote(data, itemId))}});
    };
    notice("notice:sent", {{"message_id", "live-q"},
                           {"kind", "request"},
                           {"state", "sent"},
                           {"expect_reply", true},
                           {"direction", "outgoing"},
                           {"peer_number", "8941"},
                           {"peer_title", "DS Icon"},
                           {"peer_provider", "claude"},
                           {"text", "Is it fine?"}});
    QCOMPARE(noteRows().size(), 1);
    QCOMPARE(noteRows().first().toMap().value("meta").toString(),
             "DS Icon · Claude · Sent · reply requested");
    notice("notice:held", {{"message_id", "live-q"},
                           {"kind", "request"},
                           {"state", "held"},
                           {"direction", "outgoing"},
                           {"peer_number", "8941"}});
    QCOMPARE(noteRows().size(), 1);
    QCOMPARE(noteRows().first().toMap().value("meta").toString(),
             "DS Icon · Claude · Held · waiting for approval there");
    appServer.notificationReceived(
        "daemon/userMessage", {{"threadId", "question-thread"},
                               {"itemId", "user:live-reply"},
                               {"text", "Yes"},
                               {"agentMessages", QJsonArray{QJsonObject{{"message_id", "live-r"},
                                                                        {"kind", "reply"},
                                                                        {"reply_to", "live-q"},
                                                                        {"from_number", "8941"},
                                                                        {"body", "Yes"}}}}});
    QCOMPARE(noteRows().size(), 1);
    QCOMPARE(noteRows().first().toMap().value("meta").toString(), "DS Icon · Claude · Replied");
    QCOMPARE(noteRows().first().toMap().value("body").toString(), "Is it fine?");
}

void AgentWorkspaceViewTest::sidebarDividerRemainsResizable() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* handle = root->findChild<QQuickItem*>("sidebarResizeHandle");
    auto* divider = root->findChild<QQuickItem*>("sidebarDivider");
    auto* sidebar = root->findChild<QQuickItem*>("conversationSidebar");
    QVERIFY(handle);
    QVERIFY(divider);
    QVERIFY(sidebar);
    // The divider takes one pixel of layout, as the mock; the grab area stays 6 px.
    QTRY_COMPARE(handle->width(), 1.0);
    QCOMPARE(divider->width(), 1.0);
    QTRY_VERIFY(handle->height() > 0);
    QCOMPARE(divider->height(), handle->height());
    QTRY_VERIFY(handle->contains(QPointF(3, 100)));
    QVERIFY(handle->contains(QPointF(-2, 100)));
    QVERIFY(!handle->contains(QPointF(5, 100)));
    const auto start = handle->mapToScene(QPointF(3, 100)).toPoint();
    const auto originalWidth = sidebar->width();
    QTest::mousePress(view.quickView(), Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(view.quickView(), start + QPoint(80, 0), 30);
    QTest::mouseRelease(view.quickView(), Qt::LeftButton, Qt::NoModifier, start + QPoint(80, 0));
    QTRY_VERIFY(sidebar->width() > originalWidth + 40);
    QCOMPARE(divider->width(), 1.0);
}

void AgentWorkspaceViewTest::sidebarMatchesMockAtEveryZoom() {
    auto& appearance = ket::Appearance::instance();
    const qreal originalZoom = appearance.zoom();
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1440, 900);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* sidebar = root->findChild<QQuickItem*>("conversationSidebar");
    auto* indicator = root->findChild<QQuickItem*>("workingAnimationIcon");
    QVERIFY(sidebar);
    QVERIFY(indicator);
    // The mock's sidebar recipe: 240 px scaled by interface zoom.
    for (const qreal zoom : {1.0, 1.25}) {
        appearance.setZoom(zoom);
        QTRY_COMPARE(sidebar->width(), ket::Appearance::scale(240, zoom));
    }
    // A dragged width survives zoom changes in both directions.
    appearance.setZoom(1.0);
    QTRY_COMPARE(sidebar->width(), 240.0);
    auto* handle = root->findChild<QQuickItem*>("sidebarResizeHandle");
    QVERIFY(handle);
    const auto start = handle->mapToScene(QPointF(3, 100)).toPoint();
    QTest::mousePress(view.quickView(), Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(view.quickView(), start + QPoint(70, 0), 30);
    QTest::mouseRelease(view.quickView(), Qt::LeftButton, Qt::NoModifier, start + QPoint(70, 0));
    QTRY_VERIFY(sidebar->width() > 290);
    const qreal dragged = sidebar->width();
    appearance.setZoom(1.25);
    QTRY_COMPARE(sidebar->width(), std::min(ket::Appearance::scale(dragged, 1.25),
                                            ket::Appearance::scale(320, 1.25)));
    appearance.setZoom(1.0);
    QTRY_COMPARE(sidebar->width(), dragged);
    appearance.setZoom(originalZoom);
    // The working bars keep the recipe's success colour, as in the mock.
    QCOMPARE(indicator->property("color").value<QColor>(), QColor(Qt::transparent));
    QStringList icons;
    for (const auto* id : {"workspace", "explorer", "source-control", "connections"}) {
        auto* item = findVisualItem(root, QStringLiteral("productNavigation_") + id);
        QVERIFY(item);
        icons << item->property("iconName").toString();
    }
    QCOMPARE(icons, (QStringList{"chats", "folder", "branch", "network"}));
}

void AgentWorkspaceViewTest::preservesQuestionDraftsAcrossScope() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);

    root->setProperty("transcriptScope", QStringLiteral("scope-a"));
    const QVariantMap answers{
        {QStringLiteral("detail"),
         QVariantMap{{QStringLiteral("option_ids"), QStringList{QStringLiteral("brief")}},
                     {QStringLiteral("custom_answer"), QString{}}}}};
    QVERIFY(QMetaObject::invokeMethod(root, "saveQuestionDraft", Q_ARG(QVariant, "request-1"),
                                      Q_ARG(QVariant, answers), Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, QVariantMap{})));

    QVariant stored;
    QVERIFY(QMetaObject::invokeMethod(root, "questionDraftFor", Q_RETURN_ARG(QVariant, stored),
                                      Q_ARG(QVariant, "request-1")));
    QCOMPARE(stored.toMap().value("questionIndex").toInt(), 1);

    root->setProperty("transcriptScope", QStringLiteral("scope-b"));
    QVERIFY(QMetaObject::invokeMethod(root, "questionDraftFor", Q_RETURN_ARG(QVariant, stored),
                                      Q_ARG(QVariant, "request-1")));
    QVERIFY(stored.toMap().isEmpty());

    root->setProperty("transcriptScope", QStringLiteral("scope-a"));
    QVERIFY(QMetaObject::invokeMethod(root, "questionDraftFor", Q_RETURN_ARG(QVariant, stored),
                                      Q_ARG(QVariant, "request-1")));
    QCOMPARE(stored.toMap().value("answers").toMap(), answers);

    for (int index = 0; index < 65; ++index) {
        QVERIFY(QMetaObject::invokeMethod(
            root, "saveQuestionDraft", Q_ARG(QVariant, QStringLiteral("bounded-%1").arg(index)),
            Q_ARG(QVariant, QVariantMap{}), Q_ARG(QVariant, 0), Q_ARG(QVariant, QVariantMap{})));
    }
    QCOMPARE(root->property("questionDraftOrder").toList().size(), 64);
    QVERIFY(QMetaObject::invokeMethod(root, "questionDraftFor", Q_RETURN_ARG(QVariant, stored),
                                      Q_ARG(QVariant, "request-1")));
    QVERIFY(stored.toMap().isEmpty());
}

void AgentWorkspaceViewTest::linksFileReferencesInInlineCode() {
    QTemporaryDir root;
    const auto write = [&root](const QString& path) {
        QVERIFY(QDir().mkpath(QFileInfo(root.filePath(path)).absolutePath()));
        QFile file(root.filePath(path));
        QVERIFY(file.open(QIODevice::WriteOnly));
    };
    write(QStringLiteral("apps/api/src/comment/comment.service.ts"));
    write(QStringLiteral("apps/web/src/components/Pages/Home/Hooks/usePostPreview.ts"));
    write(QStringLiteral("apps/web/src/components/Pages/MyPage/index.tsx"));
    write(QStringLiteral("apps/web/src/components/Pages/Tw/index.tsx"));
    write(QStringLiteral("docs/hướng-dẫn/tài-liệu.md"));
    QProcess git;
    git.setWorkingDirectory(root.path());
    git.start(QStringLiteral("git"), {QStringLiteral("init"), QStringLiteral("-q")});
    QVERIFY(git.waitForFinished(10'000));

    ketplus::TranscriptFileLinks links;
    int requests = 0;
    links.setFileRequester([&requests] { ++requests; });
    links.setRoot(root.path());
    QCOMPARE(links.href(QStringLiteral("apps/api/src/comment/comment.service.ts:54")),
             QStringLiteral("apps/api/src/comment/comment.service.ts:54"));
    QCOMPARE(requests, 0);
    // Formatting never lists the tree itself: a suffix waits for the background listing, and
    // the miss is not remembered.
    QVERIFY(links.href(QStringLiteral("Pages/Tw/index.tsx")).isEmpty());
    QCOMPARE(requests, 1);
    QVERIFY(links.href(QStringLiteral("Pages/Home/Hooks/usePostPreview.ts")).isEmpty());
    QCOMPARE(requests, 1);
    links.setFiles(root.path(), ketplus::file_mentions::listEntries(root.path()));
    // Listed with -z, so a non-ASCII name is not quoted away.
    QCOMPARE(links.href(QStringLiteral("tài-liệu.md:3")),
             QStringLiteral("docs/hướng-dẫn/tài-liệu.md:3"));
    // A range or column keeps its first line; a path from inside the tree finds its file.
    QCOMPARE(links.href(QStringLiteral("Pages/Home/Hooks/usePostPreview.ts:48-49")),
             QStringLiteral("apps/web/src/components/Pages/Home/Hooks/usePostPreview.ts:48"));
    QCOMPARE(links.href(QStringLiteral(".../usePostPreview.ts:50")),
             QStringLiteral("apps/web/src/components/Pages/Home/Hooks/usePostPreview.ts:50"));
    QCOMPARE(links.href(QStringLiteral("Pages/Tw/index.tsx:252")),
             QStringLiteral("apps/web/src/components/Pages/Tw/index.tsx:252"));
    // Two files end the same way, so neither is guessed.
    QVERIFY(links.href(QStringLiteral("index.tsx")).isEmpty());
    QVERIFY(links.href(QStringLiteral("linkify(encode(content))")).isEmpty());
    QVERIFY(links.href(QStringLiteral("encode")).isEmpty());
    QVERIFY(links.href(QStringLiteral("apps/api/src/missing.ts")).isEmpty());
    QVERIFY(links.href(QStringLiteral("https://example.com/a.ts")).isEmpty());
    QVERIFY(links.href(QStringLiteral("../outside.ts")).isEmpty());
    // `//host/share/…` is a UNC path on Windows; rendering must not look it up. Here it
    // names a local file, so only the guard keeps it from linking.
    const QString local =
        QFileInfo(root.filePath(QStringLiteral("docs/hướng-dẫn/tài-liệu.md"))).absoluteFilePath();
    QVERIFY(QFileInfo(QLatin1Char('/') + local).isFile());
    QVERIFY(links.href(QLatin1Char('/') + local).isEmpty());
}

void AgentWorkspaceViewTest::mermaidFillsGetReadableLabels() {
    using ketplus::agent_message::withReadableMermaidLabels;
    const QString diagram = QStringLiteral("flowchart TD\n"
                                           "  A[User] --> B[linkify]\n"
                                           "  style B fill:#fdd,stroke:#c33\n"
                                           "  classDef bad fill:#7a1f1f;\n"
                                           "  classDef named fill:pink,color:#000\n"
                                           "  style A stroke:#333");
    const QStringList lines = withReadableMermaidLabels(diagram).split('\n');
    // Labels take Két DS main text: the light theme's on a light fill, the dark theme's on a
    // dark one.
    const QString onLight = ket::Tokens::color(u"text-main", ket::Theme::Light).name();
    const QString onDark = ket::Tokens::color(u"text-main", ket::Theme::Dark).name();
    QVERIFY(onLight != onDark);
    QCOMPARE(lines.at(2), QStringLiteral("  style B fill:#fdd,stroke:#c33,color:") + onLight);
    QCOMPARE(lines.at(3), QStringLiteral("  classDef bad fill:#7a1f1f,color:") + onDark);
    // A mid-grey fill where neither theme text reaches 4.5:1 takes black or white, and every
    // fill gets a label at 4.5:1 or more.
    const auto luminance = [](const QColor& colour) {
        const auto channel = [](const qreal value) {
            return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(colour.redF()) + 0.7152 * channel(colour.greenF()) +
               0.0722 * channel(colour.blueF());
    };
    const auto labelContrast = [&luminance](const QString& fill) {
        const QString line = withReadableMermaidLabels(QStringLiteral("style A fill:") + fill);
        const QColor label(line.section(QStringLiteral("color:"), 1));
        const qreal a = luminance(QColor(fill));
        const qreal b = luminance(label);
        return (qMax(a, b) + 0.05) / (qMin(a, b) + 0.05);
    };
    QVERIFY(labelContrast(QStringLiteral("#8b8b8b")) >=
            ketplus::agent_message::minimumLabelContrast);
    for (int grey = 0; grey < 256; grey += 3) {
        for (const QColor& fill : {QColor(grey, grey, grey), QColor(grey, 255 - grey, 128),
                                   QColor(255 - grey, 64, grey)}) {
            const qreal ratio = labelContrast(fill.name());
            QVERIFY2(ratio >= ketplus::agent_message::minimumLabelContrast,
                     qPrintable(fill.name() + QStringLiteral(" %1").arg(ratio)));
        }
    }
    // An author's own text colour and a style without a fill stay as written.
    QCOMPARE(lines.at(4), QStringLiteral("  classDef named fill:pink,color:#000"));
    QCOMPARE(lines.at(5), QStringLiteral("  style A stroke:#333"));
    QCOMPARE(lines.at(1), QStringLiteral("  A[User] --> B[linkify]"));
}

void AgentWorkspaceViewTest::presentsStateAxesIndependently() {
    ketplus::AgentWorkspaceView view;
    view.setAgentAvailable(true);
    view.setSessionState(QStringLiteral("Review"), QStringLiteral("Thinking…"), true);
    view.setHistoryState({{QStringLiteral("hasOlder"), true}});

    QVariantMap state = view.presentationState();
    QCOMPARE(state.value("connection").toString(), QStringLiteral("connected"));
    QCOMPARE(state.value("folder").toString(), QStringLiteral("available"));
    QCOMPARE(state.value("history").toString(), QStringLiteral("partial"));
    QCOMPARE(state.value("turn").toString(), QStringLiteral("working"));

    view.setActivePath(QStringLiteral("/path-that-must-not-exist/ketplus-worktree"));
    view.setAgentAvailable(false);
    view.setHistoryState({{QStringLiteral("error"), QStringLiteral("offline")}});
    state = view.presentationState();
    QCOMPARE(state.value("connection").toString(), QStringLiteral("unavailable"));
    QCOMPARE(state.value("folder").toString(), QStringLiteral("missing"));
    QCOMPARE(state.value("history").toString(), QStringLiteral("error"));
    QCOMPARE(state.value("turn").toString(), QStringLiteral("working"));

    {
        // A worktree deleted and recreated in place updates the notice without a reopen.
        QTemporaryDir root;
        const QString worktree = root.filePath(QStringLiteral("worktree"));
        QVERIFY(QDir().mkpath(worktree));
        ketplus::AgentWorkspaceView folderView;
        folderView.setActivePath(worktree);
        QSignalSpy changed(&folderView, &ketplus::AgentWorkspaceView::presentationStateChanged);
        QVERIFY(QDir(worktree).removeRecursively());
        QTRY_COMPARE_WITH_TIMEOUT(folderView.presentationState().value("folder").toString(),
                                  QStringLiteral("missing"), 5'000);
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 5'000);
        changed.clear();
        QVERIFY(QDir().mkpath(worktree));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 5'000);
        QCOMPARE(folderView.presentationState().value("folder").toString(),
                 QStringLiteral("available"));
    }

    qputenv("KETPLUS_REDUCED_MOTION", "1");
    {
        ketplus::AgentWorkspaceView reducedMotionView;
        QVERIFY(!reducedMotionView.motionEnabled());
    }
    qunsetenv("KETPLUS_REDUCED_MOTION");
}

void AgentWorkspaceViewTest::compactFollowsProviderCapabilities() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.resize(800, 600);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    const auto offersCompact = [root] {
        for (const QVariant& command : root->property("slashCommands").toList()) {
            if (command.toMap().value("id").toString() == QStringLiteral("compact"))
                return true;
        }
        return false;
    };
    // Direct (ungated) transport keeps /compact.
    QVERIFY(offersCompact());
    const QJsonArray providers{
        QJsonObject{{"provider", "codex"}, {"capabilities", QJsonArray{"images", "turnSteer"}}}};
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(providers));
    QVERIFY(!offersCompact());
    QVERIFY(!root->property("usageCompactEnabled").toBool());
    view.setProviderRuntime(ketplus::ProviderRuntimeStatus::stateFrom(QJsonArray{
        QJsonObject{{"provider", "codex"}, {"capabilities", QJsonArray{"threadCompact"}}}}));
    QVERIFY(offersCompact());
}

void AgentWorkspaceViewTest::weeklyLimitFooter() {
    const auto conversation = [](const QString& id, const QString& provider, bool active = false) {
        return QVariantMap{{"id", id},         {"title", id},      {"provider", provider},
                           {"active", active}, {"running", false}, {"status", "Ready"}};
    };
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setConversations({conversation("first", "claude"), conversation("second", "codex", true)});
    QCOMPARE(view.statusProvider(), QStringLiteral("codex"));
    view.resize(800, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* value = root->findChild<QQuickItem*>("weeklyLimitValue");
    auto* progress = root->findChild<QQuickItem*>("weeklyLimitProgress");
    auto* refresh = root->findChild<QQuickItem*>("refreshWeeklyLimitButton");
    QVERIFY(value && progress && refresh);
    QCOMPARE(value->property("text").toString(), QStringLiteral("--"));
    view.setWeeklyLimit({{"available", true}, {"usedPercent", 50}, {"resetsAt", 2000000000}});
    QCOMPARE(value->property("text").toString(), QStringLiteral("50% used"));
    QCOMPARE(progress->width(), progress->parentItem()->width() / 2);
    QSignalSpy requested(&view, &ketplus::AgentWorkspaceView::weeklyLimitRequested);
    QVERIFY(QMetaObject::invokeMethod(refresh, "clicked"));
    QCOMPARE(requested.count(), 1);
    QTest::qWait(100);
    const auto screenshot = qEnvironmentVariable("KETPLUS_WEEKLY_SCREENSHOT");
    if (!screenshot.isEmpty())
        QVERIFY(view.quickView()->grabWindow().save(screenshot));
    view.resize(480, 600);
    ketplus::ThemeManager lightTheme(*qobject_cast<QApplication*>(QCoreApplication::instance()));
    lightTheme.setMode(ketplus::ThemeManager::Mode::Light);
    view.applyTheme(lightTheme.palette());
    view.setWeeklyLimit({{"available", true}, {"usedPercent", 100}});
    QTest::qWait(100);
    QVERIFY(value->width() >= value->property("contentWidth").toReal());
    QVERIFY(value->mapToScene(QPointF(value->width(), 0)).x() <=
            refresh->mapToScene(QPointF(0, 0)).x());
    if (!screenshot.isEmpty())
        QVERIFY(view.quickView()->grabWindow().save(screenshot + ".light.png"));
    view.setWeeklyLimit({{"loading", true}});
    QCOMPARE(value->property("text").toString(), QStringLiteral("Loading..."));
    QCOMPARE(progress->width(), 0.0);
    QVERIFY(!refresh->isEnabled());
    view.setWeeklyLimit({{"available", true}, {"usedPercent", 50}});
    view.setConversations({conversation("first", "claude", true), conversation("second", "codex")});
    QCOMPARE(view.statusProvider(), QStringLiteral("claude"));
    QCOMPARE(value->property("text").toString(), QStringLiteral("--"));
    QVERIFY(!refresh->isEnabled());
    auto legacy = conversation("legacy", "codex");
    legacy.remove("provider");
    view.setConversations({legacy});
    QCOMPARE(view.statusProvider(), QStringLiteral("codex"));
    view.setConversations({});
    QVERIFY(view.statusProvider().isEmpty());
    QVERIFY(!root->findChild<QQuickItem*>("weeklyLimitFooter")->isVisible());
}

void AgentWorkspaceViewTest::usageLimitsPopover() {
    const QVariantMap codexConversation{{"id", "codex"},  {"title", "Codex"}, {"provider", "codex"},
                                        {"active", true}, {"running", false}, {"status", "Ready"}};
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setConversations({codexConversation});
    view.setUsageSnapshot({
        {"provider", "codex"},
        {"context", QVariantMap{{"available", true},
                                {"usedTokens", 465000},
                                {"limitTokens", 500000},
                                {"autoCompactPercent", 93}}},
        {"limits",
         QVariantList{
             QVariantMap{{"id", "five-hour"},
                         {"label", "5-hour limit"},
                         {"available", true},
                         {"usedPercent", 17},
                         {"resetsAt", 2000000000}},
             QVariantMap{{"id", "weekly"},
                         {"label", "Weekly · all models"},
                         {"available", true},
                         {"usedPercent", 96},
                         {"resetsAt", 2000000000}},
         }},
    });
    view.resize(900, 680);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* composer = root->findChild<QQuickItem*>("agentComposer");
    QVERIFY(composer);
    auto* button = root->findChild<QQuickItem*>("usageLimitsButton");
    auto* popup = root->findChild<QObject*>("usageLimitsPopover");
    QVERIFY(button);
    QVERIFY(popup);
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTRY_VERIFY(popup->property("visible").toBool());
    auto* popupContent = qvariant_cast<QQuickItem*>(popup->property("contentItem"));
    QVERIFY(popupContent);
    QQuickItem* contextValue = nullptr;
    QQuickItem* contextProgress = nullptr;
    QQuickItem* fiveHourProgress = nullptr;
    QQuickItem* weeklyProgress = nullptr;
    QQuickItem* compact = nullptr;
    QTRY_VERIFY((contextValue = findVisualItem(popupContent, QStringLiteral("contextUsageValue"))));
    QTRY_VERIFY(
        (contextProgress = findVisualItem(popupContent, QStringLiteral("contextUsageProgress"))));
    QTRY_VERIFY((fiveHourProgress =
                     findVisualItem(popupContent, QStringLiteral("usageLimitProgress_five-hour"))));
    QTRY_VERIFY((weeklyProgress =
                     findVisualItem(popupContent, QStringLiteral("usageLimitProgress_weekly"))));
    QTRY_VERIFY((compact = findVisualItem(popupContent, QStringLiteral("compactSessionButton"))));
    QTRY_COMPARE(contextValue->property("text").toString(), QStringLiteral("465k / 500k (93%)"));
    QCOMPARE(contextProgress->width(), contextProgress->parentItem()->width() * 0.93);
    QCOMPARE(fiveHourProgress->width(), fiveHourProgress->parentItem()->width() * 0.17);
    QCOMPARE(weeklyProgress->width(), weeklyProgress->parentItem()->width() * 0.96);
    // Anchor click must toggle closed (and not immediately reopen).
    QVERIFY(QMetaObject::invokeMethod(popup, "toggleFromAnchor"));
    QTRY_VERIFY(!popup->property("visible").toBool());
    QCoreApplication::processEvents(); // let the same-click reopen guard expire before another
                                       // click
    QVERIFY(QMetaObject::invokeMethod(popup, "toggleFromAnchor"));
    QTRY_VERIFY(popup->property("visible").toBool());
    QSignalSpy submitted(&view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted);
    QVERIFY(QMetaObject::invokeMethod(compact, "clicked"));
    QCOMPARE(submitted.count(), 1);
    QCOMPARE(submitted.constFirst().at(0).toString(), QStringLiteral("/compact"));
}

void AgentWorkspaceViewTest::heldMessageNoticeRendersInTranscript() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setActivePath(QDir::tempPath());
    view.resize(900, 680);
    view.show();
    QSignalSpy actions(&view, &ketplus::AgentWorkspaceView::heldMessageActionRequested);
    view.resetMessages(
        {QVariantMap{
            {"id", "held-notice"},
            {"itemId", "held-notice"},
            {"kind", "system"},
            {"category", "system"},
            {"subtype", "heldAgentMessage"},
            {"title", "Agent message"},
            {"body", "A message from #1093 is held because automatic exchanges reached the limit."},
            {"sanitizedPayload", QVariantMap{{"messageId", "actual-held"}}}}},
        "receiver");
    auto* root = view.quickView()->rootObject();
    QTRY_VERIFY(findVisualItem(root, "heldMessageAction"));
    QTRY_VERIFY(!actions.isEmpty());
    QCOMPARE(actions.first().at(0).toString(), QString("actual-held"));
    QCOMPARE(actions.first().at(1).toBool(), false);
    view.acceptHeldMessageState("actual-held", {{"state", "held"}, {"can_release", true}});
    QTRY_VERIFY(findVisualItem(root, "receiveHeldMessageButton"));
    QVERIFY(findVisualItem(root, "receiveHeldMessageButton")->isVisible());
}

void AgentWorkspaceViewTest::heldMessageRequiresExplicitClickAndFencesDuplicates() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 680);
    view.show();
    QSignalSpy actions(&view, &ketplus::AgentWorkspaceView::heldMessageActionRequested);
    QQmlComponent component(view.quickView()->engine(), QUrl("qrc:/agent/HeldMessageAction.qml"));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    QScopedPointer<QQuickItem> card(qobject_cast<QQuickItem*>(
        component.createWithInitialProperties({{"width", 600}, {"messageId", "held-1"}})));
    QVERIFY(card);
    card->setParentItem(view.quickView()->rootObject());
    QCOMPARE(actions.size(), 1);
    QCOMPARE(actions.first().at(0).toString(), QString("held-1"));
    QCOMPARE(actions.first().at(1).toBool(), false); // observing is a read, never authorization
    view.acceptHeldMessageState("held-1", {{"state", "held"}, {"can_release", true}});
    auto* button = findVisualItem(card.data(), "receiveHeldMessageButton");
    QVERIFY(button);
    QTRY_VERIFY(button->isVisible() && button->isEnabled());
    QVERIFY(button->width() <= card->width());
    if (const auto capture = qEnvironmentVariable("KETPLUS_HELD_NOTICE_CAPTURE");
        !capture.isEmpty()) {
        for (auto* child : view.quickView()->rootObject()->childItems())
            if (child != card.data())
                child->setVisible(false);
        card->setPosition({24, 24});
        view.resize(660, 140);
        QTest::qWait(80);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QCOMPARE(actions.size(), 2);
    QCOMPARE(actions.last().at(1).toBool(), true);
    QVERIFY(!button->isEnabled());
    view.releaseHeldMessage("held-1");
    QCOMPARE(actions.size(), 2);
    view.acceptHeldMessageState("held-1", {{"state", "delivered"}, {"can_release", false}});
    QTRY_VERIFY(!button->isVisible());
    view.releaseHeldMessage("held-1");
    QCOMPARE(actions.size(), 2);
    view.acceptHeldMessageState(
        "held-1", {{"state", "failed"}, {"can_release", true}, {"reason", "Provider unavailable"}});
    QTRY_COMPARE(button->property("text").toString(), QString("Retry receiving"));
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QCOMPARE(actions.size(), 3);
    view.acceptHeldMessageState("held-1", {{"state", "queued"}, {"can_release", false}});
    auto* refresh = card->findChild<QObject*>("heldMessageRefreshTimer");
    QVERIFY(refresh);
    QTRY_VERIFY(refresh->property("running").toBool());
    view.hide();
    QTRY_VERIFY(!refresh->property("running").toBool());
    view.show();
    QTRY_VERIFY(refresh->property("running").toBool());
    view.acceptHeldMessageState("held-1", {{"state", "delivered"}, {"can_release", false}});
    QTRY_VERIFY(!refresh->property("running").toBool());
    view.resetMessages({}, "different-conversation");
    QVERIFY(view.heldMessageStates().isEmpty());
}

void AgentWorkspaceViewTest::backgroundTaskBar() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setActivePath(QDir::tempPath());
    view.resize(900, 680);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    auto* bar = root->findChild<QQuickItem*>("backgroundTaskBar");
    QVERIFY(bar);
    QVERIFY(!bar->isVisible());

    const QVariantMap shell{
        {"task_id", "build"}, {"kind", "shell"},
        {"title", "Rebuild"}, {"status", "running"},
        {"command", "ninja"}, {"started_at", QDateTime::currentMSecsSinceEpoch()},
        {"has_output", true}};
    const QVariantMap agent{{"task_id", "review"},  {"kind", "agent"},    {"title", "Review"},
                            {"status", "running"},  {"tool_uses", 3},     {"total_tokens", 38000},
                            {"summary", "Reading"}, {"last_tool", "Read"}};
    view.setBackgroundTasks({shell, agent});
    QTRY_VERIFY(bar->isVisible());
    QTRY_COMPARE(bar->property("headline").toString(),
                 QStringLiteral("Turn finished · 2 background tasks"));
    QTRY_VERIFY(findVisualItem(bar, QStringLiteral("backgroundTask_build")));
    QVERIFY(findVisualItem(bar, QStringLiteral("backgroundTask_review")));
    // A finished task reads as the mock words it: what it reported, else how long it took.
    const auto meta = [bar](const QVariantMap& task) {
        QVariant text;
        QMetaObject::invokeMethod(bar, "meta", Q_RETURN_ARG(QVariant, text),
                                  Q_ARG(QVariant, QVariant(task)));
        return text.toString();
    };
    QCOMPARE(meta({{"status", "completed"}, {"duration_ms", 48000}}), QString("Completed in 48s"));
    QCOMPARE(meta({{"status", "failed"}, {"duration_ms", 10000}, {"result", "2 tests failed"}}),
             QString("2 tests failed"));
    QCOMPARE(meta({{"status", "stopped"}, {"duration_ms", 75000}}),
             QString("Stopped after 1m 15s"));

    // Stop asks first, then sends the task id.
    QSignalSpy stops(&view, &ketplus::AgentWorkspaceView::backgroundTaskStopRequested);
    bar->setProperty("confirmingStop", QStringLiteral("build"));
    QQuickItem* confirm = nullptr;
    QTRY_VERIFY((confirm = findVisualItem(bar, QStringLiteral("confirmStopBackgroundTask"))) &&
                confirm->isVisible());
    QVERIFY(QMetaObject::invokeMethod(confirm, "clicked"));
    QCOMPARE(stops.count(), 1);
    QCOMPARE(stops.constFirst().at(0).toString(), QStringLiteral("build"));
    QVERIFY(bar->property("confirmingStop").toString().isEmpty());

    // The output pane shows the read tail, ignores answers for a task it no longer shows,
    // and closes when its task leaves the report.
    QSignalSpy reads(&view, &ketplus::AgentWorkspaceView::backgroundTaskOutputRequested);
    // The row itself opens its output, as the mock's current row; there is no Output button.
    auto* buildRow = findVisualItem(bar, QStringLiteral("backgroundTaskCard_build"));
    QVERIFY(buildRow);
    auto* reviewRow = findVisualItem(bar, QStringLiteral("backgroundTaskCard_review"));
    QVERIFY(reviewRow);
    // Wait for the strip's layout: the rows stack once it is polished.
    QTRY_VERIFY(reviewRow->mapToScene(QPointF()).y() > buildRow->mapToScene(QPointF()).y());
    const QPoint rowCenter =
        buildRow->mapToScene(QPointF(buildRow->width() / 2, buildRow->height() / 2)).toPoint();
    QTest::mouseClick(view.quickView(), Qt::LeftButton, {}, rowCenter);
    QTRY_COMPARE(reads.count(), 1);
    QCOMPARE(reads.constFirst().at(0).toString(), QStringLiteral("build"));
    // Keys: Return on the row opens it; on Stop it keeps Stop's meaning and Space asks to
    // stop. The row shows a focus ring of its own, apart from the open output's border.
    view.quickView()->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(view.quickView()));
    const auto actionOf = [](QQuickItem* card) -> QQuickItem* {
        for (auto* child : card->childItems())
            if (child->property("iconName").isValid())
                return child;
        return nullptr;
    };
    auto* ring = findVisualItem(buildRow, QStringLiteral("backgroundTaskFocusRing"));
    QVERIFY(ring);
    QVERIFY(!ring->isVisible());
    view.closeBackgroundTaskOutput();
    buildRow->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(ring->isVisible());
    QTest::keyClick(view.quickView(), Qt::Key_Return);
    QTRY_COMPARE(reads.count(), 2);
    QCOMPARE(reads.last().at(0).toString(), QStringLiteral("build"));
    view.closeBackgroundTaskOutput();
    auto* stop = actionOf(buildRow);
    QVERIFY(stop);
    stop->forceActiveFocus(Qt::TabFocusReason);
    QVERIFY(!ring->isVisible());
    QTest::keyClick(view.quickView(), Qt::Key_Return);
    QTest::qWait(50);
    QCOMPARE(reads.count(), 2);
    QTest::keyClick(view.quickView(), Qt::Key_Space);
    QTRY_COMPARE(bar->property("confirmingStop").toString(), QStringLiteral("build"));
    QCOMPARE(reads.count(), 2);
    bar->setProperty("confirmingStop", QString());
    view.requestBackgroundTaskOutput(QStringLiteral("build"));
    QCOMPARE(reads.count(), 3);
    QVERIFY(view.backgroundTaskOutput().value("loading").toBool());
    view.acceptBackgroundTaskResult(QStringLiteral("review"), QStringLiteral("output"),
                                    {{"output", "wrong"}});
    QVERIFY(view.backgroundTaskOutput().value("output").isNull());
    view.acceptBackgroundTaskResult(
        QStringLiteral("build"), QStringLiteral("output"),
        {{"output", "[3/4] Linking"}, {"total_bytes", 1400000}, {"truncated", true}});
    QCOMPARE(view.backgroundTaskOutput().value("output").toString(),
             QStringLiteral("[3/4] Linking"));
    QVERIFY(!view.backgroundTaskOutput().value("loading").toBool());
    auto* pane = root->findChild<QQuickItem*>("backgroundTaskOutput");
    QVERIFY(pane);
    QTRY_VERIFY(pane->isVisible());
    // A refresh keeps the shown tail until the new one arrives.
    view.requestBackgroundTaskOutput(QStringLiteral("build"));
    QCOMPARE(view.backgroundTaskOutput().value("output").toString(),
             QStringLiteral("[3/4] Linking"));

    QSignalSpy failures(&view, &ketplus::AgentWorkspaceView::backgroundTaskFailed);
    view.acceptBackgroundTaskResult(QStringLiteral("build"), QStringLiteral("stop"),
                                    {{"error", "This background task has already ended."}});
    QCOMPARE(failures.count(), 1);

    view.setBackgroundTasks({agent});
    QVERIFY(view.backgroundTaskOutput().isEmpty());
    QTRY_VERIFY(!pane->isVisible());

    // Dismiss hides a finished receipt locally, without deleting daemon task history.
    auto finished = shell;
    finished.insert("status", "completed");
    finished.insert("ended_at", QDateTime::currentMSecsSinceEpoch());
    view.setBackgroundTasks({finished});
    QQuickItem* taskCard = nullptr;
    QTRY_VERIFY((taskCard = findVisualItem(bar, "backgroundTaskCard_build")));
    // Return on Dismiss neither opens the output nor dismisses; Space dismisses.
    QQuickItem* dismiss = nullptr;
    QTRY_VERIFY((dismiss = actionOf(taskCard)) && dismiss->isVisible());
    const int readsBeforeDismiss = reads.count();
    dismiss->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(view.quickView(), Qt::Key_Return);
    QTest::qWait(50);
    QCOMPARE(reads.count(), readsBeforeDismiss);
    QVERIFY(bar->isVisible());
    QTest::keyClick(view.quickView(), Qt::Key_Space);
    QTRY_VERIFY(!bar->isVisible());
    QCOMPARE(view.backgroundTasks().size(), 1);
    view.setBackgroundTasks({shell});
    QTRY_VERIFY(bar->isVisible()); // A running incarnation is never hidden.

    // Another worktree never shows this conversation's tasks.
    view.setActivePath(QDir::homePath());
    QVERIFY(view.backgroundTasks().isEmpty());
    QTRY_VERIFY(!bar->isVisible());
}

void AgentWorkspaceViewTest::usageLimitsFollowConversationProvider() {
    const auto conversation = [](const QString& id, const QString& provider) {
        return QVariantMap{{"id", id},       {"title", id},      {"provider", provider},
                           {"active", true}, {"running", false}, {"status", "Ready"}};
    };
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setWeeklyLimit({{"available", true}, {"usedPercent", 46}, {"resetsAt", 2000000000}});
    view.setConversations({conversation("cursor", "cursor")});
    QCOMPARE(view.statusProvider(), QStringLiteral("cursor"));
    view.resize(900, 680);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QCOMPARE(root->findChild<QQuickItem*>("weeklyLimitValue")->property("text").toString(),
             QStringLiteral("--"));
    auto* composer = root->findChild<QQuickItem*>("agentComposer");
    QVERIFY(composer);
    auto* button = root->findChild<QQuickItem*>("usageLimitsButton");
    auto* popup = root->findChild<QObject*>("usageLimitsPopover");
    QVERIFY(button);
    QVERIFY(popup);
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTRY_VERIFY(popup->property("visible").toBool());
    auto* popupContent = qvariant_cast<QQuickItem*>(popup->property("contentItem"));
    QVERIFY(popupContent);
    QTRY_VERIFY(findVisualItem(popupContent, QStringLiteral("contextUsageValue")));
    QVERIFY(!findVisualItem(popupContent, QStringLiteral("usageLimitProgress_weekly")));
    QVERIFY(QMetaObject::invokeMethod(popup, "close"));
    QTRY_VERIFY(!popup->property("visible").toBool());
    QCoreApplication::processEvents(); // let the same-click reopen guard expire before another
                                       // click
    view.setConversations({conversation("codex", "codex")});
    QCOMPARE(view.statusProvider(), QStringLiteral("codex"));
    QCOMPARE(root->findChild<QQuickItem*>("weeklyLimitValue")->property("text").toString(),
             QStringLiteral("46% used"));
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QTRY_VERIFY(popup->property("visible").toBool());
    popupContent = qvariant_cast<QQuickItem*>(popup->property("contentItem"));
    QVERIFY(popupContent);
    QTRY_VERIFY(findVisualItem(popupContent, QStringLiteral("usageLimitProgress_weekly")));
}

void AgentWorkspaceViewTest::loadsQmlAndMapsWorktrees() {
    QTemporaryDir repository;
    QVERIFY(repository.isValid());
    const QString featurePath = repository.filePath(QStringLiteral("feature"));
    QVERIFY(QDir().mkpath(featurePath));

    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    const QVector<ketplus::GitWorktree> worktrees{
        {.path = repository.path(),
         .head = QStringLiteral("11111111"),
         .branch = QStringLiteral("main"),
         .current = true},
        {.path = featurePath,
         .head = QStringLiteral("22222222"),
         .branch = QStringLiteral("feature/agent-ui")},
    };
    view.setWorktrees(worktrees, featurePath, {featurePath});

    QCOMPARE(view.quickView()->status(), QQuickView::Ready);
    QCOMPARE(view.worktrees().size(), 2);
    QCOMPARE(view.activePath(), QFileInfo(featurePath).canonicalFilePath());
    QCOMPARE(view.worktrees().at(1).toMap().value(QStringLiteral("branch")).toString(),
             QStringLiteral("feature/agent-ui"));
    QCOMPARE(view.worktrees().at(1).toMap().value(QStringLiteral("status")).toString(),
             QStringLiteral("Agent working"));
    QCOMPARE(view.worktrees().at(1).toMap().value(QStringLiteral("running")).toBool(), true);
    QCOMPARE(view.worktrees().at(1).toMap().value(QStringLiteral("selected")).toBool(), true);
    QVERIFY(view.worktrees()
                .at(1)
                .toMap()
                .value(QStringLiteral("searchText"))
                .toString()
                .contains(QStringLiteral("feature/agent-ui")));
}

void AgentWorkspaceViewTest::sidebarGroupsConversationsWithoutWorktreeRail() {
    const auto previousAppearance = ket::Appearance::instance().mode();
    ket::Appearance::instance().setMode(ket::Appearance::Mode::Dark);
    const auto restoreAppearance = qScopeGuard(
        [previousAppearance] { ket::Appearance::instance().setMode(previousAppearance); });
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setRepositoryName(QStringLiteral("sample-repository"));
    view.setConversations({QVariantMap{
        {"id", "thread-a"}, {"title", "Implement sidebar"}, {"active", true}, {"started", true}}});
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* group = root->findChild<QQuickItem*>("sidebarRepositoryGroup");
    auto* list = root->findChild<QQuickItem*>("sidebarConversationList");
    QVERIFY(group);
    QVERIFY(list);
    QCOMPARE(group->property("name").toString(), QStringLiteral("sample-repository"));
    QTRY_COMPARE(list->property("count").toInt(), 1);
    QVERIFY(!root->findChild<QQuickItem*>("worktreeList"));
    QVERIFY(!root->findChild<QQuickItem*>("worktreeSearch"));
    QVERIFY(!root->findChild<QQuickItem*>("addWorktreeButton"));
    QVERIFY(!root->findChild<QQuickItem*>("allWorktreesButton"));
    group->setProperty("expanded", false);
    QTRY_VERIFY(!list->isVisible());
    group->setProperty("expanded", true);
    QTRY_VERIFY(list->isVisible());
    QSignalSpy create(&view, &ketplus::AgentWorkspaceView::newSessionRequested);
    QVERIFY(QMetaObject::invokeMethod(group, "actionTriggered"));
    QCOMPARE(create.count(), 1);
    const auto capture = qEnvironmentVariable("KETPLUS_SIDEBAR_CAPTURE");
    if (!capture.isEmpty()) {
        QTest::qWait(40);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
}

void AgentWorkspaceViewTest::restoringDoesNotCoverChatPane() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 700);
    view.show();

    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* chatPane = root->findChild<QQuickItem*>(QStringLiteral("chatPane"));
    auto* header = root->findChild<QQuickItem*>(QStringLiteral("agentWorkspaceHeader"));
    auto* newConversation =
        root->findChild<QQuickItem*>(QStringLiteral("headerNewConversationButton"));
    auto* terminal = root->findChild<QQuickItem*>(QStringLiteral("headerTerminalButton"));
    QVERIFY(chatPane);
    QVERIFY(header);
    QVERIFY(newConversation);
    QVERIFY(terminal);
    const auto headerRecipe =
        ket::iconButtonRecipe(ket::ControlSize::Medium, {}, ket::Appearance::instance().theme());
    const auto headerSize =
        ket::Appearance::scale(headerRecipe.height, ket::Appearance::instance().zoom());
    QCOMPARE(newConversation->size(), QSizeF(headerSize, headerSize));
    QCOMPARE(terminal->size(), QSizeF(headerSize, headerSize));

    // Transcripts restore fast enough that a full-pane overlay only caused a flash.
    QSignalSpy changed(&view, &ketplus::AgentWorkspaceView::restoringChanged);
    view.setRestoring(true);
    QCOMPARE(changed.size(), 1);
    QVERIFY(view.restoring());
    QVERIFY(root->findChild<QQuickItem*>(QStringLiteral("restoreOverlay")) == nullptr);
    QVERIFY(chatPane->isVisible());
    QVERIFY(chatPane->isEnabled());
    QVERIFY(header->isVisible());

    view.setRestoring(false);
    QCOMPARE(changed.size(), 2);
    QVERIFY(!view.restoring());
}

void AgentWorkspaceViewTest::largeWorktreeCatalogDoesNotCreateSidebarRail() {
    QTemporaryDir repository;
    QVERIFY(repository.isValid());

    QVector<ketplus::GitWorktree> worktrees;
    worktrees.reserve(120);
    for (int index = 0; index < 120; ++index) {
        const QString name = QStringLiteral("wt-%1").arg(index, 3, 10, QLatin1Char('0'));
        const QString path = repository.filePath(name);
        QVERIFY(QDir().mkpath(path));
        worktrees.append({.path = path,
                          .head = QStringLiteral("abcdef%1").arg(index),
                          .branch = QStringLiteral("feature/worktree-%1").arg(index)});
    }

    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    QSet<QString> conversationPaths;
    for (const ketplus::GitWorktree& worktree : worktrees) {
        conversationPaths.insert(worktree.path);
    }
    view.setWorktrees(worktrees, worktrees.at(94).path, {worktrees.at(12).path}, conversationPaths);
    view.show();
    QTest::qWait(100);
    QCOMPARE(view.worktrees().constFirst().toMap().value(QStringLiteral("branch")).toString(),
             QStringLiteral("feature/worktree-0"));
    QCOMPARE(view.worktrees().at(12).toMap().value(QStringLiteral("running")).toBool(), true);
    QCOMPARE(view.worktrees().at(12).toMap().value(QStringLiteral("selected")).toBool(), false);
    QCOMPARE(view.worktrees().constFirst().toMap().value(QStringLiteral("selected")).toBool(),
             false);
    bool selectedWorktreeFound = false;
    for (const QVariant& value : view.worktrees()) {
        const QVariantMap worktree = value.toMap();
        if (worktree.value(QStringLiteral("branch")).toString() ==
            QStringLiteral("feature/worktree-94")) {
            selectedWorktreeFound = true;
            QCOMPARE(worktree.value(QStringLiteral("running")).toBool(), false);
            QCOMPARE(worktree.value(QStringLiteral("selected")).toBool(), true);
        }
    }
    QVERIFY(selectedWorktreeFound);

    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    QVERIFY(!root->findChild<QQuickItem*>("worktreeList"));
    QVERIFY(!root->findChild<QQuickItem*>("worktreeSearch"));
    auto* list = root->findChild<QQuickItem*>("sidebarConversationList");
    QVERIFY(list);
    QTRY_VERIFY(list->height() > 250);
    QCOMPARE(view.worktrees().size(), 120); // Catalog remains available to the execution picker.
}

void AgentWorkspaceViewTest::exposesUiInteractions() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());

    QSignalSpy navigationRequested(&view, &ketplus::AgentWorkspaceView::productNavigationRequested);
    view.requestProductNavigation(QStringLiteral("atlas"));
    QCOMPARE(navigationRequested.count(), 1);
    QCOMPARE(navigationRequested.constFirst().constFirst().toString(), QStringLiteral("atlas"));
    view.requestProductNavigation(QString());
    QCOMPARE(navigationRequested.count(), 1);

    QSignalSpy fileOpened(&view, &ketplus::AgentWorkspaceView::fileOpenRequested);
    view.openRelativeFile(QStringLiteral("src/editor/EditorWidget.cpp"));
    QCOMPARE(fileOpened.count(), 1);
    QCOMPARE(fileOpened.constFirst().constFirst().toString(),
             QStringLiteral("src/editor/EditorWidget.cpp"));

    QSignalSpy fileDiffRequested(&view, &ketplus::AgentWorkspaceView::fileDiffRequested);
    view.requestFileDiff(QStringLiteral("src/editor/EditorWidget.cpp"));
    QCOMPARE(fileDiffRequested.count(), 1);
    QCOMPARE(fileDiffRequested.constFirst().constFirst().toString(),
             QStringLiteral("src/editor/EditorWidget.cpp"));

    QSignalSpy submitted(&view, &ketplus::AgentWorkspaceView::promptSubmitted);
    view.submitPrompt(QStringLiteral("  benchmark this UI  "));
    QCOMPARE(submitted.count(), 1);
    QCOMPARE(submitted.constFirst().constFirst().toString(), QStringLiteral("benchmark this UI"));

    QSignalSpy terminalRequested(&view, &ketplus::AgentWorkspaceView::terminalRequested);
    view.requestTerminal();
    QCOMPARE(terminalRequested.count(), 1);

    QSignalSpy clearRequested(&view, &ketplus::AgentWorkspaceView::clearSessionRequested);
    view.requestClearSession();
    QCOMPARE(clearRequested.count(), 1);

    QSignalSpy diffRequested(&view, &ketplus::AgentWorkspaceView::currentFileDiffRequested);
    view.requestCurrentFileDiff();
    QCOMPARE(diffRequested.count(), 1);

    QSignalSpy worktreeRequested(&view, &ketplus::AgentWorkspaceView::addWorktreeRequested);
    view.requestAddWorktree();
    QCOMPARE(worktreeRequested.count(), 1);
    QSignalSpy worktreeMenuRequested(&view,
                                     &ketplus::AgentWorkspaceView::worktreeContextMenuRequested);
    const auto previousActivePath = view.activePath();
    view.requestWorktreeContextMenu(QStringLiteral("/test/linked-worktree"));
    QCOMPARE(worktreeMenuRequested.count(), 1);
    QCOMPARE(worktreeMenuRequested.first().first().toString(),
             QStringLiteral("/test/linked-worktree"));
    QCOMPARE(view.activePath(), previousActivePath);

    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    const QString skillDirectory = workspace.filePath(QStringLiteral(".agents/skills/reviewer"));
    QVERIFY(QDir().mkpath(skillDirectory));
    const QString skillPath = QDir(skillDirectory).filePath(QStringLiteral("SKILL.md"));
    QFile skillFile(skillPath);
    QVERIFY(skillFile.open(QIODevice::WriteOnly | QIODevice::Text));
    skillFile.write(R"MD(---
name: reviewer
description: Review code changes before editing.
---

Check tests before changing code.
)MD");
    skillFile.close();
    view.setActivePath(workspace.path());
    view.refreshSkills();
    const QString normalizedSkillPath = QFileInfo(skillPath).canonicalFilePath();
    bool discoveredSkill = false;
    for (const QVariant& value : view.skills()) {
        const QVariantMap skill = value.toMap();
        if (skill.value(QStringLiteral("path")).toString() == normalizedSkillPath) {
            discoveredSkill = true;
            QCOMPARE(skill.value(QStringLiteral("name")).toString(), QStringLiteral("reviewer"));
            QCOMPARE(skill.value(QStringLiteral("source")).toString(), QStringLiteral("Project"));
            QCOMPARE(skill.value(QStringLiteral("enabled")).toBool(), false);
        }
    }
    QVERIFY(discoveredSkill);

    const QVariantList suggestions = view.skillSuggestions(QStringLiteral("rev"));
    QVariantMap reviewerSuggestion;
    for (const QVariant& value : suggestions) {
        const QVariantMap suggestion = value.toMap();
        if (suggestion.value(QStringLiteral("mention")).toString() == QStringLiteral("$reviewer")) {
            reviewerSuggestion = suggestion;
            break;
        }
    }
    QVERIFY(!reviewerSuggestion.isEmpty());
    QCOMPARE(reviewerSuggestion.value(QStringLiteral("source")).toString(),
             QStringLiteral("Project"));

    QSignalSpy configured(&view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted);
    view.submitConfiguredPrompt(QStringLiteral("$reviewer inspect this change"), {});
    QCOMPARE(configured.count(), 1);
    QVariantList submittedSkills =
        configured.constFirst().at(1).toMap().value(QStringLiteral("skills")).toList();
    QCOMPARE(submittedSkills.size(), 1);
    QCOMPARE(submittedSkills.constFirst().toMap().value(QStringLiteral("path")).toString(),
             normalizedSkillPath);

    view.toggleSkill(normalizedSkillPath, true);
    bool enabledSkill = false;
    for (const QVariant& value : view.skills()) {
        const QVariantMap skill = value.toMap();
        if (skill.value(QStringLiteral("path")).toString() == normalizedSkillPath) {
            enabledSkill = skill.value(QStringLiteral("enabled")).toBool();
        }
    }
    QVERIFY(enabledSkill);
    configured.clear();
    view.submitConfiguredPrompt(QStringLiteral("Use enabled skill and $reviewer"), {});
    QCOMPARE(configured.count(), 1);
    submittedSkills =
        configured.constFirst().at(1).toMap().value(QStringLiteral("skills")).toList();
    QCOMPARE(submittedSkills.size(), 1);
    QCOMPARE(submittedSkills.constFirst().toMap().value(QStringLiteral("path")).toString(),
             normalizedSkillPath);

    view.toggleSkill(normalizedSkillPath, false);
    configured.clear();
    view.submitConfiguredPrompt(QStringLiteral("Keep $UNKNOWN and shell $HOME unchanged"), {});
    QCOMPARE(configured.count(), 1);
    QVERIFY(!configured.constFirst().at(1).toMap().contains(QStringLiteral("skills")));

    view.setConversations(
        {QVariantMap{{QStringLiteral("id"), QStringLiteral("conversation-1")},
                     {QStringLiteral("title"), QStringLiteral("Conversation one")},
                     {QStringLiteral("status"), QStringLiteral("Ready")},
                     {QStringLiteral("running"), false},
                     {QStringLiteral("active"), true}}});
    QCOMPARE(view.conversations().size(), 1);
    QSignalSpy conversationActivated(&view, &ketplus::AgentWorkspaceView::conversationActivated);
    view.selectConversation(QStringLiteral("conversation-1"));
    QCOMPARE(conversationActivated.count(), 1);

    view.prepareBenchmarkMessages(128);
    QCOMPARE(view.benchmarkMessageCount(), 128);
    view.setBenchmarkScrollProgress(0.5);
}

void AgentWorkspaceViewTest::builtInBrowserSkillFollowsBrowserTools() {
    const QString configPath = QString::fromLocal8Bit(qgetenv("KETPLUS_BROWSER_TOOLS_CONFIG"));
    QVERIFY2(!configPath.isEmpty(), "tests run with an isolated browser tools config");
    QTemporaryDir browserApp;
    QVERIFY(browserApp.isValid());
    const QString skillDirectory =
        browserApp.filePath(QStringLiteral(".claude/skills/ketplus-browser"));
    QVERIFY(QDir().mkpath(skillDirectory));
    QFile skillFile(QDir(skillDirectory).filePath(QStringLiteral("SKILL.md")));
    QVERIFY(skillFile.open(QIODevice::WriteOnly | QIODevice::Text));
    skillFile.write(
        "---\nname: ketplus-browser\ndescription: Drive the KetPlus browser API.\n---\n");
    skillFile.close();

    ketplus::browser_tools::Settings settings;
    settings.enabled = true;
    settings.browserAppPath = browserApp.path();
    QVERIFY(ketplus::browser_tools::saveSettings(settings, configPath));
    const auto restore = qScopeGuard([&configPath] { QFile::remove(configPath); });

    ketplus::AgentWorkspaceView view;
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    view.setActivePath(workspace.path());
    view.refreshSkills();
    const auto builtInSkill = [&view]() -> QVariantMap {
        for (const QVariant& value : view.skills()) {
            const QVariantMap skill = value.toMap();
            if (skill.value(QStringLiteral("name")).toString() == QStringLiteral("ketplus-browser"))
                return skill;
        }
        return {};
    };
    // Offered in any repository while the tools are enabled, but never added to every turn.
    QCOMPARE(builtInSkill().value(QStringLiteral("source")).toString(), QStringLiteral("Built-in"));
    QCOMPARE(builtInSkill().value(QStringLiteral("enabled")).toBool(), false);
    bool mentioned = false;
    for (const QVariant& value : view.skillSuggestions(QStringLiteral("ketplus")))
        mentioned = mentioned || value.toMap().value(QStringLiteral("mention")).toString() ==
                                     QStringLiteral("$ketplus-browser");
    QVERIFY(mentioned);

    settings.enabled = false;
    QVERIFY(ketplus::browser_tools::saveSettings(settings, configPath));
    view.refreshSkills();
    QVERIFY(builtInSkill().isEmpty());
}

void AgentWorkspaceViewTest::pathChangeScansSkillsOffTheGuiThread() {
    QTemporaryDir withSkill;
    QTemporaryDir withoutSkill;
    QVERIFY(withSkill.isValid() && withoutSkill.isValid());
    const QString skillDirectory = withSkill.filePath(QStringLiteral(".agents/skills/reviewer"));
    QVERIFY(QDir().mkpath(skillDirectory));
    const QString skillPath = QDir(skillDirectory).filePath(QStringLiteral("SKILL.md"));
    QFile skillFile(skillPath);
    QVERIFY(skillFile.open(QIODevice::WriteOnly | QIODevice::Text));
    skillFile.write("---\nname: reviewer\ndescription: Review the current change.\n---\n");
    skillFile.close();
    const QString normalizedSkillPath = QFileInfo(skillPath).canonicalFilePath();
    const auto hasReviewer = [&normalizedSkillPath](const QVariantList& skills) {
        return std::any_of(skills.cbegin(), skills.cend(), [&](const QVariant& value) {
            return value.toMap().value(QStringLiteral("path")).toString() == normalizedSkillPath;
        });
    };

    ketplus::AgentWorkspaceView view;
    QSignalSpy configured(&view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted);
    // A prompt sent before the scan lands still resolves the new worktree's skills.
    view.setActivePath(withSkill.path());
    view.submitConfiguredPrompt(QStringLiteral("$reviewer inspect this change"), {});
    QCOMPARE(configured.count(), 1);
    QVERIFY(hasReviewer(
        configured.constFirst().at(1).toMap().value(QStringLiteral("skills")).toList()));

    // Without a prompt the list follows the path once the pooled scan lands.
    view.setActivePath(withoutSkill.path());
    QTRY_VERIFY(!hasReviewer(view.skills()));
    view.setActivePath(withSkill.path());
    QTRY_VERIFY(hasReviewer(view.skills()));
    // A scan for an earlier path never overwrites the current one.
    view.setActivePath(withoutSkill.path());
    view.setActivePath(withSkill.path());
    QTest::qWait(200);
    QVERIFY(hasReviewer(view.skills()));
}

void AgentWorkspaceViewTest::mentionsOnlyFilesInConversationWorktree() {
    namespace mentions = ketplus::file_mentions;
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    const QString worktree = outside.filePath(QStringLiteral("worktree"));
    QVERIFY(QDir().mkpath(QDir(worktree).filePath(QStringLiteral("src/app"))));
    const auto writeFile = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write("x") == 1;
    };
    QVERIFY(writeFile(QDir(worktree).filePath(QStringLiteral("src/app/main.cpp"))));
    QVERIFY(writeFile(QDir(worktree).filePath(QStringLiteral("README.md"))));
    QVERIFY(writeFile(outside.filePath(QStringLiteral("secret.txt"))));
    QVERIFY(QFile::link(outside.filePath(QStringLiteral("secret.txt")),
                        QDir(worktree).filePath(QStringLiteral("escape.txt"))));

    const QVector<mentions::Mention> parsed = mentions::parseMentions(
        QStringLiteral("See @src/app/main.cpp, @\"a b/c.txt\" and mail me@example.com."));
    QCOMPARE(parsed.size(), 2);
    QCOMPARE(parsed.at(0).path, QStringLiteral("src/app/main.cpp"));
    QCOMPARE(parsed.at(1).path, QStringLiteral("a b/c.txt"));
    QCOMPARE(mentions::insertionText(QStringLiteral("a b/c.txt")),
             QStringLiteral("@\"a b/c.txt\""));

    QCOMPARE(mentions::resolve(worktree, QStringLiteral("src/app/")).relativePath,
             QStringLiteral("src/app/"));
    QCOMPARE(mentions::resolve(worktree, QStringLiteral("src/../README.md")).resolution,
             mentions::Resolution::Inside);
    QCOMPARE(mentions::resolve(worktree, QStringLiteral("unknown-handle")).resolution,
             mentions::Resolution::Missing);
    QCOMPARE(mentions::resolve(worktree, QStringLiteral("../secret.txt")).resolution,
             mentions::Resolution::Outside);
    QCOMPARE(mentions::resolve(worktree, outside.filePath(QStringLiteral("secret.txt"))).resolution,
             mentions::Resolution::Outside);
    QCOMPARE(mentions::resolve(worktree, QStringLiteral("escape.txt")).resolution,
             mentions::Resolution::Outside);
    // Paths that name nothing on disk are ordinary text, wherever they would point.
    QCOMPARE(mentions::resolve(worktree, QStringLiteral("/api/users")).resolution,
             mentions::Resolution::Missing);
    QCOMPARE(mentions::resolve(worktree, QStringLiteral("../no-such-file")).resolution,
             mentions::Resolution::Missing);

    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setActivePath(worktree);
    // The first request starts an off-thread listing and returns nothing yet.
    QSignalSpy listed(&view, &ketplus::AgentWorkspaceView::fileMentionsChanged);
    QVERIFY(view.fileMentionSuggestions(QStringLiteral("main")).isEmpty());
    QTRY_COMPARE(listed.count(), 1);
    const QVariantList suggestions = view.fileMentionSuggestions(QStringLiteral("main"));
    QVERIFY(!suggestions.isEmpty());
    QCOMPARE(suggestions.constFirst().toMap().value(QStringLiteral("mention")).toString(),
             QStringLiteral("@src/app/main.cpp"));
    bool offersFolder = false;
    for (const QVariant& value : view.fileMentionSuggestions(QStringLiteral("app"))) {
        offersFolder |=
            value.toMap().value(QStringLiteral("path")).toString() == QStringLiteral("src/app/");
    }
    QVERIFY(offersFolder);
    for (const QVariant& value : view.fileMentionSuggestions(QStringLiteral("secret"))) {
        QVERIFY(!value.toMap().value(QStringLiteral("path")).toString().contains("secret"));
    }

    QSignalSpy configured(&view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted);
    QSignalSpy rejected(&view, &ketplus::AgentWorkspaceView::composerErrorOccurred);
    view.submitConfiguredPrompt(QStringLiteral("Explain @src/app/main.cpp in @src/app/"), {});
    QCOMPARE(configured.count(), 1);
    const QVariantList files =
        configured.constFirst().at(1).toMap().value(QStringLiteral("files")).toList();
    QCOMPARE(files.size(), 2);
    QCOMPARE(files.at(1).toMap().value(QStringLiteral("directory")).toBool(), true);
    QVERIFY(mentions::referencedPathInstructions(files).contains(
        QStringLiteral("`src/app/main.cpp` (file)")));

    configured.clear();
    view.submitConfiguredPrompt(QStringLiteral("Read @../secret.txt"), {});
    QCOMPARE(configured.count(), 0);
    QCOMPARE(rejected.count(), 1);
    view.submitConfiguredPrompt(QStringLiteral("Read @escape.txt"), {});
    QCOMPARE(configured.count(), 0);
    QCOMPARE(rejected.count(), 2);
    view.submitConfiguredPrompt(QStringLiteral("Call @/api/users"), {});
    QCOMPARE(configured.count(), 1);
    QCOMPARE(rejected.count(), 2);

    // Typing `@` in the composer opens the file palette.
    auto* root = view.quickView()->rootObject();
    auto* prompt = root->findChild<QQuickItem*>(QStringLiteral("agentPrompt"));
    QVERIFY(prompt);
    prompt->setProperty("text", QStringLiteral("look at @mai"));
    prompt->setProperty("cursorPosition", 12);
    QTRY_COMPARE(root->property("visibleSkillSuggestions").toList().size(), 1);
    // A file suggestion shows its worktree path, then File or Folder.
    view.resize(900, 600);
    view.show();
    // Delegates hang off the item tree, not the QObject tree.
    const auto findItem = [](const auto& self, QQuickItem* item,
                             const QString& name) -> QQuickItem* {
        if (item->objectName() == name)
            return item;
        for (auto* child : item->childItems())
            if (auto* found = self(self, child, name))
                return found;
        return nullptr;
    };
    QQuickItem* fileRow = nullptr;
    QTRY_VERIFY((fileRow = findItem(findItem, root, QStringLiteral("fileMentionSuggestion"))) &&
                fileRow->isVisible());
    const auto* pathLabel = findItem(findItem, fileRow, QStringLiteral("fileMentionPath"));
    QVERIFY(pathLabel);
    QCOMPARE(pathLabel->property("text").toString(), QStringLiteral("src/app/main.cpp"));
    if (const auto capture = qEnvironmentVariable("KETPLUS_MENTION_CAPTURE"); !capture.isEmpty()) {
        QTest::qWait(300);
        QVERIFY(view.quickView()->grabWindow().save(capture));
    }
    QMetaObject::invokeMethod(
        root, "applySkillSuggestion",
        Q_ARG(QVariant, root->property("visibleSkillSuggestions").toList().constFirst()));
    QCOMPARE(prompt->property("text").toString(), QStringLiteral("look at @src/app/main.cpp "));
}

// `@agent/4821` names a conversation as its header does; it completes like `#4821`.
void AgentWorkspaceViewTest::agentMentionsCompleteConversations() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setActivePath(QDir::tempPath());
    view.setConversationDirectory([] {
        return QVariantList{
            QVariantMap{{"number", "4821"}, {"title", "Parser"}, {"provider", "claude"}},
            QVariantMap{{"number", "5302"}, {"title", "Icons"}, {"provider", "codex"}}};
    });
    view.resize(900, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    auto* prompt = root->findChild<QQuickItem*>(QStringLiteral("agentPrompt"));
    QVERIFY(prompt);
    const auto complete = [&](const QString& text, const QString& expected) {
        prompt->setProperty("text", text);
        prompt->setProperty("cursorPosition", int(text.size()));
        QTRY_COMPARE(root->property("visibleSkillSuggestions").toList().size(), 1);
        const QVariantMap suggestion =
            root->property("visibleSkillSuggestions").toList().constFirst().toMap();
        QCOMPARE(suggestion.value("kind").toString(), QStringLiteral("conversation"));
        QMetaObject::invokeMethod(root, "applySkillSuggestion",
                                  Q_ARG(QVariant, QVariant(suggestion)));
        QCOMPARE(prompt->property("text").toString(), expected);
    };
    complete(QStringLiteral("ask @agent/48"), QStringLiteral("ask @agent/4821 "));
    complete(QStringLiteral("ask #53"), QStringLiteral("ask #5302 "));

    // The mention is not a worktree path, so sending it is not refused.
    QSignalSpy configured(&view, &ketplus::AgentWorkspaceView::configuredPromptSubmitted);
    QSignalSpy rejected(&view, &ketplus::AgentWorkspaceView::composerErrorOccurred);
    view.submitConfiguredPrompt(QStringLiteral("ask @agent/4821 about it"), {});
    QCOMPARE(configured.count(), 1);
    QCOMPARE(rejected.count(), 0);
}

void AgentWorkspaceViewTest::claudeUsageBudget() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.setAgentAvailable(true);
    view.setConversations(
        {QVariantMap{{"id", "claude"}, {"provider", "claude"}, {"active", true}}});
    view.setUsageSnapshot({{"provider", "claude"},
                           {"session", QVariantMap{{"mode", "api_key"},
                                                   {"estimatedCostUsd", 12.5},
                                                   {"budgetUsd", 5.0},
                                                   {"inputTokens", 300},
                                                   {"outputTokens", 40},
                                                   {"cacheReadInputTokens", 1000},
                                                   {"cacheCreationInputTokens", 500}}}});
    int requests = 0;
    view.setClaudeUsageRequester([&](ketplus::AgentWorkspaceView::UsageReply reply) {
        ++requests;
        reply({{"configured", true},
               {"report", QJsonObject{{"costUsd", 42.5},
                                      {"totalTokens", 50000},
                                      {"startingAt", "2026-10-01T00:00:00Z"},
                                      {"endingAt", "2026-10-31T00:00:00Z"}}}},
              {});
    });
    view.resize(900, 680);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* popup = root->findChild<QObject*>("usageLimitsPopover");
    QVERIFY(popup);
    QVERIFY(QMetaObject::invokeMethod(popup, "toggleFromAnchor"));
    QTRY_VERIFY(popup->property("visible").toBool());
    auto* content = qvariant_cast<QQuickItem*>(popup->property("contentItem"));
    QVERIFY(content);
    auto* cost = findVisualItem(content, "claudeSessionCost");
    QVERIFY(cost);
    QCOMPARE(cost->property("text").toString(), "$12.5000");
    auto* budget = findVisualItem(content, "claudeQueryBudget");
    QVERIFY(budget);
    QCOMPARE(budget->property("text").toString(), "$5.0000");
    auto* account = findVisualItem(content, "claudeAccountSummary");
    QVERIFY(account);
    QTRY_VERIFY(account->property("text").toString().contains("$42.5000"));
    QCOMPARE(requests, 1);
    QVERIFY(popup->property("height").toDouble() <= view.height());
    QTRY_VERIFY(popup->property("y").toDouble() + popup->property("height").toDouble() <=
                view.height());
    QTest::qWait(100);
    QVERIFY(view.quickView()->grabWindow().save("/tmp/ketplus-claude-usage.png"));
    ketplus::ThemeManager theme(*qApp);
    theme.previewMode(ketplus::ThemeManager::Mode::Light);
    view.applyTheme(theme.palette());
    view.resize(720, 520);
    QTest::qWait(250);
    QTRY_VERIFY(popup->property("y").toDouble() + popup->property("height").toDouble() <=
                view.height());
    content->setProperty(
        "contentY", qMax(0.0, content->property("contentHeight").toDouble() - content->height()));
    QTest::qWait(100);
    QVERIFY(view.quickView()->grabWindow().save("/tmp/ketplus-claude-usage-light.png"));
    view.setClaudeUsageRequester(
        [](ketplus::AgentWorkspaceView::UsageReply reply) { reply({}, "Admin access denied"); });
    auto* refresh = findVisualItem(content, "claudeAccountRefresh");
    QVERIFY(refresh);
    QVERIFY(QMetaObject::invokeMethod(refresh, "clicked"));
    QTRY_COMPARE(account->property("text").toString(), "Admin access denied");
    QVERIFY(!view.claudeAccountUsage().contains("report"));
    view.setUsageSnapshot({{"provider", "claude"}});
    // Repeater recreates its delegates when the snapshot changes; reacquire them.
    QTRY_VERIFY(findVisualItem(content, "claudeSessionCost"));
    QTRY_COMPARE(findVisualItem(content, "claudeSessionCost")->property("text").toString(),
                 "Not reported");
    QTRY_VERIFY(findVisualItem(content, "claudeQueryBudget"));
    QTRY_COMPARE(findVisualItem(content, "claudeQueryBudget")->property("text").toString(),
                 "Not reported");
}

void AgentWorkspaceViewTest::questionCardSingleClickAndFreeText() {
    ketplus::AgentWorkspaceView view;
    view.resize(900, 700);
    view.show();
    QQmlComponent component(view.quickView()->engine(), QUrl("qrc:/agent/AgentQuestionCard.qml"));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    const QVariantList options{QVariantMap{{"id", "yes-original"}, {"label", "Yes"}},
                               QVariantMap{{"id", "no-original"}, {"label", "No"}}};
    auto makeCard = [&](const QVariantList& choices, bool secret) {
        return qobject_cast<QQuickItem*>(component.createWithInitialProperties(
            {{"width", 640},
             {"questions", QVariantList{QVariantMap{{"id", "choice"},
                                                    {"question", "Continue?"},
                                                    {"selection", "single"},
                                                    {"isOther", true},
                                                    {"isSecret", secret},
                                                    {"options", choices}}}},
             {"pending", true},
             {"answerData", QVariantMap{}},
             {"messageKey", "single-click"}}));
    };
    QScopedPointer<QQuickItem> card(makeCard(options, false));
    QVERIFY(card);
    card->setParentItem(view.quickView()->rootObject());
    QSignalSpy sent(card.data(), SIGNAL(answersSubmitted(QVariant)));
    QVERIFY(sent.isValid());
    QVERIFY(card->property("confirmation").toBool());
    auto* button = findVisualItem(card.data(), "questionConfirmOption");
    QVERIFY(button);
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QCOMPARE(sent.size(), 1);
    const auto answer = sent.first().first().toMap().value("choice").toMap();
    QCOMPARE(answer.value("option_ids").toList().first().toString(), "yes-original");
    QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    QCOMPARE(sent.size(), 1); // Programmatic second activation is fenced too.
    card.reset(makeCard({}, true));
    QVERIFY(card);
    card->setParentItem(view.quickView()->rootObject());
    QVERIFY(!card->property("confirmation").toBool());
    QVERIFY(!findVisualItem(card.data(), "questionOtherOption")->isVisible());
    auto* input = findVisualItem(card.data(), "questionOtherInput");
    QVERIFY(input && input->isVisible());
    QCOMPARE(input->property("echoMode").toInt(), 2); // QQuickTextInput::Password
    input->setProperty("text", "custom answer");
    QTRY_VERIFY(findVisualItem(card.data(), "questionContinueButton")->isEnabled());
}
