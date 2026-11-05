#include "TranscriptWindowTestSupport.h"

#include <QStandardPaths>

class ConversationAttentionDaemonTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void backgroundAsyncQuestionDoesNotHydrateTranscript() {
        Environment env;
        QVERIFY(env.directory.isValid());
        const auto executable = env.directory.filePath("fake-codex");
        QVERIFY(writeFakeCodex(executable, env.directory.filePath("provider.log")));
        DaemonService service({env.directory.filePath("journal.sqlite3"),
                               env.directory.filePath("local.token"),
                               env.directory.filePath("daemon.sock"), executable});
        QVERIFY2(service.start(), qPrintable(service.errorString()));
        const auto first = env.directory.filePath("first");
        const auto second = env.directory.filePath("second");
        QVERIFY(QDir().mkpath(first));
        QVERIFY(QDir().mkpath(second));
        AgentSessionManager manager;
        manager.activate(first);
        manager.submitPrompt(first, "ask");
        manager.activate(second);
        const auto needsInput = [&] {
            const auto conversations = manager.conversations(first);
            return !conversations.isEmpty() &&
                   conversations.first().toMap().value("needsInput").toBool();
        };
        QTRY_VERIFY(needsInput());
        QVERIFY(manager.isRunning(first));
        // An async question leaves the provider working; only the sidebar attention changes.
        QCOMPARE(manager.status(first), "Working…");
        const auto beforeSelection = manager.messages(first);
        for (const auto& value : beforeSelection)
            QVERIFY(value.toMap().value("subtype") != "question");
        manager.activate(first);
        QVariantMap question;
        const auto findQuestion = [&] {
            for (const auto& value : manager.messages(first)) {
                if (value.toMap().value("subtype") == "question") {
                    question = value.toMap();
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY(findQuestion());
        const auto option = question.value("questions")
                                .toList()
                                .first()
                                .toMap()
                                .value("options")
                                .toList()
                                .first()
                                .toMap()
                                .value("id")
                                .toString();
        manager.answerQuestion(first, question.value("messageKey").toString(),
                               {{"scope", QVariantMap{{"option_ids", QStringList{option}}}}});
        QTRY_VERIFY(!needsInput());
        QTRY_VERIFY(!manager.isRunning(first));
    }
};

QTEST_GUILESS_MAIN(ConversationAttentionDaemonTest)
#include "ConversationAttentionDaemonTest.moc"
