// RelayPeerLink and the relay path of RemotePeers and Signaling, without a network: the frame
// envelope, the 4 KiB client and 64 KiB host caps, page trimming, gap on a full live queue,
// and revocation.
#include "transport/RelayPeerLink.h"
#include "transport/Signaling.h"
#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>

using namespace ketplus::daemon::transport;

namespace {
QJsonObject subscribe(const QString& session = "session", qint64 cursor = 0) {
    return {{"type", "subscribe"},
            {"sessionId", session},
            {"sessionGeneration", 1},
            {"afterCursor", cursor}};
}
QJsonObject sessionEvent(qint64 cursor, qsizetype bytes) {
    return {{"cursor", cursor}, {"content", QString(bytes, QLatin1Char('x'))}};
}
Packet text(const QJsonObject& frame, Channel channel = Channel::Events) {
    return {channel, QJsonDocument(frame).toJson(QJsonDocument::Compact), false};
}

struct Relay {
    QList<QJsonObject> sent;
    bool blocked{false};
    QStringList lookups;
    QJsonObject device{{"id", "device"}, {"keyVersion", 3}, {"status", "active"}};
    std::function<EventPage(const QString&, qint64, qint64)> reader =
        [](const QString&, qint64 generation, qint64 cursor) {
            return EventPage{{}, cursor, generation, true, false};
        };
    RemotePeers peers{[this](const QString& s, qint64 g, qint64 c) { return reader(s, g, c); }};
    Signaling signaling{&peers,
                        [this](const QString& id, int version, auto callback) {
                            lookups << QStringLiteral("%1/%2").arg(id).arg(version);
                            callback(id == device.value("id").toString() ? device : QJsonObject{});
                        },
                        [](const QByteArray&) { return QByteArray(64, 's'); }};
    QList<QJsonObject> outgoing;
    qint64 time{1'000'000'000};
    Relay() {
        peers.setClock([this] { return time; });
        signaling.configure("workspace", "machine", 1, true);
        signaling.setForwardOnly(true);
        signaling.setRelaySender([this](const QJsonObject& message) {
            if (blocked)
                return false;
            sent << message;
            return true;
        });
        QObject::connect(&signaling, &Signaling::outgoing,
                         [this](const QJsonObject& message) { outgoing << message; });
    }
    void frame(const QJsonObject& frame, const QString& device = "device") {
        signaling.receive({{"type", "frame"}, {"deviceId", device}, {"frame", frame}});
    }
    QList<QJsonObject> frames(const QString& type) const {
        QList<QJsonObject> result;
        for (const auto& message : sent)
            if (message.value("frame").toObject().value("type") == type)
                result << message.value("frame").toObject();
        return result;
    }
};
} // namespace

class RelayPeerLinkTest final : public QObject {
    Q_OBJECT
  private slots:
    void wrapsOnlyEventFramesWithinTheHostLimit() {
        QList<QJsonObject> sent;
        RelayPeerLink link("device", [&](const QJsonObject& message) {
            sent << message;
            return true;
        });
        for (const auto* type : {"events", "event", "gap", "session.reset"})
            QVERIFY(link.send(text({{"type", type}, {"sessionId", "s"}})));
        QCOMPARE(sent.size(), 4);
        QCOMPARE(sent.first().value("type").toString(), QStringLiteral("frame"));
        QCOMPARE(sent.first().value("deviceId").toString(), QStringLiteral("device"));
        QCOMPARE(sent.first().value("frame").toObject().value("type").toString(),
                 QStringLiteral("events"));
        QCOMPARE(link.buffered(Channel::Events), 0u);

        for (const auto* type : {"welcome", "ping", "command.result", "hint", "event.transfer"}) {
            RelayPeerLink refused("device", [](const QJsonObject&) { return true; });
            QSignalSpy closed(&refused, &PeerLink::closed);
            QVERIFY(!refused.send(text({{"type", type}})));
            QVERIFY(closed.wait(1000));
        }
        RelayPeerLink binary("device", [](const QJsonObject&) { return true; });
        QVERIFY(!binary.send({Channel::Bulk, QByteArray(16, 0), true}));
        RelayPeerLink large("device", [](const QJsonObject&) { return true; });
        QVERIFY(!large.send(
            text({{"type", "events"}, {"pad", QString(FrameCodec::messageLimit, 'x')}})));
        QCOMPARE(sent.size(), 4);
    }

    void acceptsOnlySmallSubscriptionFrames() {
        RelayPeerLink link("device", [](const QJsonObject&) { return true; });
        QSignalSpy received(&link, &PeerLink::received);
        QVERIFY(link.deliver(subscribe()));
        QVERIFY(link.deliver({{"type", "unsubscribe"}, {"sessionId", "session"}}));
        QVERIFY(!link.deliver({{"type", "command"}}));
        QVERIFY(!link.deliver({{"type", "hello"}}));
        QVERIFY(!link.deliver(
            {{"type", "subscribe"}, {"sessionId", QString(RelayPeerLink::clientFrameLimit, 'x')}}));
        QCOMPARE(received.size(), 2);
        link.close();
        QVERIFY(!link.deliver(subscribe()));
        QVERIFY(!link.send(text({{"type", "events"}})));
    }

