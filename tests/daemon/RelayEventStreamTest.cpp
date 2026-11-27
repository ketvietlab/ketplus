// The device's relay frame link and the per-machine event stream, against a scripted peer: no
// socket, no network.
#include "control/RelayEventStream.h"
#include "FakeFrameSocket.h"
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QtTest>
#include <optional>

using namespace ketplus::remote;
namespace {
const QUrl relayOrigin("https://relay.invalid");

QJsonObject sampleEvent(qint64 cursor) {
    return {{"cursor", cursor}, {"sessionGeneration", 1}, {"eventType", "user.message"}};
}
QJsonObject page(const QString& session, std::initializer_list<qint64> cursors, bool live = false) {
    QJsonArray events;
    for (const qint64 cursor : cursors)
        events.append(sampleEvent(cursor));
    return {{"type", "events"}, {"sessionId", session}, {"events", events}, {"live", live}};
}
QJsonObject simple(const QString& type, const QString& session = {}, qint64 generation = 0,
                   qint64 after = -1) {
    QJsonObject o{{"type", type}};
    if (!session.isEmpty())
        o.insert("sessionId", session);
    if (generation > 0)
        o.insert("sessionGeneration", generation);
    if (after >= 0)
        o.insert("afterCursor", after);
    return o;
}
QList<qint64> cursors(const QJsonArray& events) {
    QList<qint64> out;
    for (const auto& e : events)
        out.append(e.toObject().value("cursor").toInteger());
    return out;
}
qint64 afterOf(const QJsonObject& frame) { return frame.value("afterCursor").toInteger(-1); }

class MemoryCursors final : public EventCursorStore {
  public:
    QHash<QString, qint64> values;
    std::optional<qint64> load(const QString& s, qint64 g) const override {
        return values.value(QStringLiteral("%1:%2").arg(s).arg(g));
    }
    bool save(const QList<Entry>& entries) override {
        for (const auto& e : entries)
            values.insert(QStringLiteral("%1:%2").arg(e.sessionId).arg(e.generation), e.cursor);
        return true;
    }
};

RelayFrameLink::TokenSource token(QStringList* failures = nullptr) {
    return [failures](std::function<void(const QString&, const QString&)> done) {
        if (failures && !failures->isEmpty())
            done({}, failures->takeFirst());
        else
            done(QStringLiteral("synthetic-access-token"), {});
    };
}
RelayFrameLink::Options quick() { return {40, 400, 150}; }
} // namespace

class RelayEventStreamTest final : public QObject {
    Q_OBJECT
  private slots:
    void linkOpensTheRelayPathWithTheBearerToken() {
        FakeFrameSockets sockets;
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        link.start();
        QCOMPARE(link.state(), RelayFrameLink::State::Connecting);
        QVERIFY(sockets.last());
        QCOMPARE(sockets.last()->url, QUrl("wss://relay.invalid/v2/relay"));
        QCOMPARE(sockets.last()->bearer, QStringLiteral("synthetic-access-token"));
        sockets.last()->accept();
        QCOMPARE(link.state(), RelayFrameLink::State::Open);
        RelayFrameLink local(QUrl("http://127.0.0.1:8080"), token(), sockets.factory(), quick());
        local.start();
        QCOMPARE(sockets.last()->url, QUrl("ws://127.0.0.1:8080/v2/relay"));
    }

    void linkReconnectsWithBackoffAndStartsOverAfterAHealthyConnection() {
        FakeFrameSockets sockets;
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        QList<qint64> madeAt;
        QElapsedTimer clock;
        clock.start();
        link.start();
        madeAt.append(clock.elapsed());
        // Three failures in a row: each retry waits longer than the one before.
        for (int i = 0; i < 3; ++i) {
            const auto count = sockets.made.size();
            sockets.last()->hangUp();
            QCOMPARE(link.state(), RelayFrameLink::State::Waiting);
            QTRY_COMPARE_WITH_TIMEOUT(sockets.made.size(), count + 1, 2000);
            madeAt.append(clock.elapsed());
        }
        const auto first = madeAt[1] - madeAt[0];
        const auto last = madeAt[3] - madeAt[2];
        QVERIFY2(first >= 30, qPrintable(QString::number(first)));
        QVERIFY2(last >= 100, qPrintable(QString::number(last))); // 40, 80, 160
        // A connection that stayed open long enough starts the backoff over.
        sockets.last()->accept();
        QTest::qWait(200);
        const auto count = sockets.made.size();
        QElapsedTimer retry;
        retry.start();
        sockets.last()->hangUp();
        QTRY_COMPARE_WITH_TIMEOUT(sockets.made.size(), count + 1, 2000);
        // Without the reset the fourth wait would be 320 ms.
        QVERIFY2(retry.elapsed() < 250, qPrintable(QString::number(retry.elapsed())));
    }

