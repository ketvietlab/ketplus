#include "KetSidebarTree.h"
#include "agent/AgentSessionManager.h"
#include "agent/CodexAppServer.h"
#include "app/WaitingBadge.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"
#include "workspace/SidebarRepositoryListDetail.h"
#include "workspace/SymbolIcons.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

#include <algorithm>

using namespace ketplus;

class ConversationAttentionTest final : public QObject {
    Q_OBJECT
  private slots:
    void pendingInput_data() {
        QTest::addColumn<bool>("blocking");
        QTest::addColumn<bool>("approval");
        QTest::newRow("blocking-question") << true << false;
        QTest::newRow("async-question") << false << false;
        QTest::newRow("confirmation") << true << true;
        QTest::newRow("async-confirmation") << false << true;
    }

    void pendingInput() {
        QFETCH(bool, blocking);
        QFETCH(bool, approval);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath("fake-codex");
        QFile file(executable);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"PY(#!/usr/bin/python3
import json, sys
def send(value): print(json.dumps(value), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    method, rid = request.get("method"), request.get("id")
    if method == "initialize": send({"id":rid,"result":{}})
    elif method == "thread/start":
        send({"id":rid,"result":{"thread":{"id":"attention-thread","turns":[]}}})
    elif method == "turn/start":
        send({"id":rid,"result":{"turn":{"id":"attention-turn"}}})
        send({"method":"turn/started","params":{"threadId":"attention-thread","turn":{"id":"attention-turn"}}})
        item = {"id":"ready","type":"agentMessage","phase":"commentary","text":"Ready for a synthetic question"}
        send({"method":"item/started","params":{"threadId":"attention-thread","turnId":"attention-turn","item":item}})
        send({"method":"item/completed","params":{"threadId":"attention-thread","turnId":"attention-turn","item":item}})
)PY");
        file.close();
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner));
        CodexAppServer server(executable);
        AgentSessionManager manager(&server);
        ActivityBar bar;
        bar.show();
        const QString path = QFileInfo(directory.path()).canonicalFilePath();
        connect(&manager, &AgentSessionManager::conversationsChanged, &bar,
                [&](const QString& changedPath, const QVariantList& conversations) {
                    if (changedPath == path)
                        bar.setRepositories({{path, "Repository", true, conversations}});
                });
        QSignalSpy changes(&manager, &AgentSessionManager::conversationsChanged);
        manager.submitPrompt(path, "Synthetic attention test");
        QTRY_COMPARE(manager.messages(path).size(), 2);
        ket::SidebarRow* icon = nullptr;
        QTRY_VERIFY((icon = bar.findChild<ket::SidebarRow*>("productConversationButton")));
        QVERIFY((icon->status() == ket::ConversationStatus::Working));

        // This conversation is now in the background; attention must still update its row.
        manager.activate(directory.filePath("other"));
        const auto request = [&](const QString& id) {
            server.userInputRequested(
                id,
                {{"threadId", "attention-thread"},
                 {"turnId", "attention-turn"},
                 {"itemId", id},
                 {"isBlocking", blocking},
                 {"approvalKind", approval ? "approval" : "question"},
                 {"questions",
                  QJsonArray{QJsonObject{
                      {"id", "q"},
                      {"question", "Continue?"},
                      {"options", QJsonArray{QJsonObject{{"id", "yes"}, {"label", "Yes"}}}}}}}});
        };
        const auto paused = [&] { return icon->status() == ket::ConversationStatus::NeedsInput; };
        changes.clear();
        request("first");
        QVERIFY(!changes.isEmpty());
        QVERIFY(manager.conversations(path).first().toMap().value("needsInput").toBool());
        QVERIFY(manager.isRunning(path));
        QVERIFY(paused());
        QCOMPARE(icon->accessibleDescription(), "Needs input");
        if (!blocking)
            QCOMPARE(manager.status(path), "Working…");
        const QString key = manager.messages(path).last().toMap().value("messageKey").toString();
        request("second");
        manager.answerQuestion(path, key, {{"q", QVariantMap{{"option_ids", QStringList{"yes"}}}}});
        QVERIFY(paused()); // Answering one request must not clear another pending request.
        changes.clear();
        server.notificationReceived(
            "userInput/resolved",
            {{"threadId", "attention-thread"}, {"requestId", "second"}, {"expired", true}});
        QVERIFY(!changes.isEmpty());
        QVERIFY(!manager.conversations(path).first().toMap().value("needsInput").toBool());
        QVERIFY((icon->status() == ket::ConversationStatus::Working));
        request("answer-only");
        QVERIFY(paused());
        const QString answerKey =
            manager.messages(path).last().toMap().value("messageKey").toString();
        manager.answerQuestion(path, answerKey,
                               {{"q", QVariantMap{{"option_ids", QStringList{"yes"}}}}});
        QVERIFY((icon->status() == ket::ConversationStatus::Working));
        request("third");
        QVERIFY(paused());
        server.notificationReceived("daemon/approvalEnded",
                                    {{"threadId", "attention-thread"}, {"approvalId", "third"}});
        QVERIFY((icon->status() == ket::ConversationStatus::Working));
        request("completed-pending");
        QVERIFY(paused());
        server.notificationReceived(
            "turn/completed",
            {{"threadId", "attention-thread"},
             {"turn", QJsonObject{{"id", "attention-turn"}, {"status", "completed"}}}});
        QVERIFY(!(icon->status() == ket::ConversationStatus::Working));
        QCOMPARE(icon->status(), ket::ConversationStatus::Completed);
        QVERIFY(!manager.conversations(path).first().toMap().value("awaitingReply").toBool());
    }

    // #125: a finished turn the daemon marks `awaiting_reply` reads "Waiting for you" until the
    // next turn starts; the Dock badge counts it apart from unread completions.
    void awaitingReplyFromCompletedTurn() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath("fake-codex");
        QFile file(executable);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"PY(#!/usr/bin/python3
