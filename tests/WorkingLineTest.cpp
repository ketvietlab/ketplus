#include "agent/AgentSessionManager.h"
#include "agent/CodexAppServer.h"
#include "agent/ReasoningHeading.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using ketplus::agent_message::reasoningHeading;
using ketplus::agent_message::workingLine;

// #385: the running line names the latest reasoning heading, "Working · profiling …".
class WorkingLineTest final : public QObject {
    Q_OBJECT

  private slots:
    void readsOnlyTheLatestBoldHeading() {
        const QJsonObject codex{
            {"type", "reasoning"},
            {"summary", QJsonArray{"**Reading the settings**\n\nThe dialog reparents its pages.",
                                   "**Profiling the provider handshake**\n\nsecret plan"}}};
        QCOMPARE(reasoningHeading(codex), QStringLiteral("profiling the provider handshake"));
        // Responses-style parts carry their text in an object.
        const QJsonObject parts{
            {"summary", QJsonArray{QJsonObject{{"type", "summary_text"},
                                               {"text", "**Checking the API keys**\n\nbody"}}}}};
        QCOMPARE(reasoningHeading(parts), QStringLiteral("checking the API keys"));
        // An acronym keeps its case.
        QCOMPARE(reasoningHeading({{"summary", QJsonArray{"**API limits**"}}}),
                 QStringLiteral("API limits"));
        // No bold heading, a heading later in the text, or no summary: nothing to show.
        QVERIFY(reasoningHeading({{"summary", QJsonArray{"Plain thought"}}}).isEmpty());
        QVERIFY(reasoningHeading({{"summary", QJsonArray{"Text **not a heading**"}}}).isEmpty());
        QVERIFY(reasoningHeading({{"text", "**Hidden**"}}).isEmpty());
        // A later part without a heading keeps the earlier one.
        QCOMPARE(reasoningHeading({{"summary", QJsonArray{"**Planning**", "more body"}}}),
                 QStringLiteral("planning"));
    }

    void composesTheRunningLine() {
        QCOMPARE(workingLine("Working…", "profiling"), QStringLiteral("Working · profiling"));
        QCOMPARE(workingLine("Reasoning…", "profiling"), QStringLiteral("Working · profiling"));
        QCOMPARE(workingLine("Working…", {}), QStringLiteral("Working…"));
        // Other states keep their own wording.
        QCOMPARE(workingLine("Compacting context…", "profiling"),
                 QStringLiteral("Compacting context…"));
        QCOMPARE(workingLine("Waiting for your answer…", "profiling"),
                 QStringLiteral("Waiting for your answer…"));
    }

    void followsTheTurnFromAThoughtToItsEnd() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString worktree = directory.filePath("worktree");
        QVERIFY(QDir().mkpath(worktree));
        const QString serverPath = directory.filePath("fake-codex");
        QFile server(serverPath);
        QVERIFY(server.open(QIODevice::WriteOnly));
        const QByteArray source = R"PY(#!/usr/bin/python3
import json
import sys
import time

def send(value):
    print(json.dumps(value, separators=(",", ":")), flush=True)

for line in sys.stdin:
    message = json.loads(line)
    method = message.get("method")
    request_id = message.get("id")
    params = message.get("params", {})
    if method == "initialize":
        send({"id": request_id, "result": {"codexHome": "/tmp", "platformFamily": "unix", "platformOs": "macos", "userAgent": "fake"}})
    elif method == "thread/start":
        send({"id": request_id, "result": {"thread": {"id": "thread-1", "turns": []}}})
    elif method == "turn/start":
        send({"id": request_id, "result": {"turn": {"id": "turn-1", "status": "inProgress", "items": []}}})
        send({"method": "turn/started", "params": {"threadId": "thread-1", "turn": {"id": "turn-1", "status": "inProgress", "items": []}}})
        thought = {"id": "thought-1", "type": "reasoning", "summary": [], "content": []}
        send({"method": "item/started", "params": {"threadId": "thread-1", "turnId": "turn-1", "item": thought}})
        thought["summary"] = ["**Profiling the provider handshake**\n\nsecret plan"]
        send({"method": "item/completed", "params": {"threadId": "thread-1", "turnId": "turn-1", "item": thought}})
        time.sleep(0.3)
        send({"method": "turn/completed", "params": {"threadId": "thread-1", "turn": {"id": "turn-1", "status": "completed", "items": []}}})
)PY";
        QCOMPARE(server.write(source), source.size());
        server.close();
        QVERIFY(QFile::setPermissions(serverPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                      QFileDevice::ExeOwner));

        ketplus::CodexAppServer appServer(serverPath);
        ketplus::AgentSessionManager manager(&appServer);
        QSignalSpy details(&manager, &ketplus::AgentSessionManager::workingDetailChanged);
        manager.submitPrompt(worktree, QStringLiteral("Profile it"));
        QTRY_COMPARE_WITH_TIMEOUT(manager.workingDetail(worktree),
                                  QStringLiteral("profiling the provider handshake"), 3000);
        QVERIFY(manager.isRunning(worktree));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.isRunning(worktree), 3000);
        // The turn's end clears it, and the thought's body never reached the line.
        QVERIFY(manager.workingDetail(worktree).isEmpty());
        QVERIFY(!details.isEmpty());
        QCOMPARE(details.constLast().at(1).toString(), QString());
        for (const auto& emitted : std::as_const(details))
            QVERIFY(!emitted.at(1).toString().contains(QStringLiteral("secret")));
    }
};

QTEST_MAIN(WorkingLineTest)
#include "WorkingLineTest.moc"
