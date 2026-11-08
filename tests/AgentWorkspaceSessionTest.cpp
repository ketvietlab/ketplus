#include "AgentWorkspaceViewTest.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include "ui/Theme.h"

#include <QAbstractItemModelTester>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QUuid>
#include <QtTest>

using namespace agent_workspace_test;

void AgentWorkspaceViewTest::persistsComposerDraftByConversation() {
    const QString firstScope = QStringLiteral("draft-first-%1").arg(QUuid::createUuid().toString());
    const QString secondScope =
        QStringLiteral("draft-second-%1").arg(QUuid::createUuid().toString());
    {
        ketplus::AgentWorkspaceView view;
        view.saveComposerDraft(firstScope, QStringLiteral("Keep this conversation draft"));
        view.saveComposerDraft(secondScope, QStringLiteral("Independent draft"));
    }
    ketplus::AgentWorkspaceView restored;
    QCOMPARE(restored.composerDraft(firstScope), QStringLiteral("Keep this conversation draft"));
    QCOMPARE(restored.composerDraft(secondScope), QStringLiteral("Independent draft"));
    restored.saveComposerDraft(firstScope, QString{});
    QVERIFY(restored.composerDraft(firstScope).isEmpty());
    QCOMPARE(restored.composerDraft(secondScope), QStringLiteral("Independent draft"));
    restored.saveComposerDraft(secondScope, QString{});

    QTemporaryDir files;
    QVERIFY(files.isValid());
    QFile attachment(files.filePath(QStringLiteral("draft.txt")));
    QVERIFY(attachment.open(QIODevice::WriteOnly));
    attachment.write("draft attachment");
    attachment.close();
    const QVariantList conversation{QVariantMap{{QStringLiteral("id"), firstScope},
                                                {QStringLiteral("active"), true},
                                                {QStringLiteral("running"), false},
                                                {QStringLiteral("missing"), false}}};
    {
        ketplus::AgentWorkspaceView view;
        view.setActivePath(files.path());
        view.setConversations(conversation);
        view.addAttachments({attachment.fileName()});
        QCOMPARE(view.attachments().size(), 1);
        view.setConversations({QVariantMap{{QStringLiteral("id"), secondScope},
                                           {QStringLiteral("active"), true},
                                           {QStringLiteral("running"), false},
                                           {QStringLiteral("missing"), false}}});
    }
    ketplus::AgentWorkspaceView attachmentRestored;
    attachmentRestored.setActivePath(files.path());
    attachmentRestored.setConversations(conversation);
    QCOMPARE(attachmentRestored.attachments().size(), 1);
    QCOMPARE(attachmentRestored.attachments().first().toMap().value(QStringLiteral("path")),
             attachment.fileName());
    attachmentRestored.removeAttachment(0);
}

void AgentWorkspaceViewTest::runningCheckoutsIncludeExecutionWorktrees() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString conversation = directory.filePath(QStringLiteral("repo"));
    const QString execution = directory.filePath(QStringLiteral("repo-feature"));
    QVERIFY(QDir().mkpath(conversation));
    QVERIFY(QDir().mkpath(execution));

    ketplus::CodexAppServer appServer(QStringLiteral("/nonexistent/fake-codex"));
    ketplus::AgentSessionManager manager(&appServer);
    // A conversation born in `repo` that now runs in `repo-feature`.
    emit appServer.daemonSessionDiscovered(
        QJsonObject{{QStringLiteral("session_id"), QStringLiteral("moved-session")},
                    {QStringLiteral("session_generation"), 1},
                    {QStringLiteral("conversation_path"), conversation},
                    {QStringLiteral("worktree_path"), execution},
                    {QStringLiteral("state"), QStringLiteral("running")}});
    const QString canonicalExecution = QFileInfo(execution).canonicalFilePath();
    QVERIFY(!manager.runningPaths().contains(canonicalExecution));
    QVERIFY(manager.runningCheckoutPaths().contains(canonicalExecution));
    QVERIFY(manager.runningCheckoutPaths().contains(QFileInfo(conversation).canonicalFilePath()));
}

void AgentWorkspaceViewTest::selectingColdDaemonConversationShowsChatRestoreState() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString worktree = directory.filePath(QStringLiteral("worktree"));
    QVERIFY(QDir().mkpath(worktree));

    ketplus::CodexAppServer appServer(QStringLiteral("/nonexistent/fake-codex"));
    ketplus::AgentSessionManager manager(&appServer);
    const auto storedSession = [&worktree](const QString& sessionId) {
        return QJsonObject{{QStringLiteral("session_id"), sessionId},
                           {QStringLiteral("session_generation"), 1},
                           {QStringLiteral("worktree_path"), worktree},
                           {QStringLiteral("state"), QStringLiteral("ready")}};
    };

    emit appServer.daemonSessionDiscovered(storedSession(QStringLiteral("warm-session")));
    emit appServer.daemonSessionRestoreStarted(QStringLiteral("warm-session"));
    emit appServer.daemonSessionRestoreFinished(QStringLiteral("warm-session"));
    emit appServer.daemonSessionDiscovered(storedSession(QStringLiteral("cold-session")));
    emit appServer.daemonSessionRestoreStarted(QStringLiteral("cold-session"));

    manager.activate(worktree);
    QVERIFY(!manager.isRestoring());
    QSignalSpy restoreState(&manager, &ketplus::AgentSessionManager::restoreStateChanged);
    manager.selectConversation(worktree, QStringLiteral("cold-session"));
    QVERIFY(manager.isRestoring());
    QCOMPARE(restoreState.size(), 1);
    QCOMPARE(restoreState.constFirst().constFirst().toBool(), true);

    emit appServer.daemonSessionRestoreFinished(QStringLiteral("cold-session"));
    QVERIFY(!manager.isRestoring());
    QCOMPARE(restoreState.size(), 2);
    QCOMPARE(restoreState.constLast().constFirst().toBool(), false);
}