    void linkRetriesWhenNoTokenIsAvailable() {
        FakeFrameSockets sockets;
        QStringList failures{"Sign in first.", "Sign in first."};
        RelayFrameLink link(relayOrigin, token(&failures), sockets.factory(), quick());
        link.start();
        QCOMPARE(sockets.made.size(), 0);
        QCOMPARE(link.state(), RelayFrameLink::State::Waiting);
        QTRY_VERIFY_WITH_TIMEOUT(sockets.last(), 3000);
        QCOMPARE(sockets.made.size(), 1);
    }

    void aRevokedDeviceStopsTheLinkForGood() {
        FakeFrameSockets sockets;
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        QSignalSpy revoked(&link, &RelayFrameLink::authorizationRevoked);
        link.start();
        sockets.last()->accept();
        sockets.last()->hangUp(4003);
        QCOMPARE(revoked.size(), 1);
        QCOMPARE(link.state(), RelayFrameLink::State::Stopped);
        QTest::qWait(200);
        QCOMPARE(sockets.made.size(), 1);
    }

    void stoppingHaltsAPendingRetry() {
        FakeFrameSockets sockets;
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        link.start();
        sockets.last()->hangUp();
        link.stop();
        QTest::qWait(150);
        QCOMPARE(sockets.made.size(), 1);
        QCOMPARE(link.state(), RelayFrameLink::State::Stopped);
    }

    void sendWrapsFramesForAMachineWithinTheLimit() {
        FakeFrameSockets sockets;
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        QVERIFY(!link.send("machine-a", R"({"type":"unsubscribe","sessionId":"s"})")); // closed
        link.start();
        sockets.last()->accept();
        QVERIFY(link.send("machine-a", R"({"type":"unsubscribe","sessionId":"s"})"));
        const auto sent = QJsonDocument::fromJson(sockets.last()->sent.last()).object();
        QCOMPARE(sent.value("type").toString(), QStringLiteral("frame"));
        QCOMPARE(sent.value("machineId").toString(), QStringLiteral("machine-a"));
        QCOMPARE(sent.value("frame").toObject().value("type").toString(),
                 QStringLiteral("unsubscribe"));
        const QByteArray big = QByteArray(R"({"type":"unsubscribe","sessionId":")") +
                               QByteArray(RelayFrameLink::maximumSendBytes, 'x') + "\"}";
        QVERIFY(!link.send("machine-a", big));
        QVERIFY(!link.send("machine-a", "not json"));
        QVERIFY(!link.send({}, R"({"type":"unsubscribe"})"));
        QCOMPARE(sockets.last()->sent.size(), 1);
    }

