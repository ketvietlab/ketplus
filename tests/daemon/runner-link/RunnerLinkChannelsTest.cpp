// The three channels of an open runner connection, over fakes.
#include "RunnerLinkTest.h"

#include "transport/FrameCodec.h"

#include <QtTest>

namespace runner_link_test {
namespace {

using ketplus::daemon::transport::FrameCodec;
using ketplus::daemon::transport::Packet;
const QString kId = QString::fromLatin1(vectors::connectionId);

QByteArray pattern(const qsizetype size, const int seed = 0) {
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype i = 0; i < size; ++i)
        bytes[i] = static_cast<char>(i * 13 + 5 + seed);
    return bytes;
}

// Reassembles the transfers among `packets`, in the order they complete.
QList<FrameCodec::CompletedTransfer> transfers(const QList<Packet>& packets) {
    FrameCodec codec;
    QList<FrameCodec::CompletedTransfer> done;
    for (const Packet& packet : packets) {
        QString error;
        if (auto transfer = codec.receiveTransfer(packet, 0, &error))
            done.append(*transfer);
        else if (!error.isEmpty())
            qWarning() << "transfer error" << error;
    }
    return done;
}

bool samePackets(const QList<Packet>& a, const QList<Packet>& b) {
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i)
        if (a[i].channel != b[i].channel || a[i].binary != b[i].binary || a[i].bytes != b[i].bytes)
            return false;
    return true;
}

} // namespace

void RunnerLinkChannelsTest::sendsFrames() {
    LinkHarness h;
    h.links.send(kId, FrameChannel::Control, "{\"type\":\"welcome\"}"); // not open yet
    h.deliver(offer());
    h.links.send(kId, FrameChannel::Control, "{\"type\":\"welcome\"}");
    QVERIFY(h.transports.last().sent.isEmpty());
    TransportRecord& record = h.transports.last();
    record.events->localDescription(QString::fromLatin1(vectors::answerSdp));
    record.events->opened();

    h.links.send(kId, FrameChannel::Control, "{\"type\":\"welcome\"}");
    h.links.send(kId, FrameChannel::Events, "{\"type\":\"spec\"}");
    h.links.send(kId, FrameChannel::Events, QByteArray(FrameCodec::messageLimit, 'e'));
    QCOMPARE(record.sent.size(), 3);
    QCOMPARE(record.sent[0].channel, TransportChannel::Control);
    QCOMPARE(record.sent[0].bytes, QByteArray("{\"type\":\"welcome\"}"));
    QVERIFY(!record.sent[0].binary);
    QCOMPARE(record.sent[1].channel, TransportChannel::Events);
    QCOMPARE(record.sent[2].bytes.size(), FrameCodec::messageLimit);
    h.links.send(connectionId(9), FrameChannel::Control, "{}");
    QCOMPARE(record.sent.size(), 3);
}

void RunnerLinkChannelsTest::keepsTheConnectionForFramesItCannotSend() {
    LinkHarness h;
    TransportRecord& record = h.open();
    logLines().clear();
    h.links.send(kId, FrameChannel::Control, QByteArray(FrameCodec::messageLimit + 1, 'c'));
    h.links.send(kId, FrameChannel::Events, QByteArray());
    h.links.send(kId, FrameChannel::Bulk, "{}");
    QVERIFY(record.sent.isEmpty());
    QVERIFY(h.links.owns(kId));
    QCOMPARE(h.events.events.last(), "opened " + kId);
    const QStringList lines = logLines().filter("runner frame not sent");
    QCOMPARE(lines.size(), 3);
    QVERIFY(lines[0].contains(QString::number(FrameCodec::messageLimit + 1)));
}

void RunnerLinkChannelsTest::reportsAFailedSend() {
    LinkHarness h;
    TransportRecord& record = h.open();
    record.sendOk = false;
    h.links.send(kId, FrameChannel::Control, "{}");
    QCOMPARE(h.events.events.last(), "lost " + kId + " send_failed");
    QVERIFY(record.closed);

    LinkHarness bulk;
    TransportRecord& bulkRecord = bulk.open();
    bulkRecord.sendOk = false;
    bulk.links.sendBulk(kId, QStringLiteral("0000000000000001"), pattern(100));
    QCOMPARE(bulk.events.events.last(), "lost " + kId + " send_failed");
}

void RunnerLinkChannelsTest::receivesFrames() {
    LinkHarness h;
    h.deliver(offer());
    TransportRecord& record = h.transports.last();
    record.events->received(TransportChannel::Control, "{\"type\":\"early\"}", false);
    QVERIFY(h.events.frames.isEmpty()); // before the channels opened
    record.events->localDescription(QString::fromLatin1(vectors::answerSdp));
    record.events->opened();
    record.events->received(TransportChannel::Control, "{\"type\":\"hello\"}", false);
    record.events->received(TransportChannel::Events, "not json, passed on as is", false);
    record.events->received(TransportChannel::Events, QByteArray(FrameCodec::messageLimit, 'e'),
                            false);
    QCOMPARE(h.events.frames.size(), 3);
    QCOMPARE(std::get<0>(h.events.frames[0]), kId);
    QCOMPARE(std::get<1>(h.events.frames[0]), FrameChannel::Control);
    QCOMPARE(std::get<2>(h.events.frames[0]), QByteArray("{\"type\":\"hello\"}"));
    QCOMPARE(std::get<1>(h.events.frames[1]), FrameChannel::Events);
    QCOMPARE(std::get<2>(h.events.frames[1]), QByteArray("not json, passed on as is"));
    QVERIFY(h.links.owns(kId));
}

