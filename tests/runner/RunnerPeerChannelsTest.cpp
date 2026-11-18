// The session's peer channel over an open fake transport: frames, transfers and limits.
#include "RunnerPeerTest.h"

#include "transport/FrameCodec.h"

#include <QtTest>

namespace runner_peer_test {
namespace {

using ketplus::daemon::transport::FrameCodec;

const QString kId = QString::fromLatin1(vectors::connectionId);

QByteArray pattern(const qsizetype size, const char seed) {
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        bytes[i] = static_cast<char>(seed + i * 7);
    return bytes;
}

void deliver(PeerHarness& h, const QList<ketplus::daemon::transport::Packet>& packets) {
    for (const auto& packet : packets)
        h.connector.received(packet.channel, packet.bytes, packet.binary);
}

} // namespace

void RunnerPeerChannelsTest::openedAndLostAreReportedOnce() {
    PeerHarness h;
    h.open();
    QCOMPARE(h.listener.events, QStringList{"opened " + kId});
    h.connector.closed();
    h.connector.closed();
    h.connector.received(Channel::Control, "{}", false);
    QCOMPARE(h.listener.events, (QStringList{"opened " + kId, QStringLiteral("lost peer_closed")}));
    QVERIFY(h.listener.frames.isEmpty());
}

void RunnerPeerChannelsTest::sessionCloseReportsNothingLater() {
    PeerHarness h;
    h.open();
    h.channels.close(QStringLiteral("job_done"));
    QVERIFY(h.transports.last().closed);
    h.connector.closed();
    h.connector.received(Channel::Control, "{}", false);
    QCOMPARE(h.listener.events, QStringList{"opened " + kId});
    QVERIFY(h.listener.frames.isEmpty());
    QVERIFY(!h.channels.eventsWritable());
}

void RunnerPeerChannelsTest::framesPassThroughAsBytes() {
    PeerHarness h;
    h.open();
    // Not JSON: the channel layer passes it on and leaves parsing to the session.
    h.connector.received(Channel::Control, "not json", false);
    h.connector.received(Channel::Events, QByteArray(FrameCodec::messageLimit, 'e'), false);
    QCOMPARE(h.listener.frames.size(), 2);
    QCOMPARE(h.listener.frames[0].first, protocol::Channel::Control);
    QCOMPARE(h.listener.frames[0].second, QByteArray("not json"));
    QCOMPARE(h.listener.frames[1].first, protocol::Channel::Events);
    QCOMPARE(h.listener.frames[1].second.size(), FrameCodec::messageLimit);
    QCOMPARE(h.listener.events, QStringList{"opened " + kId});
}

void RunnerPeerChannelsTest::receiveViolationsCloseTheConnection_data() {
    QTest::addColumn<int>("channel");
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<bool>("binary");
    QTest::addColumn<QString>("reason");
    const QByteArray transfer = FrameCodec::encodeTransfer(1, "abc").first().bytes;
    QTest::newRow("control too large")
        << int(Channel::Control) << QByteArray(FrameCodec::messageLimit + 1, 'x') << false
        << "frame_limit";
    QTest::newRow("events empty") << int(Channel::Events) << QByteArray() << false << "frame_limit";
    QTest::newRow("binary on control")
        << int(Channel::Control) << transfer << true << "invalid_transfer";
    QTest::newRow("binary on events")
        << int(Channel::Events) << transfer << true << "invalid_transfer";
    QTest::newRow("text on bulk") << int(Channel::Bulk) << QByteArray("{}") << false
                                  << "invalid_transfer";
    QTest::newRow("bulk too large")
        << int(Channel::Bulk) << QByteArray(FrameCodec::messageLimit + 1, 'x') << true
        << "frame_limit";
    QTest::newRow("bulk header only")
        << int(Channel::Bulk) << transfer.left(12) << true << "invalid_transfer";
    QByteArray zero = transfer;
    zero.replace(0, 8, QByteArray(8, '\0'));
    QTest::newRow("bulk id zero") << int(Channel::Bulk) << zero << true << "transfer_limit";
    QByteArray later = transfer;
    later[11] = 1;
    QTest::newRow("bulk starts mid-way") << int(Channel::Bulk) << later << true << "transfer_limit";
}

void RunnerPeerChannelsTest::receiveViolationsCloseTheConnection() {
    QFETCH(int, channel);
    QFETCH(QByteArray, bytes);
    QFETCH(bool, binary);
    QFETCH(QString, reason);
    PeerHarness h;
    h.open();
    h.connector.received(static_cast<Channel>(channel), bytes, binary);
    QCOMPARE(h.listener.events, (QStringList{"opened " + kId, "lost " + reason}));
    QVERIFY(h.transports.last().closed);
    QVERIFY(!h.connector.isOpen());
    h.connector.received(Channel::Control, "{}", false);
    QVERIFY(h.listener.frames.isEmpty());
    QCOMPARE(h.listener.events.size(), 2);
}

void RunnerPeerChannelsTest::sendKeepsToControlAndEvents() {
    PeerHarness idle;
    idle.channels.send(protocol::Channel::Control, "{}");
    QVERIFY(idle.transports.records.isEmpty());

    PeerHarness h;
    h.open();
    h.channels.send(protocol::Channel::Control, "{\"a\":1}");
    h.channels.send(protocol::Channel::Events, QByteArray(FrameCodec::messageLimit, 'e'));
    h.channels.send(protocol::Channel::Bulk, "{}");
    h.channels.send(protocol::Channel::Events, QByteArray(FrameCodec::messageLimit + 1, 'e'));
    h.channels.send(protocol::Channel::Control, {});
    const auto& sent = h.transports.last().sent;
    QCOMPARE(sent.size(), 2);
    QCOMPARE(sent[0].channel, Channel::Control);
    QCOMPARE(sent[0].bytes, QByteArray("{\"a\":1}"));
    QVERIFY(!sent[0].binary);
    QCOMPARE(sent[1].channel, Channel::Events);
    QVERIFY(!sent[1].binary);
    // Refused sends keep the connection.
    QVERIFY(h.connector.isOpen());
    QCOMPARE(h.listener.events, QStringList{"opened " + kId});
}

void RunnerPeerChannelsTest::eventsWritableIsReportedOncePerChange() {
    PeerHarness h;
    QVERIFY(!h.channels.eventsWritable());
    h.open();
    QVERIFY(h.channels.eventsWritable());
    h.connector.writable();
    QCOMPARE(h.listener.events.count(QStringLiteral("writable")), 0);

    h.transports.last().bufferedEvents = eventsBufferLimit;
    QVERIFY(!h.channels.eventsWritable());
    h.connector.writable();
    QCOMPARE(h.listener.events.count(QStringLiteral("writable")), 0);
    h.transports.last().bufferedEvents = eventsBufferLimit - 1;
    h.connector.writable();
    h.connector.writable();
    QCOMPARE(h.listener.events.count(QStringLiteral("writable")), 1);

    h.transports.last().bufferedEvents = eventsBufferLimit + 10;
    QVERIFY(!h.channels.eventsWritable());
    h.transports.last().bufferedEvents = 0;
    h.connector.writable();
    QCOMPARE(h.listener.events.count(QStringLiteral("writable")), 2);
}

void RunnerPeerChannelsTest::aFourMiBTransferArrivesWithItsId() {
    PeerHarness h;
    h.open();
    const QByteArray bytes = pattern(4 * 1024 * 1024, 3);
    const quint64 id = 0xfedcba9876543210ULL;
    deliver(h, FrameCodec::encodeTransfer(id, bytes));
    QCOMPARE(h.listener.transfers.size(), 1);
    QCOMPARE(h.listener.transfers[0].first, id);
    QCOMPARE(h.listener.transfers[0].second.size(), 4'194'304);
    QVERIFY(h.listener.transfers[0].second == bytes);
    QVERIFY(h.listener.frames.isEmpty());
    QCOMPARE(h.listener.events, QStringList{"opened " + kId});
}

void RunnerPeerChannelsTest::fourInterleavedTransfersAndAFifth() {
    PeerHarness h;
    h.open();
    QList<QList<ketplus::daemon::transport::Packet>> transfers;
    for (quint64 id = 1; id <= 5; ++id)
        transfers.append(FrameCodec::encodeTransfer(id, pattern(200'000, char(id))));
    for (int part = 0; part < transfers[0].size(); ++part) {
        for (int t = 0; t < 4; ++t)
            h.connector.received(Channel::Bulk, transfers[t][part].bytes, true);
        if (part == 0) {
            // Four are open: a fifth may not begin.
            h.connector.received(Channel::Bulk, transfers[4][0].bytes, true);
            break;
        }
    }
    QCOMPARE(h.listener.events, (QStringList{"opened " + kId, "lost transfer_limit"}));
    QVERIFY(h.listener.transfers.isEmpty());

    PeerHarness ok;
    ok.open();
    for (int part = 0; part < transfers[0].size(); ++part)
        for (int t = 0; t < 4; ++t)
            ok.connector.received(Channel::Bulk, transfers[t][part].bytes, true);
    QCOMPARE(ok.listener.transfers.size(), 4);
    for (int t = 0; t < 4; ++t) {
        QCOMPARE(ok.listener.transfers[t].first, quint64(t + 1));
        QVERIFY(ok.listener.transfers[t].second == pattern(200'000, char(t + 1)));
    }
    // With those done, the fifth fits.
    deliver(ok, transfers[4]);
    QCOMPARE(ok.listener.transfers.size(), 5);
    QCOMPARE(ok.listener.events, QStringList{"opened " + kId});
}

void RunnerPeerChannelsTest::transfersExpireAfterFiveMinutes() {
    const auto parts = FrameCodec::encodeTransfer(9, pattern(200'000, 9));
    QVERIFY(parts.size() > 2);

    PeerHarness slow;
    slow.open();
    slow.connector.received(Channel::Bulk, parts[0].bytes, true);
    slow.clock.advance(runnerTransferTimeoutMs);
    for (int i = 1; i < parts.size(); ++i)
        slow.connector.received(Channel::Bulk, parts[i].bytes, true);
    QCOMPARE(slow.listener.transfers.size(), 1);

    PeerHarness late;
    late.open();
    late.connector.received(Channel::Bulk, parts[0].bytes, true);
    late.clock.advance(runnerTransferTimeoutMs + 1);
    late.connector.received(Channel::Bulk, parts[1].bytes, true);
    QCOMPARE(late.listener.events, (QStringList{"opened " + kId, "lost frame_limit"}));
    QVERIFY(late.listener.transfers.isEmpty());
}

void RunnerPeerChannelsTest::anUnfinishedTransferDiesWithItsConnection() {
    const auto parts = FrameCodec::encodeTransfer(5, pattern(200'000, 5));
    PeerHarness h;
    h.open();
    h.connector.received(Channel::Bulk, parts[0].bytes, true);
    h.connector.closed();
    h.listener.events.clear();
    h.request(2);
    h.clock.advance(h.clock.remaining(PeerTimer::AttemptRetry));
    h.started(QStringLiteral("pc_bbbbbbbbbbbbbbbbbbbbbbbb"));
    h.describe();
    h.answer(QStringLiteral("pc_bbbbbbbbbbbbbbbbbbbbbbbb"));
    h.connector.opened();
    // The rest of the old transfer cannot continue on the new connection.
    h.connector.received(Channel::Bulk, parts[1].bytes, true);
    QCOMPARE(h.listener.events, (QStringList{QStringLiteral("opened pc_bbbbbbbbbbbbbbbbbbbbbbbb"),
                                             QStringLiteral("lost transfer_limit")}));
    QVERIFY(h.listener.transfers.isEmpty());
}

void RunnerPeerChannelsTest::requestConnectionAsksForTheCurrentJob() {
    PeerHarness none;
    none.channels.requestConnection();
    QVERIFY(none.sender.sent.isEmpty());

    PeerHarness h;
    h.open();
    h.clock.advance(startSpacingMs);
    h.channels.requestConnection();
    QCOMPARE(h.listener.events, (QStringList{"opened " + kId, QStringLiteral("lost replaced")}));
    const auto starts = h.sender.ofType(QStringLiteral("signal.start"));
    QCOMPARE(starts.size(), 2);
    QCOMPARE(starts.last().value("jobId").toString(), QStringLiteral("job_42"));
    QCOMPARE(starts.last().value("leaseId").toString(), QStringLiteral("lease_1"));

    h.channels.setRequest(std::nullopt);
    h.clock.advance(startSpacingMs);
    h.channels.requestConnection();
    QCOMPARE(h.sender.ofType(QStringLiteral("signal.start")).size(), 2);
}

} // namespace runner_peer_test
