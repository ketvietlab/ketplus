// The content-free push notices the daemon sends while events are forwarded: their exact
// content, when they are sent, and the bounded queue behind a closed socket. No network.
#include "transport/PushSignals.h"
#include "transport/Signaling.h"
#include <QJsonDocument>
#include <QtTest>

using namespace ketplus::daemon::transport;

namespace {
QJsonObject journalEvent(const QString& type, qint64 cursor = 7,
                         const QString& session = "session-1") {
    return {{"session_id", session},
            {"session_generation", 1},
            {"cursor", cursor},
            {"event_type", type},
            {"payload", QJsonObject{{"text", "secret words"}, {"tool", "secret tool"}}}};
}
struct Harness {
    QList<QJsonObject> sent;
    bool open{false};
    bool enabled{true};
    qint64 time{1'000'000};
    PushSignals notices{[this](const QJsonObject& message) {
                            if (!open)
                                return false;
                            sent << message;
                            return true;
                        },
                        [this] { return enabled; }};
    Harness() {
        notices.setClock([this] { return time; });
    }
};
} // namespace

class PushSignalsTest final : public QObject {
    Q_OBJECT
  private slots:
    void messageHasExactlyFiveKeysAndNoContent_data() {
        QTest::addColumn<QString>("kind");
        for (const auto* kind : {"turn.completed", "runtime.error", "turn.failed",
                                 "approval.requested", "question.requested"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void messageHasExactlyFiveKeysAndNoContent() {
        QFETCH(QString, kind);
        const auto message = PushSignals::message(journalEvent(kind, 42));
        QCOMPARE(message.keys(), (QStringList{"cursor", "kind", "routingId", "sessionId", "type"}));
        QCOMPARE(message.value("type").toString(), QStringLiteral("push.signal"));
        QCOMPARE(message.value("sessionId").toString(), QStringLiteral("session-1"));
        QCOMPARE(message.value("routingId").toString(), QStringLiteral("session-1"));
        QCOMPARE(message.value("kind").toString(), kind);
        QVERIFY(message.value("cursor").isDouble());
        QCOMPARE(message.value("cursor").toInteger(), 42);
        const auto text = QJsonDocument(message).toJson();
        QVERIFY(!text.contains("secret"));
    }

    void otherEventsRaiseNothing() {
        for (const auto* kind : {"turn.started", "item.completed", "assistant.delta",
                                 "tasks.changed", "session.deleted", "approval.resolved", ""})
            QVERIFY2(PushSignals::message(journalEvent(kind)).isEmpty(), kind);
        QVERIFY(PushSignals::message(journalEvent("turn.completed", 0)).isEmpty());
        QVERIFY(PushSignals::message(journalEvent("turn.completed", 7, "")).isEmpty());
        QVERIFY(PushSignals::message(journalEvent("turn.completed", 7, "bad id")).isEmpty());
        QVERIFY(
            PushSignals::message(journalEvent("turn.completed", 7, QString(129, 'x'))).isEmpty());
        auto text = journalEvent("turn.completed");
        text.insert("cursor", "7");
        QVERIFY(PushSignals::message(text).isEmpty());
    }

    void sendsOnlyWhileEnabled() {
        Harness h;
        h.open = true;
        h.enabled = false;
        h.notices.note(journalEvent("turn.completed"));
        QVERIFY(h.sent.isEmpty());
        QCOMPARE(h.notices.pending(), 0);
        h.enabled = true;
        h.notices.note(journalEvent("turn.completed", 8));
        QCOMPARE(h.sent.size(), 1);
        QCOMPARE(h.sent.first().value("cursor").toInteger(), 8);
        QCOMPARE(h.notices.pending(), 0);
    }

    void disablingDropsWhatWaits() {
        Harness h;
        h.notices.note(journalEvent("turn.completed"));
        QCOMPARE(h.notices.pending(), 1);
        h.enabled = false;
        h.open = true;
        h.notices.flush();
        QVERIFY(h.sent.isEmpty());
        QCOMPARE(h.notices.pending(), 0);
    }

    void closedSocketKeepsAtMostSixtyFourNewestInOrder() {
        Harness h;
        for (int i = 1; i <= 100; ++i)
            h.notices.note(journalEvent("turn.completed", i));
        QCOMPARE(h.notices.pending(), PushSignals::maxPending);
        h.open = true;
        h.notices.flush();
        QCOMPARE(h.sent.size(), 64);
        QCOMPARE(h.sent.first().value("cursor").toInteger(), 37);
        QCOMPARE(h.sent.last().value("cursor").toInteger(), 100);
        QCOMPARE(h.notices.pending(), 0);
    }

    void staleNoticesAreDropped() {
        Harness h;
        h.notices.note(journalEvent("turn.completed", 1));
        h.time += PushSignals::maxAgeMs + 1;
        h.notices.note(journalEvent("turn.failed", 2));
        h.open = true;
        h.notices.flush();
        QCOMPARE(h.sent.size(), 1);
        QCOMPARE(h.sent.first().value("cursor").toInteger(), 2);
    }

    void aBackedUpSocketIsRetriedWithoutBlocking() {
        Harness h;
        h.notices.note(journalEvent("turn.completed", 1));
        QCOMPARE(h.notices.pending(), 1);
        h.open = true;
        QTRY_COMPARE_WITH_TIMEOUT(h.sent.size(), 1, 3000);
        QCOMPARE(h.notices.pending(), 0);
    }

    void signalingIgnoresTheAnswer() {
        RemotePeers peers([](const QString&, qint64, qint64) { return EventPage{}; });
        Signaling signaling(
            &peers, [](const QString&, int, auto callback) { callback({}); },
            [](const QByteArray&) { return QByteArray(); });
        signaling.configure("workspace", "machine", 1, true);
        QSignalSpy outgoing(&signaling, &Signaling::outgoing);
        signaling.receive({{"type", "push.signal.result"},
                           {"sessionId", "session-1"},
                           {"kind", "turn.completed"},
                           {"cursor", 7}});
        QCOMPARE(outgoing.size(), 0);
        QVERIFY(!signaling.forwardOnlyActive());
        signaling.setForwardOnly(true);
        QVERIFY(signaling.forwardOnlyActive());
        signaling.configure({}, {}, 1, false);
        QVERIFY(!signaling.forwardOnlyActive());
    }
};

QTEST_GUILESS_MAIN(PushSignalsTest)
#include "PushSignalsTest.moc"