void AgentWorkspaceViewTest::streamsAndKeepsSessionsByWorktree() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString firstWorktree = directory.filePath(QStringLiteral("first"));
    const QString secondWorktree = directory.filePath(QStringLiteral("second"));
    QVERIFY(QDir().mkpath(firstWorktree));
    QVERIFY(QDir().mkpath(secondWorktree));

    const QString serverPath = directory.filePath(QStringLiteral("fake-codex"));
    QFile server(serverPath);
    QVERIFY(server.open(QIODevice::WriteOnly));
    const QByteArray source = R"PY(#!/usr/bin/python3
import json
import sys
import time

def send(value):
    print(json.dumps(value, separators=(",", ":")), flush=True)

thread_number = 0
for line in sys.stdin:
    message = json.loads(line)
    method = message.get("method")
    request_id = message.get("id")
    params = message.get("params", {})
    if method == "initialize":
        send({"id": request_id, "result": {"codexHome": "/tmp", "platformFamily": "unix", "platformOs": "macos", "userAgent": "fake"}})
    elif method == "initialized":
        continue
    elif method == "thread/start":
        thread_number += 1
        thread_id = "thread-%d" % thread_number
        send({"id": request_id, "result": {"thread": {"id": thread_id, "turns": []}}})
    elif method == "turn/start":
        thread_id = params["threadId"]
        turn_id = "turn-" + thread_id
        send({"id": request_id, "result": {"turn": {"id": turn_id, "status": "inProgress", "items": []}}})
        send({"method": "turn/started", "params": {"threadId": thread_id, "turn": {"id": turn_id, "status": "inProgress", "items": []}}})
        send({"method": "item/started", "params": {"threadId": thread_id, "turnId": turn_id, "startedAtMs": 1, "item": {"id": "command-1", "type": "commandExecution", "command": "pwd", "commandActions": [], "cwd": params.get("cwd", ""), "status": "inProgress"}}})
        time.sleep(0.15)
        send({"method": "item/commandExecution/outputDelta", "params": {"threadId": thread_id, "turnId": turn_id, "itemId": "command-1", "delta": "workspace\n"}})
        send({"method": "item/completed", "params": {"threadId": thread_id, "turnId": turn_id, "completedAtMs": 2, "item": {"id": "command-1", "type": "commandExecution", "command": "pwd", "commandActions": [], "cwd": params.get("cwd", ""), "status": "completed", "aggregatedOutput": "workspace\n", "exitCode": 0}}})
        send({"method": "item/started", "params": {"threadId": thread_id, "turnId": turn_id, "startedAtMs": 3, "item": {"id": "command-read", "type": "commandExecution", "command": "cat src/main.cpp", "commandActions": [], "cwd": params.get("cwd", ""), "status": "inProgress"}}})
        send({"method": "item/completed", "params": {"threadId": thread_id, "turnId": turn_id, "completedAtMs": 4, "item": {"id": "command-read", "type": "commandExecution", "command": "cat src/main.cpp", "commandActions": [], "cwd": params.get("cwd", ""), "status": "completed", "aggregatedOutput": "int main() {}\n", "exitCode": 0}}})
        patch = {"id": "patch-1", "type": "fileChange", "status": "completed", "changes": [
            {"path": "src/main.cpp", "kind": {"type": "update"}, "diff": "--- a/src/main.cpp\n+++ b/src/main.cpp\n@@ -1,2 +1,3 @@\n-old\n---content\n+new\n+++content\n+extra\n\\ No newline at end of file\n"},
            {"path": "new.txt", "kind": {"type": "add"}, "diff": "one\n\ntwo\n"},
            {"path": "deleted.txt", "kind": {"type": "delete"}, "diff": "last line"},
            {"path": "binary.png", "kind": {"type": "update"}, "diff": "Binary files differ"}]}
        send({"method": "item/started", "params": {"threadId": thread_id, "turnId": turn_id, "item": patch}})
        send({"method": "item/completed", "params": {"threadId": thread_id, "turnId": turn_id, "item": patch}})
        send({"method": "item/started", "params": {"threadId": thread_id, "turnId": turn_id, "startedAtMs": 3, "item": {"id": "message-1", "type": "agentMessage", "phase": "final_answer", "text": ""}}})
        send({"method": "item/agentMessage/delta", "params": {"threadId": thread_id, "turnId": turn_id, "itemId": "message-1", "delta": "Hello "}})
        send({"method": "item/agentMessage/delta", "params": {"threadId": thread_id, "turnId": turn_id, "itemId": "message-1", "delta": "world"}})
        send({"method": "item/completed", "params": {"threadId": thread_id, "turnId": turn_id, "completedAtMs": 4, "item": {"id": "message-1", "type": "agentMessage", "phase": "final_answer", "text": "Hello world"}}})
        send({"method": "turn/completed", "params": {"threadId": thread_id, "turn": {"id": turn_id, "status": "completed", "items": []}}})
)PY";
    QCOMPARE(server.write(source), source.size());
    server.close();
    QVERIFY(QFile::setPermissions(serverPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));

    ketplus::CodexAppServer appServer(serverPath);
    ketplus::AgentSessionManager manager(&appServer);
    QSignalSpy activity(&manager, &ketplus::AgentSessionManager::activityChanged);

    manager.submitPrompt(firstWorktree, QStringLiteral("Inspect this worktree"));
    QTRY_VERIFY_WITH_TIMEOUT(manager.isRunning(firstWorktree), 2000);
    const QString originalConversationId = manager.conversations(firstWorktree)
                                               .constFirst()
                                               .toMap()
                                               .value(QStringLiteral("id"))
                                               .toString();
    manager.newSession(firstWorktree);
    QCOMPARE(manager.conversations(firstWorktree).size(), 2);
    QVERIFY(!manager.isRunning(firstWorktree));
    QVERIFY(manager.runningPaths().contains(QFileInfo(firstWorktree).canonicalFilePath()));

    manager.activate(secondWorktree);
    QVERIFY(!manager.isRunning(secondWorktree));

    QTRY_VERIFY_WITH_TIMEOUT(
        !manager.runningPaths().contains(QFileInfo(firstWorktree).canonicalFilePath()), 5000);
    manager.selectConversation(firstWorktree, originalConversationId);
    const QVariantList firstMessages = manager.messages(firstWorktree);
    QCOMPARE(firstMessages.size(), 5);
    const auto payloadScope =
        QFileInfo(firstWorktree).canonicalFilePath() + ":" + originalConversationId;
    const auto payloadKey = firstMessages.at(1).toMap().value("messageKey").toString();
    QCOMPARE(manager.transcriptPayload(firstWorktree, payloadScope, payloadKey, "details")
                 .toMap()
                 .value("output")
                 .toString(),
             QStringLiteral("workspace\n"));
    QVERIFY(
        !manager.transcriptPayload(firstWorktree, "wrong-scope", payloadKey, "details").isValid());
    QVERIFY(
        !manager.transcriptPayload(secondWorktree, payloadScope, payloadKey, "details").isValid());
    QVERIFY(!manager.transcriptPayload(firstWorktree, payloadScope, payloadKey, "raw").isValid());
    QCOMPARE(firstMessages.at(0).toMap().value(QStringLiteral("kind")).toString(),
             QStringLiteral("user"));
    QCOMPARE(firstMessages.at(1).toMap().value(QStringLiteral("kind")).toString(),
             QStringLiteral("tool"));
    QCOMPARE(firstMessages.at(1).toMap().value(QStringLiteral("body")).toString(),
             QStringLiteral("pwd"));
    QCOMPARE(firstMessages.at(1).toMap().value(QStringLiteral("subtype")).toString(),
             QStringLiteral("command"));
    QCOMPARE(firstMessages.at(1).toMap().value(QStringLiteral("command")).toString(),
             QStringLiteral("pwd"));
    QCOMPARE(firstMessages.at(1).toMap().value(QStringLiteral("running")).toBool(), false);
    QCOMPARE(firstMessages.at(2).toMap().value(QStringLiteral("body")).toString(),
             QStringLiteral("cat src/main.cpp"));
    QCOMPARE(firstMessages.at(2).toMap().value(QStringLiteral("subtype")).toString(),
             QStringLiteral("fileRead"));
    QCOMPARE(firstMessages.at(2).toMap().value(QStringLiteral("command")).toString(),
             QStringLiteral("cat src/main.cpp"));
    const auto fileChanges = firstMessages.at(3).toMap().value("fileChanges").toList();
    QCOMPARE(fileChanges.size(), 4);
    QCOMPARE(fileChanges[0].toMap().value("added").toInt(), 3);
    QCOMPARE(fileChanges[0].toMap().value("removed").toInt(), 2);
    QVERIFY(fileChanges[0].toMap().value("known").toBool());
    QCOMPARE(fileChanges[1].toMap().value("added").toInt(), 3);
    QCOMPARE(fileChanges[2].toMap().value("removed").toInt(), 1);
    QVERIFY(!fileChanges[3].toMap().value("known").toBool());
    QCOMPARE(firstMessages.at(4).toMap().value(QStringLiteral("body")).toString(),
             QStringLiteral("Hello world"));
    QVERIFY(activity.count() >= 2);
    QCOMPARE(manager.messages(secondWorktree).size(), 0);

    manager.newSession(firstWorktree);
    QCOMPARE(manager.messages(firstWorktree).size(), 0);
    QCOMPARE(manager.conversations(firstWorktree).size(), 3);
    manager.selectConversation(firstWorktree, originalConversationId);
    QCOMPARE(manager.messages(firstWorktree).size(), 5);
}

