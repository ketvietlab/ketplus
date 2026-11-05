#include "AgentWorkspaceViewTest.h"
#include "agent/AgentMessageFormatter.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

void AgentWorkspaceViewTest::missingWorktreeKeepsHistoryAndCanStop() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath("worktree");
    QVERIFY(QDir().mkdir(path));
    const QString scriptPath = directory.filePath("fake-codex");
    QFile script(scriptPath);
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import json,sys
for line in sys.stdin:
    r=json.loads(line)
    def send(v): print(json.dumps(v),flush=True)
    def result(v): send({"id":r["id"],"result":v})
    m=r.get("method")
    if m=="initialize": result({})
    elif m=="thread/start": result({"thread":{"id":"missing-thread","turns":[]}})
    elif m=="turn/start":
        result({"turn":{"id":"running-turn"}})
        send({"method":"turn/started","params":{"threadId":"missing-thread","turn":{"id":"running-turn"}}})
    elif m=="turn/interrupt":
        result({})
        send({"method":"turn/completed","params":{"threadId":"missing-thread","turn":{"id":"running-turn","status":"interrupted"}}})
)PY");
    script.close();
    QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner));
    ketplus::CodexAppServer server(scriptPath);
    ketplus::AgentSessionManager manager(&server);
    manager.submitPrompt(path, "Keep running until interrupted");
    QTRY_COMPARE(manager.status(path), QStringLiteral("Working…"));
    const QString id = manager.activeConversationId(path);
    const auto messages = manager.messages(path);
    QVERIFY(!messages.isEmpty());
    QVERIFY(QDir().rmdir(path));

    QSignalSpy reset(&manager, &ketplus::AgentSessionManager::messagesReset);
    manager.activate(path);
    QCOMPARE(reset.size(), 1);
    QCOMPARE(manager.activeConversationId(path), id);
    QCOMPARE(manager.messages(path), messages);

    QSignalSpy errors(&manager, &ketplus::AgentSessionManager::errorOccurred);
    QSignalSpy steering(&manager, &ketplus::AgentSessionManager::steeringFinished);
    manager.submitPrompt(path, "Do not send");
    manager.steerPrompt(path, "Do not steer");
    manager.newSession(path);
    QCOMPARE(errors.size(), 3);
    for (const auto& error : errors) {
        QVERIFY(error.first().toString().contains("Worktree missing"));
    }
    QCOMPARE(steering.size(), 1);
    QVERIFY(!steering.first().last().toBool());
    QCOMPARE(manager.activeConversationId(path), id);
    QCOMPARE(manager.messages(path), messages);

    ketplus::AgentWorkspaceView view;
    ketplus::GitWorktree worktree;
    worktree.path = path;
    worktree.branch = "feature";
    worktree.prunable = true;
    view.setWorktrees({worktree}, path, manager.runningPaths());
    QCOMPARE(view.worktrees().first().toMap().value("status").toString(),
             QStringLiteral("Worktree missing"));
    QVERIFY(view.worktrees().first().toMap().value("running").toBool());

    manager.cancel(path);
    QTRY_VERIFY(!manager.isRunning(path));
    QVERIFY(!manager.messages(path).isEmpty());
    QVERIFY(QDir().mkdir(path));
    view.setWorktrees({worktree}, path, manager.runningPaths());
    QCOMPARE(view.worktrees().first().toMap().value("status").toString(), QStringLiteral("Ready"));
}