    void subscriptionOpensOneLinkPerDeviceAndPagesEvents() {
        Relay relay;
        relay.frame(subscribe());
        relay.frame(subscribe("other"));
        QCOMPARE(relay.lookups, QStringList{"device/0"});
        QCOMPARE(relay.peers.size(), 1);
        QVERIFY(qobject_cast<RelayPeerLink*>(relay.peers.link(RemotePeers::relayId("device"))));
        relay.peers.tick();
        relay.peers.tick();
        const auto pages = relay.frames("events");
        QCOMPARE(pages.size(), 2);
        QVERIFY(pages.first().value("live").toBool());
        QVERIFY(pages.first().contains("epoch"));
        // Nothing but event frames ever leaves on this path: no welcome, no ping.
        for (const auto& message : relay.sent)
            QVERIFY(RelayPeerLink::hostFrame(
                message.value("frame").toObject().value("type").toString()));
        QVERIFY(relay.outgoing.isEmpty());
    }

    void unknownOrInactiveDevicesOpenNothing() {
        Relay relay;
        relay.frame(subscribe(), "stranger");
        relay.frame({{"type", "unsubscribe"}, {"sessionId", "s"}});
        relay.frame(subscribe(), "bad device id");
        QCOMPARE(relay.peers.size(), 0);
        relay.device.insert("status", "revoked");
        relay.frame(subscribe());
        QCOMPARE(relay.peers.size(), 0);

        Relay off;
        off.signaling.setForwardOnly(false);
        off.frame(subscribe());
        QVERIFY(off.lookups.isEmpty());
        QCOMPARE(off.peers.size(), 0);
    }

    void trimsPagesToOneRelayMessage() {
        Relay relay;
        relay.reader = [](const QString&, qint64 generation, qint64 cursor) {
            QJsonArray events;
            for (int i = 1; i <= 100; ++i)
                events.append(sessionEvent(cursor + i, 7 * 1024));
            return EventPage{events, cursor + 100, generation, false, false};
        };
        relay.frame(subscribe());
        relay.peers.tick();
        const auto pages = relay.frames("events");
        QCOMPARE(pages.size(), 1);
        const auto page = pages.first().value("page").toObject();
        const auto events = page.value("events").toArray();
        QVERIFY(!events.isEmpty());
        QVERIFY(events.size() < 100);
        QCOMPARE(page.value("nextCursor").toInteger(),
                 events.last().toObject().value("cursor").toInteger());
        QVERIFY(page.value("hasMore").toBool());
        QVERIFY(QJsonDocument(relay.sent.first()).toJson(QJsonDocument::Compact).size() <=
                FrameCodec::messageLimit + 256);
    }

    void skipsAnEventAboveTheRelayLimitWithGap() {
        Relay relay;
        relay.reader = [](const QString&, qint64 generation, qint64 cursor) {
            return EventPage{QJsonArray{sessionEvent(cursor + 1, FrameCodec::messageLimit)},
                             cursor + 1, generation, true, false};
        };
        relay.frame(subscribe("session", 7));
        relay.peers.tick();
        const auto gaps = relay.frames("gap");
        QCOMPARE(gaps.size(), 1);
        QCOMPARE(gaps.first().value("afterCursor").toInteger(), 8);
        QVERIFY(relay.frames("events").isEmpty());
    }

    void resetsAClientTooFarBehind() {
        Relay relay;
        relay.reader = [](const QString&, qint64 generation, qint64) {
            return EventPage{{}, 0, generation + 1, false, true};
        };
        relay.frame(subscribe());
        relay.peers.tick();
        QCOMPARE(relay.frames("session.reset").size(), 1);
    }

    void fullLiveQueueDropsEventsAndSendsGap() {
        Relay relay;
        relay.frame(subscribe("session", 5));
        const auto id = RemotePeers::relayId("device");
        relay.blocked = true;
        const QJsonObject page{{"events", QJsonArray{sessionEvent(6, 60 * 1024)}}};
        int accepted = 0;
        while (relay.peers.send(id, {{"type", "events"}, {"sessionId", "session"}, {"page", page}},
                                Channel::Events))
            ++accepted;
        QVERIFY(accepted >= 60); // 4 MiB of ~60 KiB frames
        QCOMPARE(relay.peers.size(), 1);
        relay.blocked = false;
        relay.peers.tick();
        const auto gaps = relay.frames("gap");
        QCOMPARE(gaps.size(), 1);
        QCOMPARE(gaps.first().value("sessionId").toString(), QStringLiteral("session"));
        QCOMPARE(gaps.first().value("afterCursor").toInteger(), 5);
    }