void AgentWorkspaceViewTest::configuredTurnsAndAttachments() {
    QTemporaryDir directory;
    const QString scriptPath = directory.filePath("server.py");
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import sys,json
for line in sys.stdin:
    request=json.loads(line)
    method=request.get("method")
    p=request.get("params",{})
    def send(x): print(json.dumps(x),flush=True)
    def result(x): send({"id":request["id"],"result":x})
    if method=="initialize": result({})
    elif method=="model/list": result({"data":[{"model":"test-model","displayName":"Test model","hidden":False,"defaultReasoningEffort":"medium","supportedReasoningEfforts":[{"reasoningEffort":"medium","description":"Balanced"}]}]})
    elif method=="thread/start": result({"thread":{"id":"test-thread","turns":[]}})
    elif method=="thread/resume":
        import os
        result({"thread":{"id":"test-thread","turns":[{"items":[{"type":"userMessage","content":[{"type":"text","text":"Inspect attachments"},{"type":"localImage","path":os.path.join(os.path.dirname(__file__),"sample.png")}]}]}]}})
    elif method=="turn/start":
        result({"turn":{"id":"test-turn"}})
        send({"method":"item/completed","params":{"threadId":"test-thread","item":{"id":"reply","type":"agentMessage","text":json.dumps(p)}}})
        send({"method":"turn/completed","params":{"threadId":"test-thread","turn":{"id":"test-turn","status":"completed"}}})
)PY");
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
    ketplus::CodexAppServer server(scriptPath);
    ketplus::AgentSessionManager manager(&server);
    QSignalSpy catalog(&manager, &ketplus::AgentSessionManager::modelsLoaded);
    manager.loadModels();
    QTRY_COMPARE(catalog.size(), 1);
    const auto model = catalog.at(0).at(0).toList().at(0).toMap();
    QCOMPARE(model.value("provider").toString(), "codex");
    QCOMPARE(model.value("permissions").toList().size(), 3);

    QFile textFile(directory.filePath("note.txt"));
    QVERIFY(textFile.open(QIODevice::WriteOnly));
    textFile.write("Attached reference content");
    textFile.close();
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(Qt::red);
    const auto imagePath = directory.filePath("sample.png");
    QVERIFY(image.save(imagePath));
    QVariantList attachments{QVariantMap{{"path", textFile.fileName()}},
                             QVariantMap{{"path", imagePath}}};
    QVariantMap options{{"provider", "codex"},
                        {"model", "test-model"},
                        {"effort", "medium"},
                        {"permission", "read-only"}};
    auto lastParams = [&] {
        return QJsonDocument::fromJson(manager.messages(directory.path())
                                           .last()
                                           .toMap()
                                           .value("body")
                                           .toString()
                                           .toUtf8())
            .object();
    };
    manager.activate(directory.path());
    const QString conversationId =
        manager.conversations(directory.path()).first().toMap().value("id").toString();
    manager.renameConversation(directory.path(), conversationId, QStringLiteral("My conversation"));
    manager.submitPrompt(directory.path(), "Inspect attachments", options, attachments);
    QTRY_VERIFY(!manager.isRunning(directory.path()));
    QCOMPARE(manager.title(directory.path()), QStringLiteral("My conversation"));
    auto params = lastParams();
    QCOMPARE(params.value("model").toString(), "test-model");
    QCOMPARE(params.value("effort").toString(), "medium");
    QCOMPARE(params.value("sandboxPolicy").toObject().value("type").toString(), "readOnly");
    const auto input = params.value("input").toArray();
    QCOMPARE(input.size(), 2);
    QVERIFY(input[0].toObject().value("text").toString().contains("Attached reference content"));
    QCOMPARE(input[1].toObject().value("type").toString(), "localImage");
    QCOMPARE(input[1].toObject().value("path").toString(), imagePath);
    const auto imageUrl = QUrl::fromLocalFile(imagePath).toString();
    QCOMPARE(manager.messages(directory.path()).first().toMap().value("images").toStringList(),
             QStringList{imageUrl});
    // The turn start time orders the sidebar and survives a relaunch.
    const auto turnStartedAt = [](const ketplus::AgentSessionManager& owner, const QString& path) {
        return owner.conversations(path).first().toMap().value("lastTurnStartedAt").toLongLong();
    };
    const qint64 firstTurnStartedAt = turnStartedAt(manager, directory.path());
    QVERIFY(firstTurnStartedAt > 0);
    {
        ketplus::AgentSessionManager restored(&server);
        restored.activate(directory.path());
        QTRY_VERIFY(!restored.messages(directory.path()).isEmpty());
        QCOMPARE(restored.messages(directory.path()).first().toMap().value("images").toStringList(),
                 QStringList{imageUrl});
        QCOMPARE(turnStartedAt(restored, directory.path()), firstTurnStartedAt);
    }
    options["permission"] = "danger-full-access";
    options["model"] = "second-model";
    QTest::qWait(2);
    manager.submitPrompt(directory.path(), "Next turn", options);
    QTRY_VERIFY(!manager.isRunning(directory.path()));
    QVERIFY(turnStartedAt(manager, directory.path()) > firstTurnStartedAt);
    params = lastParams();
    QCOMPARE(params.value("sandboxPolicy").toObject().value("type").toString(), "dangerFullAccess");
    QCOMPARE(params.value("model").toString(), "second-model");
    QCOMPARE(params.value("input").toArray().size(), 1);
    options["mode"] = "plan";
    manager.submitPrompt(directory.path(), "Add chat planning mode", options);
    QTRY_VERIFY(!manager.isRunning(directory.path()));
    params = lastParams();
    QCOMPARE(params.value("sandboxPolicy").toObject().value("type").toString(), "readOnly");
    const auto planInput =
        params.value("input").toArray().first().toObject().value("text").toString();
    QVERIFY(planInput.contains(QStringLiteral("implementation plan only")));
    QVERIFY(planInput.endsWith(QStringLiteral("Add chat planning mode")));
    options["mode"] = "atlas-builder";
    options["permission"] = "workspace-write";
    QDir().mkpath(directory.filePath("checkout.ketatlas"));
    QFile atlasManifest(directory.filePath("checkout.ketatlas/atlas.json"));
    QVERIFY(atlasManifest.open(QIODevice::WriteOnly));
    atlasManifest.write(R"({"version":1,"title":"Checkout","screens":[],"flows":[]})");
    atlasManifest.close();
    options["atlasPath"] = atlasManifest.fileName();
    options["atlasFlowId"] = "checkout-flow";
    manager.submitPrompt(directory.path(), "Map the checkout journey", options);
    QTRY_VERIFY(!manager.isRunning(directory.path()));
    params = lastParams();
    const auto atlasInput =
        params.value("input").toArray().first().toObject().value("text").toString();
    QVERIFY(atlasInput.contains(QStringLiteral("new `<slug>.ketatlas` directory")));
    QVERIFY(atlasInput.contains(QStringLiteral("valid version 1 `atlas.json`")));
    QVERIFY(atlasInput.contains(QStringLiteral("bundle `checkout.ketatlas/atlas.json`")));
    QVERIFY(atlasInput.contains(QStringLiteral("@Flow `checkout-flow`")));
    QVERIFY(!atlasInput.contains(directory.path()));
    QVERIFY(atlasInput.endsWith(QStringLiteral("Map the checkout journey")));
    const auto atlasMessages = manager.messages(directory.path());
    QCOMPARE(
        atlasMessages.at(atlasMessages.size() - 2).toMap().value(QStringLiteral("body")).toString(),
        QStringLiteral("Map the checkout journey"));
    options.remove("mode");
    options["skills"] = QVariantList{
        QVariantMap{{QStringLiteral("name"), QStringLiteral("reviewer")},
                    {QStringLiteral("path"), QStringLiteral("/tmp/reviewer/SKILL.md")},
                    {QStringLiteral("content"),
                     QStringLiteral("Always prefer focused tests before changing code.")}}};
    manager.submitPrompt(directory.path(), "Use managed skill", options);
    QTRY_VERIFY(!manager.isRunning(directory.path()));
    params = lastParams();
    const auto skillInput =
        params.value("input").toArray().first().toObject().value("text").toString();
    QVERIFY(skillInput.contains(QStringLiteral("KetPlus selected skills")));
    QVERIFY(skillInput.contains(QStringLiteral("Always prefer focused tests")));
    QVERIFY(skillInput.endsWith(QStringLiteral("Use managed skill")));
    const QVariantList skillMessages = manager.messages(directory.path());
    QVERIFY(skillMessages.size() >= 2);
    QCOMPARE(
        skillMessages.at(skillMessages.size() - 2).toMap().value(QStringLiteral("body")).toString(),
        QStringLiteral("Use managed skill"));
    QSignalSpy errors(&manager, &ketplus::AgentSessionManager::errorOccurred);
    options["provider"] = "claude";
    manager.submitPrompt(directory.path(), "Must not use Codex", options);
    QCOMPARE(errors.size(), 1);
    QVERIFY(!manager.isRunning(directory.path()));
    options["provider"] = "cursor";
    manager.submitPrompt(directory.path(), "Must not use Cursor without daemon", options);
    QCOMPARE(errors.size(), 2);
    QVERIFY(!manager.isRunning(directory.path()));
}

