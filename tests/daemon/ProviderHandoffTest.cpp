#include "runtime/ProviderHandoff.h"
#include "protocol/DaemonProtocol.h"
#include "storage/LocalJournal.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

class ProviderHandoffTest final : public QObject {
    Q_OBJECT

  private slots:
    void everyRetiredThreadRemainsReachableForDeletion() {
        QJsonObject threads;
        provider_handoff::rememberThread(threads, "codex", "A1");
        provider_handoff::rememberThread(threads, "claude", "B1");
        provider_handoff::rememberThread(threads, "codex", "A2");
        provider_handoff::rememberThread(threads, "codex", "A2");
        const auto references = provider_handoff::threadReferences("codex", "A3", threads);
        QCOMPARE(references.size(), 4);
        QVERIFY(references.contains(QJsonObject{{"provider", "codex"}, {"thread_id", "A1"}}));
        QVERIFY(references.contains(QJsonObject{{"provider", "codex"}, {"thread_id", "A2"}}));
        QVERIFY(references.contains(QJsonObject{{"provider", "codex"}, {"thread_id", "A3"}}));
        QVERIFY(references.contains(QJsonObject{{"provider", "claude"}, {"thread_id", "B1"}}));
    }
    void retainedHistoryAfterAcknowledgment_data() {
        QTest::addColumn<int>("turns");
        QTest::addColumn<int>("answerSize");
        QTest::newRow("acknowledged-conversation") << 2 << 32;
        QTest::newRow("event-limited-pages") << protocol::maximumReplayEvents + 1 << 32;
        QTest::newRow("byte-limited-pages") << 12 << 100000;
    }

    void retainedHistoryAfterAcknowledgment() {
        QFETCH(int, turns);
        QFETCH(int, answerSize);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalJournal journal(directory.filePath("journal.sqlite3"));
        QVERIFY(journal.isOpen());
        StoredSession session;
        session.id = "handoff-session";
        session.worktreePath = directory.path();
        session.adapter = "claude";
        session.state = "ready";
        QVERIFY(journal.upsertSession(session));
        const auto append = [&](const QString& type, const QJsonObject& payload) {
            return journal.appendEvent(session.id, session.generation, type, payload);
        };
        QVERIFY(append("session.created", {{"adapter", "claude"}}) > 0);
        qint64 lastCursor = 0;
        for (int i = 0; i < turns; ++i) {
            const QString itemId = QStringLiteral("answer-%1").arg(i);
            QVERIFY(append("user.message", {{"text", QStringLiteral("Task %1").arg(i)}}) > 0);
            QVERIFY(append("assistant.delta", {{"item_id", itemId}, {"delta", "partial"}}) > 0);
            QVERIFY(append("item.completed",
                           {{"item_id", itemId},
                            {"item",
                             QJsonObject{{"id", itemId},
                                         {"type", "agentMessage"},
                                         {"text", QString(answerSize, QLatin1Char('x')) +
                                                      QStringLiteral(" answer %1").arg(i)}}}}) > 0);
            lastCursor = append("turn.completed", {{"status", "completed"}});
            QVERIFY(lastCursor > 0);
        }
        // This is the normal desktop ACK path, missing from the original handoff test.
        QVERIFY(journal.acknowledgeCursor("desktop", session.id, session.generation, lastCursor));

        const QJsonArray events = provider_handoff::journalEvents(journal, session.id, 1);
        QCOMPARE(events.size(), 1 + 3 * turns); // Completed answers replace deltas.
        QCOMPARE(events.last().toObject().value("cursor").toInteger(), lastCursor);
        const QString transcript = provider_handoff::renderTranscript(events);
        QCOMPARE(transcript.count("**User:**"), turns);
        QCOMPARE(transcript.count("**Claude:**"), turns);
        QVERIFY(transcript.contains("Task 0"));
        QVERIFY(transcript.contains(QStringLiteral("answer %1").arg(turns - 1)));
        QVERIFY(!transcript.contains("partial"));
        QVERIFY(provider_handoff::journalEvents(journal, session.id, 2).isEmpty());
        QVERIFY(provider_handoff::journalEvents(journal, "other-session", 1).isEmpty());

        // Physical raw cleanup is now bounded/eventual. Handoff must be identical
        // while cleanup is queued and after every acknowledged delivery row is gone.
        for (int pass = 0;
             pass < 64 && !journal.eventsAfter(session.id, session.generation, 0).isEmpty(); ++pass)
            QVERIFY(journal.pruneAcknowledgedEvents());
        QVERIFY(journal.eventsAfter(session.id, session.generation, 0).isEmpty());
        QCOMPARE(provider_handoff::journalEvents(journal, session.id, 1), events);
    }
};

QTEST_GUILESS_MAIN(ProviderHandoffTest)
#include "ProviderHandoffTest.moc"