import json, sys
def send(value): print(json.dumps(value), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    method, rid = request.get("method"), request.get("id")
    if method == "initialize": send({"id":rid,"result":{}})
    elif method == "thread/start":
        send({"id":rid,"result":{"thread":{"id":"waiting-thread","turns":[]}}})
    elif method == "turn/start":
        send({"id":rid,"result":{"turn":{"id":"waiting-turn"}}})
        send({"method":"turn/started","params":{"threadId":"waiting-thread","turn":{"id":"waiting-turn"}}})
        item = {"id":"note-%d" % rid,"type":"agentMessage","phase":"commentary","text":"Working"}
        send({"method":"item/completed","params":{"threadId":"waiting-thread","turnId":"waiting-turn","item":item}})
)PY");
        file.close();
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner));
        CodexAppServer server(executable);
        AgentSessionManager manager(&server);
        ActivityBar bar;
        const QString path = QFileInfo(directory.path()).canonicalFilePath();
        connect(&manager, &AgentSessionManager::conversationsChanged, &bar,
                [&](const QString& changedPath, const QVariantList& conversations) {
                    if (changedPath == path)
                        bar.setRepositories({{path, "Repository", true, conversations}});
                });
        manager.submitPrompt(path, "Synthetic waiting test");
        QTRY_COMPARE(manager.messages(path).size(), 2);
        const auto complete = [&](bool awaiting) {
            server.notificationReceived("turn/completed",
                                        {{"threadId", "waiting-thread"},
                                         {"turn", QJsonObject{{"id", "waiting-turn"},
                                                              {"status", "completed"},
                                                              {"awaitingReply", awaiting}}}});
        };
        complete(true);
        QTRY_VERIFY(!manager.isRunning(path));
        const auto row = [&] { return manager.conversations(path).first().toMap(); };
        QVERIFY(row().value("awaitingReply").toBool());
        ket::SidebarRow* icon = nullptr;
        QTRY_VERIFY((icon = bar.findChild<ket::SidebarRow*>("productConversationButton")));
        QCOMPARE(icon->status(), ket::ConversationStatus::AwaitingReply);

        auto& badge = WaitingBadge::instance();
        QObject first;
        QObject second;
        badge.report(&first, path, WaitingBadge::countWaiting(manager.conversations(path)));
        badge.report(&second, path, 1); // The same worktree in a second window counts once.
        badge.report(&second, "/synthetic/other", 2);
        QCOMPARE(badge.total(), 3);
        badge.forget(&second);
        QCOMPARE(badge.total(), 1);

        // A new message starts a turn, which clears the waiting state.
        manager.submitPrompt(path, "Go ahead");
        QTRY_COMPARE(manager.messages(path).size(), 4);
        QVERIFY(!row().value("awaitingReply").toBool());
        complete(false);
        QTRY_VERIFY(!manager.isRunning(path));
        QVERIFY(!row().value("awaitingReply").toBool());
        badge.report(&first, path, WaitingBadge::countWaiting(manager.conversations(path)));
        QCOMPARE(badge.total(), 0);
    }

    // ADR 0127: a turn that ends on a live managed job waits on it, so its row is neither
    // done nor unread; a job whose cleanup needs the user makes the row wait on the user.
    void waitingOnJob() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath("fake-codex");
        QFile file(executable);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"PY(#!/usr/bin/python3