void AgentWorkspaceViewTest::doesNotShowEmptyJsonWhenTurnFailsAfterError() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString worktree = directory.filePath(QStringLiteral("worktree"));
    QVERIFY(QDir().mkpath(worktree));
    const QString scriptPath = directory.filePath(QStringLiteral("server.py"));
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import json, sys
for line in sys.stdin:
    request = json.loads(line)
    method = request.get("method")
    def send(value):
        print(json.dumps(value), flush=True)
    def result(value):
        send({"id": request["id"], "result": value})
    if method == "initialize":
        result({})
    elif method == "thread/start":
        result({"thread": {"id": "fail-thread", "turns": []}})
    elif method == "turn/start":
        result({"turn": {"id": "fail-turn"}})
        send({"method": "turn/started", "params": {"threadId": "fail-thread", "turn": {"id": "fail-turn"}}})
        send({"method": "error", "params": {"threadId": "fail-thread", "error": {"message": "Cursor blocked this turn"}}})
        send({"method": "turn/completed", "params": {"threadId": "fail-thread", "turn": {"id": "fail-turn", "status": "failed"}}})
)PY");
    script.close();
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner));
    ketplus::CodexAppServer server(scriptPath);
    ketplus::AgentSessionManager manager(&server);
    manager.activate(worktree);
    manager.submitPrompt(worktree, QStringLiteral("Hit a plan limit"));
    QTRY_VERIFY(!manager.isRunning(worktree));
    const QVariantList messages = manager.messages(worktree);
    QStringList errorBodies;
    for (const QVariant& value : messages) {
        const QVariantMap entry = value.toMap();
        if (entry.value(QStringLiteral("kind")).toString() == QStringLiteral("error"))
            errorBodies.append(entry.value(QStringLiteral("body")).toString());
    }
    QCOMPARE(errorBodies, QStringList{QStringLiteral("Cursor blocked this turn")});
    QVERIFY(!errorBodies.contains(QStringLiteral("{}")));
}