void AgentWorkspaceViewTest::missingWorktreeRestoresSavedAndDaemonSessions() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = QDir(QFileInfo(directory.path()).canonicalFilePath()).filePath("worktree");
    QVERIFY(QDir().mkdir(path));
    ketplus::CodexAppServer server(QStringLiteral("/nonexistent/fake-codex"));
    QString id;
    {
        ketplus::AgentSessionManager manager(&server);
        manager.activate(path);
        id = manager.activeConversationId(path);
        manager.renameConversation(path, id, "Saved before removal");
    }
    QVERIFY(QDir().rmdir(path));
    ketplus::AgentSessionManager restored(&server);
    restored.activate(path);
    QCOMPARE(restored.activeConversationId(path), id);
    QCOMPARE(restored.title(path), QStringLiteral("Saved before removal"));

    const QString daemonPath = directory.filePath("also-missing");
    emit server.daemonSessionDiscovered({{"session_id", "missing-daemon-session"},
                                         {"worktree_path", daemonPath},
                                         {"state", "running"}});
    restored.activate(daemonPath);
    QCOMPARE(restored.activeConversationId(daemonPath), QStringLiteral("missing-daemon-session"));
    QVERIFY(restored.isRunning(daemonPath));
    QVERIFY(!restored.status(daemonPath).contains(QStringLiteral("not found")));
    QVERIFY(!restored.status(daemonPath).isEmpty());
    restored.activate(directory.filePath("unknown-missing"));
    QVERIFY(restored.conversations(directory.filePath("unknown-missing")).isEmpty());
}

void AgentWorkspaceViewTest::restoresMetadataBeforeDaemonDiscovery() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = QFileInfo(directory.path()).canonicalFilePath();
    const QString settingsKey = ketplus::agent_message::persistedKey(path);
    QSettings().setValue(
        settingsKey,
        QVariantMap{{QStringLiteral("path"), path},
                    {QStringLiteral("activeId"), QStringLiteral("ui-custom")},
                    {QStringLiteral("conversations"),
                     QVariantList{
                         QVariantMap{{QStringLiteral("id"), QStringLiteral("ui-derived")},
                                     {QStringLiteral("threadId"), QStringLiteral("daemon-derived")},
                                     {QStringLiteral("title"), QStringLiteral("New conversation")},
                                     {QStringLiteral("customTitle"), false}},
                         QVariantMap{{QStringLiteral("id"), QStringLiteral("ui-custom")},
                                     {QStringLiteral("threadId"), QStringLiteral("daemon-custom")},
                                     {QStringLiteral("title"), QStringLiteral("Keep my title")},
                                     {QStringLiteral("customTitle"), true}}}}});

    ketplus::CodexAppServer server(QStringLiteral("/nonexistent/fake-codex"));
    ketplus::AgentSessionManager manager(&server);
    QSignalSpy activity(&manager, &ketplus::AgentSessionManager::activityChanged);
    emit server.daemonSessionDiscovered(
        {{QStringLiteral("session_id"), QStringLiteral("daemon-derived")},
         {QStringLiteral("worktree_path"), path},
         {QStringLiteral("title"), QStringLiteral("Recovered daemon title")},
         {QStringLiteral("state"), QStringLiteral("ready")}});
    emit server.daemonSessionDiscovered(
        {{QStringLiteral("session_id"), QStringLiteral("daemon-custom")},
         {QStringLiteral("worktree_path"), path},
         {QStringLiteral("title"), QStringLiteral("Do not overwrite custom title")},
         {QStringLiteral("state"), QStringLiteral("ready")}});

    const QVariantList conversations = manager.conversations(path);
    QCOMPARE(conversations.size(), 2);
    QCOMPARE(conversations.at(0).toMap().value(QStringLiteral("id")).toString(),
             QStringLiteral("ui-derived"));
    QCOMPARE(conversations.at(0).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Recovered daemon title"));
    QCOMPARE(conversations.at(1).toMap().value(QStringLiteral("id")).toString(),
             QStringLiteral("ui-custom"));
    QCOMPARE(conversations.at(1).toMap().value(QStringLiteral("title")).toString(),
             QStringLiteral("Keep my title"));
    QCOMPARE(manager.activeConversationId(path), QStringLiteral("ui-custom"));
    QVERIFY(manager.conversationPaths().contains(path));
    QVERIFY(!activity.isEmpty());

    QSettings().remove(settingsKey);
}
