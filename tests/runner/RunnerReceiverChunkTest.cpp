#include "RunnerReceiverTest.h"

#include "protocol/FrameLimits.h"

#include <QFileInfo>
#include <QTest>

namespace runner_receiver_test {

using protocol::SnapshotWant;

namespace {
const QString corrupt = QStringLiteral("chunk_corrupt");
}

void ReceiverTest::framesAndPayloadsPairInAnyOrder() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QByteArray(3000, 'a')));
    const Packed packed = pack(snapshot());
    const qint64 third = packed.bytes.size() / 3;
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    chunk(packed, 0, third, true);
    QTRY_COMPARE(lastAck(), std::optional<qint64>(third));
    chunk(packed, third, third, false);
    QTRY_COMPARE(lastAck(), std::optional<qint64>(2 * third));
    chunk(packed, 2 * third, packed.bytes.size() - 2 * third, true);
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
    QVERIFY(errorsSent().isEmpty());
}

void ReceiverTest::earlyChunksWaitAndRepeatsAreAckedAgain() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QByteArray(3000, 'a')));
    const Packed packed = pack(snapshot());
    const qint64 third = packed.bytes.size() / 3;
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    chunk(packed, third, third); // early
    QTest::qWait(100);
    QVERIFY(!lastAck());
    chunk(packed, 0, third);
    QTRY_COMPARE(lastAck(), std::optional<qint64>(2 * third));
    const qsizetype acks = sentOf<protocol::SnapshotAck>().size();
    chunk(packed, 0, third); // a repeat
    QTRY_COMPARE(sentOf<protocol::SnapshotAck>().size(), acks + 1);
    QCOMPARE(lastAck(), std::optional<qint64>(2 * third));
    QVERIFY(sendPack(packed, 2 * third, packed.bytes.size()));
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
    QVERIFY(errorsSent().isEmpty());
}

void ReceiverTest::badChunksAreCorrupt() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QByteArray(3000, 'a')));
    const Packed packed = pack(snapshot());
    const qint64 half = packed.bytes.size() / 2;
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());

    chunk(packed, 0, half, false, packed.bytes.left(half - 1)); // shorter than its bytes
    QTRY_COMPARE(errorsSent(), QStringList{corrupt});
    QByteArray flipped = packed.bytes.left(half);
    flipped[0] = char(flipped[0] ^ 1);
    chunk(packed, 0, half, false, flipped); // wrong SHA-256
    QTRY_COMPARE(errorsSent(), (QStringList{corrupt, corrupt}));
    chunk(packed, half, half + 2, false, packed.bytes.mid(half) + "xx"); // past packBytes
    QTRY_COMPARE(errorsSent(), (QStringList{corrupt, corrupt, corrupt}));
    QVERIFY(!lastAck());
    QCOMPARE(QFileInfo(partialPack()).size(), 0); // nothing written

    QVERIFY(sendPack(packed)); // a good chunk resets the count
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
}

void ReceiverTest::tooManyHeldChunksAreCorrupt() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QByteArray(3000, 'a')));
    const Packed packed = pack(snapshot());
    const qint64 size = packed.bytes.size() / 8;
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    for (int i = 1; i <= 4; ++i)
        chunk(packed, i * size, size); // all early: held
    QTest::qWait(100);
    QVERIFY(errorsSent().isEmpty());
    chunk(packed, 5 * size, size);
    QTRY_COMPARE(errorsSent(), QStringList{corrupt});
    chunk(packed, 0, size); // the held ones were dropped
    QTRY_COMPARE(lastAck(), std::optional<qint64>(size));
}

void ReceiverTest::eightCorruptChunksEndThePack() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed packed = pack(snapshot());
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    for (int i = 1; i <= 7; ++i) {
        chunk(packed, 0, packed.bytes.size(), false, QByteArray("bad"));
        QTRY_COMPARE(errorsSent().size(), i);
    }
    QVERIFY(m_answers.isEmpty());
    chunk(packed, 0, packed.bytes.size(), false, QByteArray("bad"));
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("pack_invalid"));
    QCOMPARE(errorsSent().size(), 7);
    QTRY_VERIFY(!QFileInfo::exists(partialPack()));
    QVERIFY(!QFileInfo::exists(workspace()));
}

void ReceiverTest::chunksWithoutAnOfferAreDropped() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed packed = pack(snapshot());
    chunk(packed, 0, packed.bytes.size()); // no job
    begin(packed.commit);
    chunk(packed, 0, packed.bytes.size()); // no offer
    QTest::qWait(100);
    QVERIFY(m_sent.isEmpty());
    QVERIFY(m_answers.isEmpty());
    offer(packed);
    const auto want = nextWant();
    QVERIFY(want);
    QCOMPARE(want->fromOffset, 0);
    QVERIFY(sendPack(packed, 0, packed.bytes.size()));
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
}

void ReceiverTest::oversizedPackIsRefused() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    Packed packed = pack(snapshot());
    begin(packed.commit);
    protocol::SnapshotOffer offer;
    offer.epoch = 1;
    offer.jobId = QStringLiteral("job-1");
    offer.snapshotId = packed.commit;
    offer.packBytes = protocol::limits::maxPackBytes + 1;
    offer.packSha256 = packed.sha256;
    offer.chunkBytes = protocol::limits::chunkBytes;
    m_receiver->receive(offer);
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("snapshot_too_large"));
    QVERIFY(m_sent.isEmpty());
    QVERIFY(!QFileInfo::exists(partialPack()));
}

void ReceiverTest::diskFullEndsTheJob() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const Packed packed = pack(snapshot());
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    QVERIFY(QFileInfo::exists(partialPack()));
    m_diskFull = true;
    chunk(packed, 0, packed.bytes.size());
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("disk_full"));
    QVERIFY(!lastAck());
    QTRY_VERIFY(!QFileInfo::exists(partialPack()));
    QVERIFY(!QFileInfo::exists(workspace()));
}

} // namespace runner_receiver_test