void AgentWorkspaceViewTest::promotesConversationWhenAgentStarts() {
    ketplus::AgentWorkspaceView view;
    QVariantMap first{{"id", "first"}, {"active", true}, {"running", false}};
    QVariantMap second{{"id", "second"}, {"active", false}, {"running", false}};
    view.setConversations({first, second});
    first["active"] = false;
    second["active"] = true;
    view.setConversations({first, second});
    QCOMPARE(view.conversations().first().toMap().value("id").toString(), QStringLiteral("first"));
    second["running"] = true;
    view.setConversations({first, second});
    QCOMPARE(view.conversations().first().toMap().value("id").toString(), QStringLiteral("second"));
    first["running"] = true;
    view.setConversations({first, second});
    QCOMPARE(view.conversations().first().toMap().value("id").toString(), QStringLiteral("first"));
    view.setConversations({first, second});
    QCOMPARE(view.conversations().first().toMap().value("id").toString(), QStringLiteral("first"));
    first["running"] = false;
    view.setConversations({first, second});
    QCOMPARE(view.conversations().first().toMap().value("id").toString(), QStringLiteral("first"));
}

void AgentWorkspaceViewTest::workingIndicatorClockLifecycle() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(900, 640);
    view.setSessionState("Animation trial", "Working…", true);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
    view.quickView()->requestActivate();
    auto* root = view.quickView()->rootObject();
    auto* icon = root->findChild<QQuickItem*>("workingAnimationIcon");
    auto* label = root->findChild<QQuickItem*>("workingStatusLabel");
    QVERIFY(icon && label);
    QVERIFY(view.workingMotionEnabled());
    QVERIFY(!view.motionEnabled()); // Sidebar and restore animations stay off.
    QTRY_VERIFY(icon->property("holdsClock").toBool());
    const auto labelPosition = label->mapToScene(QPointF{});
    QTest::qWait(160);
    QCOMPARE(icon->scale(), 1.0);
    QCOMPARE(label->opacity(), 1.0);
    QCOMPARE(label->mapToScene(QPointF{}), labelPosition);
    const auto capture = qEnvironmentVariable("KETPLUS_WORKING_CAPTURE");
    if (!capture.isEmpty()) {
        const auto first = view.quickView()->grabWindow();
        QVERIFY(!first.isNull());
        QTest::qWait(240);
        const auto second = view.quickView()->grabWindow();
        const qreal ratio = qreal(first.width()) / view.quickView()->width();
        const QRectF iconBounds(icon->mapToScene(QPointF{}), QSizeF(icon->width(), icon->height()));
        const QRect crop(qRound(iconBounds.x() * ratio), qRound(iconBounds.y() * ratio),
                         qRound(iconBounds.width() * ratio), qRound(iconBounds.height() * ratio));
        QVERIFY(first.copy(crop) != second.copy(crop));
        QVERIFY(first.save(capture + "-first.png"));
        QVERIFY(second.save(capture + "-second.png"));
    }
    view.setRestoring(true);
    QTRY_VERIFY(!icon->property("holdsClock").toBool());
    view.setRestoring(false);
    QTRY_VERIFY(icon->property("holdsClock").toBool());
    view.setSessionState("Animation trial", "Ready", false);
    QTRY_VERIFY(!icon->property("holdsClock").toBool());
    view.setSessionState("Animation trial", "Working…", true);
    QTRY_VERIFY(icon->property("holdsClock").toBool());
    view.hide();
    QTRY_VERIFY(!icon->property("holdsClock").toBool());
    qputenv("KETPLUS_REDUCED_MOTION", "1");
    ketplus::AgentWorkspaceView reduced;
    qunsetenv("KETPLUS_REDUCED_MOTION");
    QVERIFY(!reduced.workingMotionEnabled());
}

