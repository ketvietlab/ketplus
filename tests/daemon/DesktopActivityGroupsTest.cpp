#include "AgentWorkspaceViewTest.h"
#include "TranscriptWindowTestSupport.h"
#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"
#include "ipc/LocalIpcServer.h"
#include "providers/CodexAdapter.h"
#include "runtime/SessionHost.h"

#include <QQuickItem>
#include <QQuickView>
#include <QStandardPaths>

class DesktopActivityGroupsTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }
    void summariesExpandUpdateCollapseAndExportOverAuthenticatedIpc() {
        Environment env;
        LocalJournal journal(env.directory.filePath("journal.sqlite3"));
        QVERIFY(journal.isOpen());
        QVERIFY(journal.upsertSession(
            {"grouped", env.directory.path(), "codex", {}, {}, "ready", 1, 0, 0, "Grouped"}));
        QVERIFY(journal.appendEvent("grouped", 1, "user.message",
                                    {{"text", "Synthetic group fixture"}}) > 0);
        const auto tool = [](const QString& id) {
            return QJsonObject{{"item", QJsonObject{{"id", id},
                                                    {"type", "mcpToolCall"},
                                                    {"server", "ketplus_browser"},
                                                    {"tool", "browser_observe"},
                                                    {"status", "completed"}}}};
        };
        const auto first = journal.appendEvent("grouped", 1, "item.completed", tool("one"));
        QVERIFY(first > 0);
        QVERIFY(journal.appendEvent("grouped", 1, "item.completed", tool("two")) > 0);
        const auto canonical = QFileInfo(env.directory.path()).canonicalFilePath();
        QSettings().setValue(
            persistedKey(canonical),
            QVariantMap{{"path", canonical},
                        {"activeId", "grouped"},
                        {"conversations",
                         QVariantList{QVariantMap{
                             {"id", "grouped"}, {"threadId", "grouped"}, {"title", "Grouped"}}}}});
        const QByteArray token(64, 'a');
        QFile file(env.directory.filePath("local.token"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(token), token.size());
        file.close();
        CodexAdapter adapter(env.directory.filePath("unavailable-provider"));
        SessionHost host(&journal, &adapter);
        LocalIpcServer ipc(&host, env.directory.filePath("daemon.sock"), token);
        QVERIFY(ipc.listen());
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        AgentWorkspaceView view;
        view.applyTheme(agent_workspace_test::testPalette());
        view.resize(1100, 900);
        view.show();
        const auto scope = canonical + ":grouped";
        connect(&desktop, &AgentSessionManager::messagesReset, &view,
                [&](const QString&, const QVariantList& rows) { view.resetMessages(rows, scope); });
        connect(&desktop, &AgentSessionManager::messageAppended, &view,
                [&](const QString&, int, const QVariantMap& row) { view.appendMessage(row); });
        connect(&desktop, &AgentSessionManager::messageUpdated, &view,
                [&](const QString&, int index, const QVariantMap& row) {
                    view.updateMessage(index, row);
                });
        connect(&view, &AgentWorkspaceView::activityGroupRequested, &desktop,
                [&](const QString& current, const QString& key, bool expanded) {
                    desktop.setActivityGroupOpen(canonical, current, key, expanded);
                });
        view.setTranscriptPayloadSource(
            [&](const QString& current, const QString& key, const QString& kind) {
                return desktop.transcriptPayload(canonical, current, key, kind);
            });
        desktop.activate(canonical);
        QTRY_VERIFY(server.isReady());
        QTRY_COMPARE(desktop.messages(canonical).size(), 2);
        QTRY_VERIFY(!desktop.historyState(canonical).value("loading").toBool());
        auto summary = desktop.messages(canonical).last().toMap();
        QCOMPARE(summary.value("subtype"), "daemonGroup");
        QCOMPARE(summary.value("groupSize").toInt(), 2);
        const auto key = summary.value("messageKey").toString();
        QTRY_COMPARE(view.messageModel()->rowCount(), 2);
        view.messageModel()->setGroupExpanded(key, true);
        const auto details = [&] {
            return desktop.messages(canonical)
                .at(1)
                .toMap()
                .value("details")
                .toMap()
                .value("groupItems")
                .toList();
        };
        QTRY_COMPARE(details().size(), 2);
        QTRY_VERIFY(view.quickView()->rootObject());
        const auto remote = [&]() -> QQuickItem* {
            const auto find = [&](auto&& self, QQuickItem* item) -> QQuickItem* {
                if (item->objectName() == "transcriptRemoteGroup")
                    return item;
                for (auto* child : item->childItems())
                    if (auto* found = self(self, child))
                        return found;
                return nullptr;
            };
            return find(find, view.quickView()->rootObject());
        };
        QTRY_VERIFY(remote());
        QTRY_VERIFY(remote()->height() > 0);
        const auto added = journal.appendEvent("grouped", 1, "item.completed", tool("three"));
        QVERIFY(added > 0);
        host.eventReady(journal.eventsAfter("grouped", 1, added - 1, 1).first().toObject());
        QTRY_COMPARE(desktop.messages(canonical).at(1).toMap().value("groupSize").toInt(), 3);
        QTRY_COMPARE(details().size(), 3);
        QVERIFY(view.messageModel()->groupExpanded(key));
        const auto capture = qEnvironmentVariable("KETPLUS_GROUP_CAPTURE_PREFIX");
        if (!capture.isEmpty()) {
            QVERIFY(view.quickView()->grabWindow().save(capture + "-wide.png"));
            view.resize(520, 900);
            QTest::qWait(150);
            QVERIFY(view.quickView()->grabWindow().save(capture + "-narrow.png"));
        }
        view.messageModel()->setGroupExpanded(key, false);
        QTRY_VERIFY(details().isEmpty());
        QCOMPARE(view.messageModel()->rowCount(), 2);
        QSignalSpy copied(&desktop, &AgentSessionManager::conversationCopyReady);
        desktop.copyConversation(canonical);
        QTRY_COMPARE(copied.size(), 1);
        const auto exported = copied.first().at(2).toString();
        QVERIFY(exported.contains("Observed the page"));
        QVERIFY(!view.messageModel()->groupExpanded(key));
        // An explicitly opted-out client still sees raw lifecycle events on the same daemon.
        bool replied = false;
        QJsonObject raw;
        server.readDaemonTranscriptWindow("grouped", {{"raw_activity_details", true}},
                                          [&](const QJsonObject& page, const QString& error) {
                                              QVERIFY2(error.isEmpty(), qPrintable(error));
                                              raw = page;
                                              replied = true;
                                          });
        QTRY_VERIFY(replied);
        QCOMPARE(raw.value("events").toArray().size(), 4);
        QCOMPARE(raw.value("events").toArray().last().toObject().value("event_type"),
                 "item.completed");

        // A server's step joins the line while it runs; returning images, it leaves the line
        // and shows on its own, live.
        const auto repl = [](const QString& status, bool image) {
            QJsonObject body{{"id", "shot"},
                             {"type", "mcpToolCall"},
                             {"server", "node_repl"},
                             {"tool", "js"},
                             {"status", status}};
            if (image)
                body.insert(
                    "result",
                    QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "image"},
                                                                   {"data", "iVBORw0KGgo="},
                                                                   {"mimeType", "image/png"}}}}});
            return QJsonObject{{"item", body}};
        };
        const auto joined =
            journal.appendEvent("grouped", 1, "item.started", repl("inProgress", false));
        host.eventReady(journal.eventsAfter("grouped", 1, joined - 1, 1).first().toObject());
        QTRY_COMPARE(desktop.messages(canonical).at(1).toMap().value("groupSize").toInt(), 4);
        QCOMPARE(desktop.messages(canonical).size(), 2);
        const auto evidence =
            journal.appendEvent("grouped", 1, "item.completed", repl("completed", true));
        host.eventReady(journal.eventsAfter("grouped", 1, evidence - 1, 1).first().toObject());
        QTRY_COMPARE(desktop.messages(canonical).size(), 3);
        QCOMPARE(desktop.messages(canonical).at(1).toMap().value("groupSize").toInt(), 3);
        QVERIFY(!desktop.messages(canonical).at(1).toMap().value("running").toBool());
        QCOMPARE(desktop.messages(canonical).last().toMap().value("itemId").toString(), "shot");
        server.stop();
    }
};
QTEST_MAIN(DesktopActivityGroupsTest)
#include "DesktopActivityGroupsTest.moc"
