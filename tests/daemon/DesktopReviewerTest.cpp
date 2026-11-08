#include "TranscriptWindowTestSupport.h"

#include <QSignalSpy>
#include <QStandardPaths>

// ADR 0067: Desktop reads and assigns a conversation's reviewer through a real daemon.
class DesktopReviewerTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void assignsAndClearsTheReviewerByConversationNumber() {
        Environment env;
        QVERIFY(env.directory.isValid());
        const auto path = QFileInfo(env.directory.path()).canonicalFilePath();
        const auto executable = env.directory.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, env.directory.filePath("provider.log")));
        {
            LocalJournal journal(env.directory.filePath("journal.sqlite3"));
            QVERIFY(journal.upsertSession(
                {"working", path, "codex", {}, {}, "ready", 1, 0, 0, "Working"}));
            QVERIFY(
                journal.upsertSession({"peer", path, "codex", {}, {}, "ready", 1, 0, 0, "Peer"}));
        }
        rememberConversation(path, "working");
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"), executable});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        CodexAppServer server;
        AgentSessionManager desktop(&server);
        desktop.activate(path);
        QTRY_VERIFY(server.isReady());
        QTRY_VERIFY(!server.isRestoring());
        desktop.selectConversation(path, "working");
        const auto number = [&](const QString& id) {
            for (const auto& value : desktop.conversations(path))
                if (value.toMap().value("id") == id)
                    return value.toMap().value("number").toString();
            return QString{};
        };
        QTRY_VERIFY_WITH_TIMEOUT(!number("peer").isEmpty(), 5000);

        QSignalSpy results(&desktop, &AgentSessionManager::reviewerResult);
        // Each call answers exactly once, synchronously or later.
        const auto call = [&](const auto& request) {
            const auto before = results.size();
            request();
            if (!QTest::qWaitFor([&] { return results.size() > before; }, 5000))
                return QVariantMap{{"error", "timed out"}};
            return results.last().at(2).toMap();
        };
        auto state = call([&] { desktop.readReviewer(path); });
        QVERIFY2(!state.contains("error"), qPrintable(state.value("error").toString()));
        QCOMPARE(state.value("available").toBool(), true);
        QVERIFY(!state.contains("reviewer"));
        QCOMPARE(results.last().at(1).toString(), QStringLiteral("working"));

        state = call([&] { desktop.setReviewer(path, number("peer"), {}); });
        QVERIFY2(!state.contains("error"), qPrintable(state.value("error").toString()));
        const auto reviewer = state.value("reviewer").toMap();
        QCOMPARE(reviewer.value("session_id").toString(), QStringLiteral("peer"));
        QCOMPARE(reviewer.value("number").toString(), number("peer"));
        QCOMPARE(reviewer.value("provider").toString(), QStringLiteral("codex"));

        // A number Desktop does not know never reaches the daemon.
        const auto unknown = call([&] { desktop.setReviewer(path, QStringLiteral("99999"), {}); });
        QVERIFY(unknown.contains("error"));
        QVERIFY(unknown.value("change").toBool());
        QCOMPARE(call([&] { desktop.readReviewer(path); })
                     .value("reviewer")
                     .toMap()
                     .value("session_id")
                     .toString(),
                 QStringLiteral("peer"));

        // The daemon's own validation is reported, not swallowed.
        const auto self = call([&] { desktop.setReviewer(path, number("working"), {}); });
        QVERIFY(self.value("error").toString().contains("cannot review itself"));
        QVERIFY(self.value("change").toBool());

        state = call([&] { desktop.setReviewer(path, {}, {}); });
        QVERIFY2(!state.contains("error"), qPrintable(state.value("error").toString()));
        QVERIFY(!state.contains("reviewer"));
    }
};

QTEST_GUILESS_MAIN(DesktopReviewerTest)
#include "DesktopReviewerTest.moc"