void AgentWorkspaceViewTest::animatesRunningConversationStatus() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    QVariantMap conversation{{"id", "animated"},
                             {"title", "Animation test"},
                             {"status", "Working"},
                             {"active", true},
                             {"running", true}};
    view.setConversations({conversation});
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    const auto row = [&]() {
        return findVisualItem(root, QStringLiteral("conversationRow_animated"));
    };
    QTRY_VERIFY(row());
    QVERIFY(!view.motionEnabled());
    QTRY_VERIFY(!row()->property("holdsClock").toBool());
    conversation["status"] = "Reasoning";
    view.setConversations({conversation});
    QTRY_VERIFY(row() && !row()->property("holdsClock").toBool());
    view.hide();
    QTRY_VERIFY(!row()->property("holdsClock").toBool());
    view.show();
    QTRY_VERIFY(!row()->property("holdsClock").toBool());
    conversation["running"] = false;
    conversation["status"] = "Ready";
    view.setConversations({conversation});
    QTRY_VERIFY(row() && !row()->property("holdsClock").toBool());
}

void AgentWorkspaceViewTest::restoresConversationScrollPosition() {
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    const auto activeTranscript = [&] {
        return root->findChild<QQuickItem*>(QStringLiteral("agentTranscript"));
    };
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QVariantList history;
    for (int i = 0; i < 120; ++i) {
        history.append(QVariantMap{
            {"kind", "assistant"},
            {"meta", "Codex"},
            {"body",
             QStringLiteral("Message %1\n\nSome text to read.\n\nAnother paragraph.").arg(i)},
            {"filePath", ""}});
    }
    const auto capture = [&] {
        QVariant result;
        const bool invoked = QMetaObject::invokeMethod(root, "captureTranscriptPosition",
                                                       Q_RETURN_ARG(QVariant, result));
        Q_ASSERT(invoked);
        return result.toMap();
    };
    view.resetMessages(history, "workspace:first");
    QVERIFY(transcript->opacity() == 0);
    QTRY_VERIFY(transcript->opacity() == 1);
    QVERIFY(transcript->property("followTail").toBool());
    transcript->setProperty("followTail", false);
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtIndex", Q_ARG(int, 40),
                                      Q_ARG(int, 0)));
    QTest::qWait(100);
    const auto firstPosition = capture();
    QVERIFY(firstPosition.value("index").toInt() > 0);
    auto* firstTranscript = transcript;

    view.resetMessages(history, "workspace:second");
    transcript = activeTranscript();
    QVERIFY(transcript);
    QTest::qWait(100);
    QVERIFY(transcript->property("followTail").toBool());
    view.resetMessages(history, "workspace:first");
    transcript = activeTranscript();
    QCOMPARE(transcript, firstTranscript);
    QCOMPARE(transcript->opacity(), 1.0);
    QVERIFY(!transcript->property("followTail").toBool());
    QCOMPARE(capture().value("index"), firstPosition.value("index"));
    QVERIFY(qAbs(capture().value("offset").toDouble() - firstPosition.value("offset").toDouble()) <
            2);
    for (int frame = 0; frame < 12; ++frame) {
        QTest::qWait(16);
        QCOMPARE(capture().value("index"), firstPosition.value("index"));
        QVERIFY(qAbs(capture().value("offset").toDouble() -
                     firstPosition.value("offset").toDouble()) < 2);
    }

    view.setSessionState("First", "Working", true);
    view.appendMessage(history.first().toMap());
    QTest::qWait(100);
    QVERIFY(!transcript->property("followTail").toBool());
    QCOMPARE(capture().value("index"), firstPosition.value("index"));

    view.resetMessages(history, "workspace:second");
    transcript = activeTranscript();
    view.resetMessages(history, "workspace:first");
    transcript = activeTranscript();
    QTest::qWait(150);
    QCOMPARE(capture().value("index"), firstPosition.value("index"));
    view.resetMessages(history, "workspace:second");
    transcript = activeTranscript();
    QTest::qWait(100);
    QVERIFY(transcript->property("followTail").toBool());
}

