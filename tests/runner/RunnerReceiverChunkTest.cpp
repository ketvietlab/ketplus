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
    QVERIFY(writeBigFile());
    const Packed packed = pack(snapshot());
    const qint64 size = protocol::limits::chunkBytes;
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    chunk(packed, 0, size, true);
    QTRY_COMPARE(lastAck(), std::optional<qint64>(size));
    chunk(packed, size, size, false);
    QTRY_COMPARE(lastAck(), std::optional<qint64>(2 * size));
    chunk(packed, 2 * size, packed.bytes.size() - 2 * size, true);
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
    QVERIFY(errorsSent().isEmpty());
    for (const protocol::SnapshotAck& ack : sentOf<protocol::SnapshotAck>())
        QVERIFY(ack.offset % size == 0 || ack.offset == packed.bytes.size());
}

void ReceiverTest::earlyChunksWaitAndRepeatsAreAckedAgain() {
    QVERIFY(writeBigFile());
    const Packed packed = pack(snapshot());
    const qint64 size = protocol::limits::chunkBytes;
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    chunk(packed, size, size); // early
    QTest::qWait(100);
    QVERIFY(!lastAck());
    chunk(packed, 0, size);
    QTRY_COMPARE(lastAck(), std::optional<qint64>(2 * size));
    const qsizetype acks = sentOf<protocol::SnapshotAck>().size();
    chunk(packed, 0, size); // a repeat
    QTRY_COMPARE(sentOf<protocol::SnapshotAck>().size(), acks + 1);
    QCOMPARE(lastAck(), std::optional<qint64>(2 * size));
    QVERIFY(sendPack(packed, 2 * size));
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
    QVERIFY(errorsSent().isEmpty());
}

void ReceiverTest::badChunksAreCorrupt() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), QByteArray(3000, 'a')));
    const Packed packed = pack(snapshot());
    const qint64 size = packed.bytes.size(); // one chunk, the last
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());

    chunk(packed, 0, size, false, packed.bytes.left(size - 1)); // shorter than its bytes
    QTRY_COMPARE(errorsSent(), QStringList{corrupt});
    QByteArray flipped = packed.bytes;
    flipped[0] = char(flipped[0] ^ 1);
    chunk(packed, 0, size, false, flipped); // wrong SHA-256
    QTRY_COMPARE(errorsSent().size(), 2);
    chunk(packed, 0, size + 2, false, packed.bytes + "xx"); // past packBytes
    QTRY_COMPARE(errorsSent().size(), 3);
    chunk(packed, 1, size - 1); // off a chunk boundary
    QTRY_COMPARE(errorsSent().size(), 4);
    chunk(packed, 0, size - 1); // short, and not the last
    QTRY_COMPARE(errorsSent().size(), 5);
    QCOMPARE(errorsSent(), QStringList(5, corrupt));
    QVERIFY(!lastAck());
    QCOMPARE(QFileInfo(partialPack()).size(), 0); // nothing written

    QVERIFY(sendPack(packed)); // a good chunk resets the count
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
}

void ReceiverTest::tooManyHeldChunksAreCorrupt() {
    QVERIFY(writeBigFile());
    const Packed packed = pack(snapshot());
    const qint64 size = protocol::limits::chunkBytes;
    begin(packed.commit);
    offer(packed);
    QVERIFY(nextWant());
    for (int i = 1; i <= 4; ++i)
        frameOnly(packed, i * size); // waiting for payloads: held
    QTest::qWait(100);
    QVERIFY(errorsSent().isEmpty());
    frameOnly(packed, 5 * size);
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
    QVERIFY(sendPack(packed));
    QVERIFY(waitAnswer());
    QCOMPARE(lastAnswer(), QStringLiteral("ready"));
}

// Chunks while no `need` is in force count for nothing; a new offer starts the count again.
void ReceiverTest::chunksOutsideNeedAreDroppedAndCountsRestart() {
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "alpha"));
    const QString base = snapshot();
    QVERIFY(writeFile(m_repo + QStringLiteral("/a.txt"), "changed"));
    const Packed delta = pack(snapshot(), base);
    QCOMPARE(deliver(delta, base), QStringLiteral("base_missing"));
    for (int i = 0; i < 6; ++i)
        chunk(delta, 0, delta.bytes.size(), false, QByteArray("bad")); // waiting for an offer
    QTest::qWait(200);
    QCOMPARE(errorsSent(), QStringList{QStringLiteral("base_missing")});
    QVERIFY(!lastAck());
    QVERIFY(m_answers.isEmpty());

    const Packed full = pack(delta.commit);
    offer(full);
    QVERIFY(nextWant());
    for (int i = 1; i <= 7; ++i) {
        chunk(full, 0, full.bytes.size(), false, QByteArray("bad"));
        QTRY_COMPARE(errorsSent().size(), 1 + i);
    }
    offer(full); // the same pack again: the count starts over
    QVERIFY(nextWant());
    for (int i = 1; i <= 7; ++i) {
        chunk(full, 0, full.bytes.size(), false, QByteArray("bad"));
        QTRY_COMPARE(errorsSent().size(), 8 + i);
    }
    QVERIFY(m_answers.isEmpty());
    QVERIFY(sendPack(full));
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