    void linkRoutesFramesAndDropsByMachineAndIgnoresWakeUps() {
        FakeFrameSockets sockets;
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        QSignalSpy frames(&link, &RelayFrameLink::frameReceived);
        QSignalSpy dropped(&link, &RelayFrameLink::dropped);
        link.start();
        auto* socket = sockets.last();
        socket->accept();
        socket->receive(QJsonObject{{"type", "ready"}, {"role", "client"}});
        socket->receive(QJsonObject{{"type", "resync"}});
        socket->receive(QByteArray("{broken"));
        socket->receive(QJsonObject{{"type", "frame"}, {"machineId", "m"}, {"frame", "text"}});
        socket->receive(QJsonObject{{"type", "frame"}, {"frame", QJsonObject{}}});
        QCOMPARE(frames.size(), 0);
        socket->forward("machine-a", simple("gap", "s", 0, 4));
        socket->drop("machine-b", "queue_full");
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.first().at(0).toString(), QStringLiteral("machine-a"));
        QCOMPARE(QJsonDocument::fromJson(frames.first().at(1).toByteArray())
                     .object()
                     .value("type")
                     .toString(),
                 QStringLiteral("gap"));
        QCOMPARE(dropped.size(), 1);
        QCOMPARE(dropped.first().at(0).toString(), QStringLiteral("machine-b"));
        QCOMPARE(dropped.first().at(1).toString(), QStringLiteral("queue_full"));
        // Beyond the largest frame the relay forwards, a message is not read at all.
        socket->receive(QByteArray(RelayFrameLink::maximumReceiveBytes + 1, ' '));
        QCOMPARE(frames.size(), 1);
    }

    // ---- RelayEventStream -----------------------------------------------------------------

    void streamSubscribesAfterTheCursorOnEveryNewConnection() {
        FakeFrameSockets sockets;
        MemoryCursors store;
        store.values.insert("a:1", 7);
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        RelayEventStream stream(&link, "machine", &store);
        stream.follow("a", 1);
        link.start();
        sockets.last()->accept();
        auto frames = sockets.last()->frames("machine");
        QCOMPARE(frames.size(), 1);
        QCOMPARE(frames.first().value("type").toString(), QStringLiteral("subscribe"));
        QCOMPARE(afterOf(frames.first()), 7);
        QSignalSpy ready(&stream, &RelayEventStream::eventsReady);
        sockets.last()->forward("machine", page("a", {8, 9}, true));
        QCOMPARE(ready.size(), 1);
        QCOMPARE(cursors(ready.first().at(1).toJsonArray()), (QList<qint64>{8, 9}));
        // The socket drops and opens again: subscribed again, after what was read.
        sockets.last()->hangUp();
        QTRY_COMPARE_WITH_TIMEOUT(sockets.made.size(), 2, 2000);
        sockets.last()->accept();
        frames = sockets.last()->frames("machine");
        QCOMPARE(frames.size(), 1);
        QCOMPARE(afterOf(frames.first()), 9);
        // Events already read are not delivered again.
        sockets.last()->forward("machine", page("a", {8, 9, 10}));
        QCOMPARE(ready.size(), 2);
        QCOMPARE(cursors(ready.last().at(1).toJsonArray()), (QList<qint64>{10}));
        QCOMPARE(store.values.value("a:1"), 10);
    }

    void streamIgnoresOtherMachinesFrames() {
        FakeFrameSockets sockets;
        MemoryCursors store;
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        RelayEventStream stream(&link, "machine", &store);
        stream.follow("a", 1);
        link.start();
        sockets.last()->accept();
        QSignalSpy ready(&stream, &RelayEventStream::eventsReady);
        QSignalSpy reach(&stream, &RelayEventStream::reachabilityChanged);
        sockets.last()->forward("other", page("a", {1, 2}));
        sockets.last()->drop("other", "host_offline");
        QCOMPARE(ready.size(), 0);
        QCOMPARE(reach.size(), 0);
        QVERIFY(stream.hostReachable());
    }

    void aGapOrADropSubscribesAgainAfterTheCursorAndRepeatedDropsWaitLonger() {
        FakeFrameSockets sockets;
        MemoryCursors store;
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        RelayEventStream::Options options;
        options.initialResubscribeMilliseconds = 30;
        options.maximumResubscribeMilliseconds = 120;
        RelayEventStream stream(&link, "machine", &store, options);
        stream.follow("a", 1);
        link.start();
        auto* socket = sockets.last();
        socket->accept();
        socket->forward("machine", page("a", {1, 2}, true));
        QCOMPARE(socket->frames("machine").size(), 1);
        // A gap subscribes again at once, after the cursor reached.
        socket->forward("machine", simple("gap", "a", 0, 5));
        QCOMPARE(socket->frames("machine").size(), 2);
        QCOMPARE(afterOf(socket->frames("machine").last()), 2);
        // A drop waits, then subscribes again; the next one waits longer.
        QElapsedTimer waited;
        waited.start();
        socket->drop("machine", "queue_full");
        QCOMPARE(socket->frames("machine").size(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(socket->frames("machine").size(), 3, 2000);
        const auto first = waited.elapsed();
        waited.restart();
        socket->drop("machine", "queue_full");
        socket->drop("machine", "queue_full");
        QTRY_COMPARE_WITH_TIMEOUT(socket->frames("machine").size(), 4, 2000);
        QVERIFY2(waited.elapsed() >= first, qPrintable(QString::number(waited.elapsed())));
        QCOMPARE(afterOf(socket->frames("machine").last()), 2);
        // Events arriving start the wait over, and nothing is lost or repeated.
        QSignalSpy ready(&stream, &RelayEventStream::eventsReady);
        socket->forward("machine", page("a", {2, 3, 4}, true));
        QCOMPARE(cursors(ready.last().at(1).toJsonArray()), (QList<qint64>{3, 4}));
    }

    void anOfflineHostIsReportedUntilItsFramesArriveAgain() {
        FakeFrameSockets sockets;
        MemoryCursors store;
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        RelayEventStream::Options options;
        options.initialResubscribeMilliseconds = 20;
        options.maximumResubscribeMilliseconds = 40;
        RelayEventStream stream(&link, "machine", &store, options);
        QSignalSpy reach(&stream, &RelayEventStream::reachabilityChanged);
        stream.follow("a", 1);
        link.start();
        auto* socket = sockets.last();
        socket->accept();
        socket->drop("machine", "host_offline");
        QVERIFY(!stream.hostReachable());
        QCOMPARE(reach.size(), 1);
        QCOMPARE(reach.last().at(0).toBool(), false);
        // It keeps trying, never faster than its wait, and shows nothing it did not read.
        QTRY_VERIFY_WITH_TIMEOUT(socket->frames("machine").size() >= 2, 2000);
        socket->forward("machine", page("a", {1}, true));
        QVERIFY(stream.hostReachable());
        QCOMPARE(reach.size(), 2);
        QCOMPARE(reach.last().at(0).toBool(), true);
    }

    void aResetOrARejectedEventReloadsTheWindowBeforeEventsResume() {
        FakeFrameSockets sockets;
        MemoryCursors store;
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        RelayEventStream stream(&link, "machine", &store);
        stream.follow("a", 1);
        link.start();
        auto* socket = sockets.last();
        socket->accept();
        QSignalSpy ready(&stream, &RelayEventStream::eventsReady);
        QSignalSpy reload(&stream, &RelayEventStream::reloadRequested);
        socket->forward("machine", page("a", {1, 2}));
        socket->forward("machine", simple("session.reset", "a", 1));
        QCOMPARE(reload.size(), 1);
        // Until the window is adopted, events of the conversation are not delivered.
        socket->forward("machine", page("a", {3, 4}));
        QCOMPARE(ready.size(), 1);
        stream.adoptWindow("a", 1, 40);
        QCOMPARE(afterOf(socket->frames("machine").last()), 40);
        socket->forward("machine", page("a", {41}));
        QCOMPARE(cursors(ready.last().at(1).toJsonArray()), (QList<qint64>{41}));
        // An event the caller could not open reloads the same way, once.
        stream.eventRejected("a");
        stream.eventRejected("a");
        QCOMPARE(reload.size(), 2);
        socket->forward("machine", page("a", {42}));
        QCOMPARE(ready.size(), 2);
    }

    void openingAfterAWindowFollowsItsSnapshotNotTheStoredCursor() {
        FakeFrameSockets sockets;
        MemoryCursors store;
        store.values.insert("a:1", 3);
        RelayFrameLink link(relayOrigin, token(), sockets.factory(), quick());
        RelayEventStream stream(&link, "machine", &store);
        link.start();
        sockets.last()->accept();
        stream.open("a", 1, 25);
        const auto frames = sockets.last()->frames("machine");
        QCOMPARE(frames.size(), 1);
        QCOMPARE(afterOf(frames.first()), 25);
        QCOMPARE(store.values.value("a:1"), 25);
        // Closing the conversation unsubscribes.
        stream.unfollow("a");
        QCOMPARE(sockets.last()->frames("machine").last().value("type").toString(),
                 QStringLiteral("unsubscribe"));
    }
};

QTEST_GUILESS_MAIN(RelayEventStreamTest)
#include "RelayEventStreamTest.moc"