void AgentWorkspaceViewTest::boundsWarmConversationRenderCache() {
    ketplus::AgentWorkspaceView firstView;
    ketplus::AgentWorkspaceView secondView;
    firstView.applyTheme(testPalette());
    secondView.applyTheme(testPalette());
    firstView.resize(800, 600);
    secondView.resize(800, 600);
    firstView.show();
    secondView.show();

    const auto transcriptForScope = [](ketplus::AgentWorkspaceView& view,
                                       const QString& scope) -> QQuickItem* {
        for (auto* item : view.quickView()->rootObject()->findChildren<QQuickItem*>()) {
            if (item->property("cacheScope").toString() == scope &&
                (item->objectName() == QStringLiteral("agentTranscript") ||
                 item->objectName() == QStringLiteral("cachedAgentTranscript"))) {
                return item;
            }
        }
        return nullptr;
    };
    const auto renderCount = [](ketplus::AgentWorkspaceView& view) {
        int count = 0;
        for (auto* item : view.quickView()->rootObject()->findChildren<QQuickItem*>()) {
            if (item->property("cacheScope").isValid())
                ++count;
        }
        return count;
    };
    const QVariantList message{
        QVariantMap{{"kind", "assistant"}, {"body", "Warm transcript"}, {"meta", "Codex"}}};

    firstView.resetMessages(message, QStringLiteral("first:0"));
    QPointer<ketplus::AgentMessageModel> oldestModel = firstView.messageModel();
    for (int index = 1; index < 5; ++index)
        firstView.resetMessages(message, QStringLiteral("first:%1").arg(index));
    QPointer<QQuickItem> firstViewport = transcriptForScope(firstView, QStringLiteral("first:4"));
    QVERIFY(firstViewport);
    for (int index = 0; index < 5; ++index)
        secondView.resetMessages(message, QStringLiteral("second:%1").arg(index));

    QTRY_VERIFY(oldestModel.isNull());
    QTRY_COMPARE(renderCount(firstView) + renderCount(secondView), 2);
    QVERIFY(transcriptForScope(firstView, QStringLiteral("first:4")));
    QVERIFY(transcriptForScope(secondView, QStringLiteral("second:4")));
    firstView.resetMessages(message, QStringLiteral("first:0"));
    QVERIFY(firstView.messageModel());
    QCOMPARE(firstView.messageModel()->count(), 1);
    QCOMPARE(firstView.messageModel()->get(0).value("body"), "Warm transcript");
    QTRY_COMPARE(renderCount(firstView) + renderCount(secondView), 2);
    QVERIFY(transcriptForScope(firstView, QStringLiteral("first:0")));
    firstView.resetMessages(message, QStringLiteral("first:3"));
    QCOMPARE(transcriptForScope(firstView, QStringLiteral("first:3")), firstViewport.data());
    QCOMPARE(renderCount(firstView) + renderCount(secondView), 2);
}

void AgentWorkspaceViewTest::streamingPreservesReadingPosition_data() {
    QTest::addColumn<int>("paragraphs");
    QTest::addColumn<int>("distanceFromBottom");
    QTest::newRow("outside-tail-buffer") << 50 << 64;
    QTest::newRow("reading-above") << 50 << 160;
    QTest::newRow("long-near-bottom") << 500 << 160;
    QTest::newRow("long-scrolled-up") << 500 << 1200;
}