import json, sys
def send(value): print(json.dumps(value), flush=True)
for line in sys.stdin:
    request = json.loads(line)
    method, rid = request.get("method"), request.get("id")
    if method == "initialize": send({"id":rid,"result":{}})
    elif method == "thread/start":
        send({"id":rid,"result":{"thread":{"id":"job-thread","turns":[]}}})
    elif method == "turn/start":
        send({"id":rid,"result":{"turn":{"id":"job-turn"}}})
        send({"method":"turn/started","params":{"threadId":"job-thread","turn":{"id":"job-turn"}}})
        item = {"id":"note","type":"agentMessage","phase":"commentary","text":"Started a job"}
        send({"method":"item/completed","params":{"threadId":"job-thread","turnId":"job-turn","item":item}})
)PY");
        file.close();
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner));
        CodexAppServer server(executable);
        AgentSessionManager manager(&server);
        ActivityBar bar;
        const QString path = QFileInfo(directory.path()).canonicalFilePath();
        connect(&manager, &AgentSessionManager::conversationsChanged, &bar,
                [&](const QString& changedPath, const QVariantList& conversations) {
                    if (changedPath == path)
                        bar.setRepositories({{path, "Repository", true, conversations}});
                });
        manager.submitPrompt(path, "Synthetic job test");
        QTRY_COMPARE(manager.messages(path).size(), 2);
        server.notificationReceived(
            "turn/completed", {{"threadId", "job-thread"},
                               {"turn", QJsonObject{{"id", "job-turn"}, {"status", "completed"}}}});
        QTRY_VERIFY(!manager.isRunning(path));
        const auto job = [&](const QString& state, int revision) {
            server.notificationReceived(
                "thread/backgroundTasks/updated",
                {{"threadId", "job-thread"},
                 {"tasks", QJsonArray{QJsonObject{{"task_id", "job-1"},
                                                  {"schema_version", 1},
                                                  {"kind", "shell"},
                                                  {"title", "Large library check"},
                                                  {"state", state},
                                                  {"revision", revision}}}}});
        };
        const auto row = [&] { return manager.conversations(path).first().toMap(); };
        ket::SidebarRow* icon = nullptr;
        QTRY_VERIFY((icon = bar.findChild<ket::SidebarRow*>("productConversationButton")));
        QCOMPARE(icon->status(), ket::ConversationStatus::Completed);

        QSignalSpy changed(&manager, &AgentSessionManager::conversationsChanged);
        job("queued", 1);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(row().value("liveJobs").toInt(), 1);
        QCOMPARE(row().value("queuedJobs").toInt(), 1);
        QTRY_COMPARE(
            (icon = bar.findChild<ket::SidebarRow*>("productConversationButton"))->status(),
            ket::ConversationStatus::WaitingOnTask);
        QVERIFY(icon->stateText().contains(QStringLiteral("Queued · Large library check")));
        QVERIFY(!icon->isUnread());
        QVERIFY(!sidebar_list::waitsForUser(row(), false));

        // A projection that leaves the row as it was does not republish the list.
        job("queued", 1);
        QCOMPARE(changed.size(), 1);

        job("running", 2);
        QTRY_VERIFY(!(icon = bar.findChild<ket::SidebarRow*>("productConversationButton"))
                         ->stateText()
                         .contains(QStringLiteral("Queued")));
        QVERIFY(icon->stateText().contains(QStringLiteral("Large library check")));
        QCOMPARE(icon->status(), ket::ConversationStatus::WaitingOnTask);

        job("needs_attention", 3);
        QCOMPARE(row().value("attentionJobs").toInt(), 1);
        QTRY_COMPARE(
            (icon = bar.findChild<ket::SidebarRow*>("productConversationButton"))->status(),
            ket::ConversationStatus::TaskNeedsAttention);
        QVERIFY(sidebar_list::waitsForUser(row(), true));

        job("completed", 4);
        QVERIFY(!row().contains("liveJobs"));
        QTRY_COMPARE(
            (icon = bar.findChild<ket::SidebarRow*>("productConversationButton"))->status(),
            ket::ConversationStatus::Completed);

        // A daemon owner summary covers the whole registry: it wins over the cached rows,
        // which keep a live record the bounded projection has dropped (ADR 0127 §8).
        const auto projected = [&](const QString& id, const QString& state, int revision,
                                   const QJsonObject& summary) {
            QJsonArray tasks;
            tasks.append(QJsonObject{{"task_id", id},
                                     {"schema_version", 1},
                                     {"kind", "shell"},
                                     {"title", "Hidden " + id},
                                     {"state", state},
                                     {"revision", revision}});
            server.notificationReceived(
                "thread/backgroundTasks/updated",
                {{"threadId", "job-thread"}, {"tasks", tasks}, {"summary", summary}});
        };
        projected("job-2", "running", 1, {{"live_jobs", 1}, {"job_title", "Hidden job-2"}});
        QTRY_COMPARE(
            (icon = bar.findChild<ket::SidebarRow*>("productConversationButton"))->status(),
            ket::ConversationStatus::WaitingOnTask);
        // job-2 ends outside the projection: its cached row still reads running.
        projected("job-old", "completed", 1, {{"live_jobs", 0}});
        QVERIFY(std::any_of(manager.backgroundTasks(path).cbegin(),
                            manager.backgroundTasks(path).cend(), [](const QVariant& value) {
                                return value.toMap().value("task_id") == "job-2" &&
                                       value.toMap().value("state") == "running";
                            }));
        QVERIFY(!row().contains("liveJobs"));
        QTRY_COMPARE(
            (icon = bar.findChild<ket::SidebarRow*>("productConversationButton"))->status(),
            ket::ConversationStatus::Completed);
        // A job needing attention beyond the projection still makes the row wait on the user.
        projected("job-old", "completed", 1,
                  {{"live_jobs", 2},
                   {"queued_jobs", 0},
                   {"attention_jobs", 1},
                   {"job_title", "Check cleanup"}});
        QTRY_COMPARE(
            (icon = bar.findChild<ket::SidebarRow*>("productConversationButton"))->status(),
            ket::ConversationStatus::TaskNeedsAttention);
        QVERIFY(icon->stateText().contains(QStringLiteral("Check cleanup and 1 more")));
    }

    void nativeStatusMarks() {
        ThemeManager theme(*qApp);
        for (const bool dark : {false, true}) {
            theme.setMode(dark ? ThemeManager::Mode::Dark : ThemeManager::Mode::Light);
            ActivityBar bar;
            bar.applyPalette(theme.palette());
            bar.resize(260, 680);
            QVariantList conversations;
            for (const auto& title : {"Working", "Question", "Confirmation", "Async question"}) {
                conversations.append(QVariantMap{{"id", title},
                                                 {"title", title},
                                                 {"path", "/synthetic/repository"},
                                                 {"started", true},
                                                 {"running", true},
                                                 {"status", "Working…"},
                                                 {"needsInput", QString(title) != "Working"}});
            }
            bar.setRepositories({{"/synthetic/repository", "Repository", true, conversations}});
            bar.show();
            QVERIFY(QTest::qWaitForWindowExposed(&bar));
            const auto icons = bar.findChildren<ket::SidebarRow*>("productConversationButton");
            QCOMPARE(icons.size(), 4);
            for (int index = 0; index < icons.size(); ++index) {
                QCOMPARE((icons.at(index)->status() == ket::ConversationStatus::Working),
                         icons.at(index)->property("conversationId") == "Working");
                QVERIFY(icons.at(index)->isVisible());
            }
            const auto capture = qEnvironmentVariable("KETPLUS_ATTENTION_CAPTURE");
            if (!capture.isEmpty())
                QVERIFY(bar.grab().save(capture + (dark ? "-dark.png" : "-light.png")));
        }
    }
};

QTEST_MAIN(ConversationAttentionTest)
#include "ConversationAttentionTest.moc"