    void refusedFramesAndDropsCloseTheLink() {
        Relay relay;
        relay.frame(subscribe());
        relay.frame({{"type", "command"}, {"command", QJsonObject{}}});
        QCOMPARE(relay.peers.size(), 0);
        relay.frame(subscribe());
        QCOMPARE(relay.peers.size(), 1);
        relay.signaling.receive(
            {{"type", "frames.dropped"}, {"deviceId", "device"}, {"reason", "queue_full"}});
        QCOMPARE(relay.peers.size(), 0);
        relay.frame(subscribe());
        relay.signaling.relayLost();
        QCOMPARE(relay.peers.size(), 0);
        relay.frame(subscribe());
        relay.signaling.setForwardOnly(false);
        QCOMPARE(relay.peers.size(), 0);
        QVERIFY(relay.outgoing.isEmpty()); // no signal.end for relay links
    }

    void envelopeCountsTowardsTheMessageLimit() {
        QList<QJsonObject> sent;
        RelayPeerLink link("device", [&](const QJsonObject& message) {
            sent << message;
            return true;
        });
        QSignalSpy closed(&link, &PeerLink::closed);
        // Within the limit alone, above it with the envelope: refused and closed, never retried.
        const auto frame =
            text({{"type", "events"}, {"pad", QString(FrameCodec::messageLimit - 40, 'x')}});
        QVERIFY(frame.bytes.size() <= FrameCodec::messageLimit);
        QVERIFY(!link.send(frame));
        QVERIFY(closed.wait(1000));
        QVERIFY(sent.isEmpty());
    }

    void authorizationExpiresTenMinutesAfterTheLastSubscribe() {
        Relay relay;
        relay.frame(subscribe());
        relay.time += RemotePeers::relayAuthorizationMs - 1;
        relay.peers.tick();
        QCOMPARE(relay.peers.size(), 1);
        relay.frame(subscribe("second", 9)); // renews
        relay.time += RemotePeers::relayAuthorizationMs - 1;
        relay.peers.tick();
        relay.peers.tick();
        QCOMPARE(relay.peers.size(), 1);
        relay.frame({{"type", "unsubscribe"}, {"sessionId", "other"}}); // does not renew
        relay.sent.clear();
        relay.time += 2;
        QSignalSpy ended(&relay.peers, &RemotePeers::ended);
        relay.peers.tick();
        QCOMPARE(relay.peers.size(), 0);
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.first().at(1).toString(), QStringLiteral("peer_expired"));
        // Each subscription learns to subscribe again after its cursor before the link closes.
        QMap<QString, qint64> gaps;
        for (const auto& gap : relay.frames("gap"))
            gaps.insert(gap.value("sessionId").toString(), gap.value("afterCursor").toInteger());
        QCOMPARE(gaps, (QMap<QString, qint64>{{"session", 0}, {"second", 9}}));
        QCOMPARE(relay.sent.size(), 2);

        relay.frame(subscribe());
        relay.time += RemotePeers::relayAuthorizationMs;
        relay.sent.clear();
        relay.frame(subscribe()); // a frame after expiry closes instead of renewing
        QCOMPARE(relay.peers.size(), 0);
        QCOMPARE(relay.frames("gap").size(), 1);
        QVERIFY(!relay.peers.send(RemotePeers::relayId("device"), {{"type", "gap"}}));
    }

    void revocationClosesTheDevicesRelayLink() {
        Relay relay;
        relay.frame(subscribe());
        QCOMPARE(relay.peers.size(), 1);
        relay.signaling.receive({{"type", "device.revoked"}, {"deviceId", "device"}});
        QCOMPARE(relay.peers.size(), 0);

        relay.frame(subscribe());
        QCOMPARE(relay.peers.size(), 1);
        relay.signaling.refreshDevices(QJsonArray{relay.device});
        QCOMPARE(relay.peers.size(), 1);
        // The same device with another key version no longer matches the link's admission.
        relay.signaling.refreshDevices(
            QJsonArray{QJsonObject{{"id", "device"}, {"keyVersion", 4}, {"status", "active"}}});
        QCOMPARE(relay.peers.size(), 0);

        relay.frame(subscribe());
        QCOMPARE(relay.peers.size(), 1);
        relay.signaling.devicePinChanged("other");
        QCOMPARE(relay.peers.size(), 1);
        relay.signaling.devicePinChanged("device");
        QCOMPARE(relay.peers.size(), 0);

        relay.frame(subscribe());
        relay.signaling.refreshDevices(
            QJsonArray{QJsonObject{{"id", "device"}, {"keyVersion", 3}, {"status", "revoked"}}});
        QCOMPARE(relay.peers.size(), 0);
    }
};

QTEST_GUILESS_MAIN(RelayPeerLinkTest)
#include "RelayPeerLinkTest.moc"