void AgentWorkspaceViewTest::streamingPreservesReadingPosition() {
    QFETCH(int, paragraphs);
    QFETCH(int, distanceFromBottom);
    ketplus::AgentWorkspaceView view;
    view.applyTheme(testPalette());
    view.resize(1000, 700);
    view.show();
    auto* root = view.quickView()->rootObject();
    QVERIFY(root);
    auto* transcript = root->findChild<QQuickItem*>("agentTranscript");
    QVERIFY(transcript);
    QString body;
    for (int i = 0; i < paragraphs; ++i)
        body += QStringLiteral("Paragraph %1 with **bold** text.\n\n").arg(i);
    QVariantMap message{{"kind", "assistant"}, {"meta", "Codex"}, {"body", body}, {"filePath", ""}};
    view.setSessionState("Streaming", "Working", true);
    view.resetMessages({message}, "workspace:streaming");
    QTRY_COMPARE(transcript->opacity(), 1.0);
    QTest::qWait(100);
    QVERIFY(QMetaObject::invokeMethod(transcript, "positionViewAtEnd"));
    const double readingY = transcript->property("originY").toDouble() +
                            transcript->property("contentHeight").toDouble() -
                            transcript->height() - distanceFromBottom;
    QVERIFY(readingY > transcript->property("originY").toDouble());
    transcript->setProperty("contentY", readingY);
    QVERIFY(QMetaObject::invokeMethod(transcript, "movementEnded"));
    QVERIFY(!transcript->property("followTail").toBool());
    QSignalSpy scrollChanges(transcript, SIGNAL(contentYChanged()));
    const auto findBody = [&](const auto& self, QQuickItem* item) -> QQuickItem* {
        if (item->objectName() == "messageBody")
            return item;
        for (auto* child : item->childItems()) {
            if (auto* found = self(self, child))
                return found;
        }
        return nullptr;
    };
    auto* bodyItem = findBody(findBody, root);
    QVERIFY(bodyItem);
    auto* quickDocument = qvariant_cast<QQuickTextDocument*>(bodyItem->property("textDocument"));
    QVERIFY(quickDocument);
    // Streaming must preserve the current kit typography, including custom zoom.
    const qreal lineHeight = quickDocument->textDocument()->firstBlock().blockFormat().lineHeight();
    QVERIFY(lineHeight > 0);
    auto capturePath = qEnvironmentVariable("KETPLUS_CHAT_CAPTURE");
    if (!capturePath.isEmpty())
        capturePath += QStringLiteral(".") + QTest::currentDataTag();
    QImage reference;
    QRect readingRegion;
    int capturedFrames = 0;
    int maximumChannelDifference = 0;
    if (!capturePath.isEmpty()) {
        QVERIFY(QTest::qWaitForWindowExposed(view.quickView()));
        QTest::qWait(100);
        const auto frame = view.quickView()->grabWindow();
        QVERIFY(!frame.isNull());
        const auto origin = transcript->mapToScene(QPointF(24, 24));
        const auto scale = frame.devicePixelRatio();
        readingRegion = QRect(qRound(origin.x() * scale), qRound(origin.y() * scale),
                              qRound((transcript->width() - 48) * scale), qRound(120 * scale));
        reference = frame.copy(readingRegion);
        QVERIFY(reference.save(capturePath + ".reference.png"));
    }
    const QStringList deltas{"More text",  " and more.\n\n", "```mermaid\n",
                             "graph TD\n", "A --> B\n",      "```\n"};
    for (const auto& delta : deltas) {
        body += delta;
        message["body"] = body;
        view.updateMessage(0, message);
        // Do not leave an unformatted document for a later event-loop turn to repair.
        QCOMPARE(quickDocument->textDocument()->firstBlock().blockFormat().lineHeight(),
                 lineHeight);
        for (int frameIndex = 0; frameIndex < 6; ++frameIndex) {
            QTest::qWait(8);
            if (!reference.isNull()) {
                const auto frame = view.quickView()->grabWindow();
                QVERIFY(!frame.isNull());
                const auto reading = frame.copy(readingRegion);
                ++capturedFrames;
                if (reading != reference) {
                    int changedPixels = 0;
                    QRect changedBounds;
                    for (int y = 0; y < reading.height(); ++y) {
                        for (int x = 0; x < reading.width(); ++x) {
                            const auto actual = reading.pixelColor(x, y);
                            const auto expected = reference.pixelColor(x, y);
                            const int difference =
                                qMax(qAbs(actual.red() - expected.red()),
                                     qMax(qAbs(actual.green() - expected.green()),
                                          qAbs(actual.blue() - expected.blue())));
                            maximumChannelDifference = qMax(maximumChannelDifference, difference);
                            // Allow rounding noise in native glyph antialiasing, not missing/moved
                            // text.
                            if (difference <= 2)
                                continue;
                            ++changedPixels;
                            changedBounds |= QRect(x, y, 1, 1);
                        }
                    }
                    if (changedPixels > 0) {
                        QVERIFY(frame.save(capturePath + ".changed.png"));
                        QVERIFY(reading.save(capturePath + ".changed-region.png"));
                        qWarning() << "Changed pixels" << changedPixels << "bounds" << changedBounds
                                   << "max channel difference" << maximumChannelDifference;
                        QFAIL("Visible reading region changed during streaming; frame saved");
                    }
                }
            }
        }
        QVERIFY(!transcript->property("followTail").toBool());
        QVERIFY(qAbs(transcript->property("contentY").toDouble() - readingY) < 1);
        QCOMPARE(transcript->opacity(), 1.0);
    }
    QCOMPARE(scrollChanges.count(), 0);
    if (capturedFrames > 0)
        qInfo() << "Compared frames" << capturedFrames << "max channel difference"
                << maximumChannelDifference;
}