void RunnerLinkChannelsTest::closesOnBrokenChannelRules_data() {
    QTest::addColumn<int>("channel");
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<bool>("binary");
    QTest::addColumn<QString>("reason");
    QTest::newRow("binary on control") << int(TransportChannel::Control) << QByteArray("{}") << true
                                       << QStringLiteral("invalid_transfer");
    QTest::newRow("binary on events") << int(TransportChannel::Events) << QByteArray("{}") << true
                                      << QStringLiteral("invalid_transfer");
    QTest::newRow("text on bulk") << int(TransportChannel::Bulk) << QByteArray("{}") << false
                                  << QStringLiteral("invalid_transfer");
    QTest::newRow("binary on bulk") << int(TransportChannel::Bulk) << QByteArray("\x01\x02") << true
                                    << QStringLiteral("invalid_transfer");
    QTest::newRow("above 64 KiB") << int(TransportChannel::Control)
                                  << QByteArray(FrameCodec::messageLimit + 1, 'c') << false
                                  << QStringLiteral("frame_limit");
    QTest::newRow("empty") << int(TransportChannel::Events) << QByteArray() << false
                           << QStringLiteral("frame_limit");
}

void RunnerLinkChannelsTest::closesOnBrokenChannelRules() {
    QFETCH(int, channel);
    QFETCH(QByteArray, bytes);
    QFETCH(bool, binary);
    QFETCH(QString, reason);
    LinkHarness h;
    TransportRecord& record = h.open();
    record.events->received(TransportChannel(channel), bytes, binary);
    QCOMPARE(h.events.events.last(), "lost " + kId + " " + reason);
    QVERIFY(h.events.frames.isEmpty());
    QVERIFY(record.closed);
    QVERIFY(!h.links.owns(kId));
    QVERIFY(h.out.ends().isEmpty());
}

void RunnerLinkChannelsTest::sendsTransfers() {
    LinkHarness h;
    TransportRecord& record = h.open();
    const QByteArray exact = pattern(transferSizeLimit);
    h.links.sendBulk(kId, QStringLiteral("00000000000000a1"), exact);
    QVERIFY(samePackets(record.sent, FrameCodec::encodeTransfer(0xa1, exact)));
    for (const Packet& packet : record.sent) {
        QCOMPARE(packet.channel, TransportChannel::Bulk);
        QVERIFY(packet.binary);
    }
    auto done = transfers(record.sent);
    QCOMPARE(done.size(), 1);
    QCOMPARE(done[0].transferId, quint64(0xa1));
    QVERIFY(done[0].bytes == exact);

    // Four in a row go one after the other, never interleaved.
    record.sent.clear();
    QList<QByteArray> sent;
    for (int n = 0; n < 4; ++n) {
        sent.append(pattern(1024 * 1024 + n * 4096, n));
        h.links.sendBulk(kId, QStringLiteral("ffffffffffff000%1").arg(n), sent.last());
    }
    QList<Packet> expected;
    for (int n = 0; n < 4; ++n)
        expected += FrameCodec::encodeTransfer(0xffffffffffff0000ULL + n, sent[n]);
    QVERIFY(samePackets(record.sent, expected));
    done = transfers(record.sent);
    QCOMPARE(done.size(), 4);
    for (int n = 0; n < 4; ++n) {
        QCOMPARE(done[n].transferId, 0xffffffffffff0000ULL + n);
        QVERIFY(done[n].bytes == sent[n]);
    }
    QCOMPARE(h.events.events.last(), "opened " + kId);
}

void RunnerLinkChannelsTest::keepsTheConnectionForTransfersItCannotSend_data() {
    QTest::addColumn<QString>("transferId");
    QTest::addColumn<qsizetype>("size");
    QTest::addColumn<bool>("open");
    const QString good = QStringLiteral("0123456789abcdef");
    QTest::newRow("empty") << good << qsizetype(0) << true;
    QTest::newRow("above 4 MiB") << good << qsizetype(transferSizeLimit + 1) << true;
    QTest::newRow("id all zero") << QStringLiteral("0000000000000000") << qsizetype(10) << true;
    QTest::newRow("id upper case") << QStringLiteral("0123456789ABCDEF") << qsizetype(10) << true;
    QTest::newRow("id of 15") << QStringLiteral("0123456789abcde") << qsizetype(10) << true;
    QTest::newRow("id of 17") << QStringLiteral("0123456789abcdef0") << qsizetype(10) << true;
    QTest::newRow("not open") << good << qsizetype(10) << false;
}

void RunnerLinkChannelsTest::keepsTheConnectionForTransfersItCannotSend() {
    QFETCH(QString, transferId);
    QFETCH(qsizetype, size);
    QFETCH(bool, open);
    LinkHarness h;
    if (open)
        h.open();
    else
        h.deliver(offer());
    logLines().clear();
    h.links.sendBulk(kId, transferId, pattern(size));
    QVERIFY(h.transports.last().sent.isEmpty());
    QVERIFY(h.links.owns(kId));
    QVERIFY(!h.events.events.last().startsWith("lost"));
    QCOMPARE(logLines().filter("runner transfer not sent").size(), 1);
}

void RunnerLinkChannelsTest::closesOnBulkOverflow() {
    LinkHarness h;
    TransportRecord& record = h.open();
    const QByteArray bytes = pattern(transferSizeLimit);
    // Up to four transfers waiting is fine.
    record.buffered[int(TransportChannel::Bulk)] = bulkBufferLimit - transferSizeLimit;
    h.links.sendBulk(kId, QStringLiteral("0000000000000001"), bytes);
    QVERIFY(!record.sent.isEmpty());
    record.sent.clear();
    // One byte more would leave more than that: nothing goes, the connection ends.
    record.buffered[int(TransportChannel::Bulk)] = bulkBufferLimit - transferSizeLimit + 1;
    h.links.sendBulk(kId, QStringLiteral("0000000000000002"), bytes);
    QVERIFY(record.sent.isEmpty());
    QCOMPARE(h.events.events.last(), "lost " + kId + " bulk_overflow");
    QVERIFY(record.closed);
    QVERIFY(!h.links.owns(kId));
}

void RunnerLinkChannelsTest::tellsWhenBulkIsWritableAgain() {
    LinkHarness h;
    TransportRecord& record = h.open();
    auto& buffered = record.buffered[int(TransportChannel::Bulk)];
    buffered = bulkWritableLimit - 1;
    QVERIFY(h.links.bulkWritable(kId));
    record.events->writable(); // never said false: nothing to tell
    QCOMPARE(h.events.events.last(), "opened " + kId);

    buffered = bulkWritableLimit;
    QVERIFY(!h.links.bulkWritable(kId));
    QVERIFY(!h.links.bulkWritable(kId));
    record.events->writable(); // still above
    QCOMPARE(h.events.events.last(), "opened " + kId);
    buffered = 64 * 1024;
    record.events->writable();
    record.events->writable();
    QCOMPARE(h.events.events.filter("writable").size(), 1);
    QCOMPARE(h.events.events.last(), "writable " + kId);

    // Not after the connection ended.
    buffered = bulkWritableLimit;
    QVERIFY(!h.links.bulkWritable(kId));
    h.links.close(kId, QStringLiteral("done"));
    QVERIFY(!record.alive);
    QVERIFY(!h.links.bulkWritable(kId));
    QCOMPARE(h.events.events.filter("writable").size(), 1);
    QVERIFY(!h.links.bulkWritable(connectionId(9)));
}

void RunnerLinkChannelsTest::logsNoSecrets() {
    LinkHarness h;
    logLines().clear();
    const QString sdp = QStringLiteral("v=0\r\na=") + sdpMarker + QStringLiteral("\r\n");
    h.deliver(offer({.sdp = sdp}));
    TransportRecord& record = h.transports.last();
    h.deliver({{"type", "signal.candidate"},
               {"connectionId", kId},
               {"candidate", QStringLiteral("candidate:") + candidateMarker},
               {"sdpMid", "0"}});
    record.events->localDescription(QStringLiteral("v=0\r\na=") + sdpMarker + "-answer\r\n");
    record.events->localCandidate(QStringLiteral("candidate:") + candidateMarker + "-local",
                                  QStringLiteral("0"));
    record.events->opened();
    h.links.send(kId, FrameChannel::Control, QByteArray(FrameCodec::messageLimit + 1, 'F'));
    record.events->received(TransportChannel::Control, "{\"secret\":\"FRAME-SECRET\"}", false);
    h.links.sendBulk(kId, QStringLiteral("0000000000000001"), "BULK-SECRET");
    // A refused offer too.
    QJsonObject tampered = offer({.connectionId = connectionId(2), .sdp = sdp});
    tampered["sdp"] = sdp + "x";
    h.deliver(tampered);
    record.events->closed();

    QVERIFY(!logLines().isEmpty());
    const QString all = logLines().join('\n');
    const QString answer = h.out.ofType("signal.answer").value(0).value("signature").toString();
    const QString offerSignature = offer({.sdp = sdp}).value("signature").toString();
    for (const QString& secret :
         {QString::fromLatin1(turnMarker), QString::fromLatin1(sdpMarker),
          QString::fromLatin1(candidateMarker), QStringLiteral("FRAME-SECRET"),
          QStringLiteral("BULK-SECRET"), QStringLiteral("FFFFFFFF"), answer, offerSignature,
          QStringLiteral("candidate:"), QStringLiteral("v=0")}) {
        QVERIFY(!secret.isEmpty());
        QVERIFY2(!all.contains(secret), qPrintable(secret));
    }
}

} // namespace runner_link_test
