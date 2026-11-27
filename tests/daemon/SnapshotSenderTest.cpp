#include "SnapshotSyncFakes.h"

#include <QTest>

using namespace snapshot_sync_test;

namespace {

// A pack of snapshot A on `baseId` the retention still keeps for the job, as after a restart.
PackFile keptPack(Harness& h, const QString& baseId) {
    PackFile pack{QStringLiteral("/packs/kept.pack"), 3 * chunk, sha256(QByteArray("kept")),
                  snapshotA, baseId};
    h.retention.packs.insert(jobId, pack);
    h.work.seeds[pack.path] = 9;
    return pack;
}

} // namespace

class SnapshotSenderTest final : public QObject {
    Q_OBJECT

  private slots:
    void offerCarriesTheProtocolFields() {
        Harness h(snapshotA,
                  {QStringLiteral("lfs_not_synced"), QStringLiteral("submodules_not_synced")});
        const PackFile pack = h.offered(1234);
        QCOMPARE(h.work.builds.size(), 0u);
        const auto offers = h.link.of<protocol::SnapshotOffer>();
        QCOMPARE(offers.size(), 1);
        const auto& offer = offers.first();
        QCOMPARE(offer.jobId, jobId);
        QCOMPARE(offer.snapshotId, snapshotA);
        QVERIFY(!offer.baseId.has_value());
        QCOMPARE(offer.packBytes, 1234);
        QCOMPARE(offer.packSha256, pack.sha256);
        QCOMPARE(offer.chunkBytes, 4194304);
        QCOMPARE(offer.warnings, QStringList({QStringLiteral("lfs_not_synced"),
                                              QStringLiteral("submodules_not_synced")}));
        QVERIFY(!protocol::validate(offer, protocol::Direction::DesktopToRunner).has_value());
        QCOMPARE(h.sender->phase(), SnapshotSender::Phase::Offered);
        QVERIFY(h.retention.pack(jobId).has_value());
    }

    void theBaseNeedsAConfirmationAndTheCurrentHello_data() {
        QTest::addColumn<bool>("confirmed");
        QTest::addColumn<QString>("listed");
        QTest::addColumn<QString>("base");
        QTest::newRow("confirmed and listed") << true << snapshotB << snapshotB;
        QTest::newRow("confirmed, not listed") << true << QString() << QString();
        QTest::newRow("confirmed, another listed") << true << snapshotA << QString();
        QTest::newRow("listed, not confirmed") << false << snapshotB << QString();
    }
    void theBaseNeedsAConfirmationAndTheCurrentHello() {
        QFETCH(bool, confirmed);
        QFETCH(QString, listed);
        QFETCH(QString, base);
        Harness h;
        if (confirmed)
            h.retention.confirmedRefs[{runnerId, workspace}] = snapshotB;
        QMap<QString, QString> hello;
        if (!listed.isEmpty())
            hello.insert(workspace, listed);
        h.sender->start(hello);
        QCOMPARE(h.work.builds.front().baseId, base);
        h.work.finishBuild(10);
        const auto offer = h.link.of<protocol::SnapshotOffer>().first();
        QCOMPARE(offer.baseId.value_or(QString()), base);
    }

    void aBaseTheStoreLostGivesAFullPack() {
        Harness h;
        h.retention.confirmedRefs[{runnerId, workspace}] = snapshotB;
        h.sender->start({{workspace, snapshotB}});
        h.work.failBuild(PackOutcome{std::nullopt, true, {}});
        QCOMPARE(h.work.builds.size(), 1u);
        QCOMPARE(h.work.builds.front().baseId, QString());
        QVERIFY(!h.retention.confirmed(runnerId, workspace).has_value());
        h.work.finishBuild(10);
        QVERIFY(!h.link.of<protocol::SnapshotOffer>().first().baseId.has_value());
    }

    void aPackAboveTheLimitIsNeverOffered() {
        Harness h;
        h.limits.maxPackBytes = 100;
        h.renew();
        h.offered(101);
        QVERIFY(h.link.frames.isEmpty());
        QCOMPARE(h.events.failed,
                 (QList<std::pair<QString, QString>>{{jobId, code::snapshotTooLarge}}));
        QCOMPARE(h.retention.released, QStringList{jobId});

        Harness atLimit;
        atLimit.limits.maxPackBytes = 100;
        atLimit.renew();
        atLimit.offered(100);
        QCOMPARE(atLimit.link.of<protocol::SnapshotOffer>().size(), 1);
    }

    void aBuildErrorEndsWithItsCode() {
        Harness h;
        h.sender->start({});
        h.work.failBuild(PackOutcome{std::nullopt, false, QStringLiteral("git_unsupported")});
        QCOMPARE(h.events.failed.first().second, QStringLiteral("git_unsupported"));
        Harness other;
        other.sender->start({});
        other.work.failBuild(PackOutcome{std::nullopt, true, {}}); // full pack: base not the cause
        QCOMPARE(other.events.failed.first().second, code::snapshotBuildFailed);
    }

    void haveSendsNothingAndWaitsForReady() {
        Harness h;
        h.offered(3 * chunk);
        h.want(QStringLiteral("have"));
        QCOMPARE(h.sender->phase(), SnapshotSender::Phase::AwaitReady);
        QCOMPARE(h.link.of<protocol::SnapshotChunk>().size(), 0);
        QCOMPARE(h.work.readCount, 0);
        h.ready();
        QCOMPARE(h.events.ready, QStringList{jobId});
    }

    void needSendsFromItsOffset() {
        Harness h;
        h.offered(3 * chunk + 7);
        h.want(QStringLiteral("need"), chunk);
        const auto chunks = h.link.of<protocol::SnapshotChunk>();
        QCOMPARE(chunks.size(), 3);
        QCOMPARE(chunks.first().offset, chunk);
        QCOMPARE(chunks.last().offset, 3 * chunk);
        QCOMPARE(chunks.last().bytes, 7);
    }

    void chunksCoverThePackExactly_data() {
        QTest::addColumn<qint64>("size");
        QTest::newRow("below one chunk") << qint64(100);
        QTest::newRow("one chunk") << chunk;
        QTest::newRow("two chunks exactly") << 2 * chunk;
        QTest::newRow("off a multiple") << 2 * chunk + 5;
    }
    void chunksCoverThePackExactly() {
        QFETCH(qint64, size);
        Harness h;
        h.offered(size);
        h.want(QStringLiteral("need"), 0);
        const auto chunks = h.link.of<protocol::SnapshotChunk>();
        QCOMPARE(chunks.size(), h.link.bulk.size());
        QByteArray whole;
        for (qsizetype i = 0; i < chunks.size(); ++i) {
            const auto& frame = chunks.at(i);
            const auto& [transferId, bytes] = h.link.bulk.at(i);
            QCOMPARE(frame.transferId, transferId);
            QCOMPARE(frame.transferId.size(), 16);
            QCOMPARE(frame.offset, whole.size());
            QCOMPARE(frame.bytes, bytes.size());
            QCOMPARE(frame.sha256, sha256(bytes));
            QVERIFY(frame.bytes == chunk || i == chunks.size() - 1);
            QVERIFY(!protocol::validate(frame, protocol::Direction::DesktopToRunner).has_value());
            whole += bytes;
            h.ack(whole.size());
        }
        QCOMPARE(whole.size(), size);
        QCOMPARE(sha256(whole), sha256(packBytes(0, size, 1)));
        QCOMPARE(h.sender->acked(), size);
    }

    void neverMoreThanFourChunksAreUnacknowledged() {
        Harness h;
        h.offered(10 * chunk + 1);
        h.want(QStringLiteral("need"), 0);
        QCOMPARE(h.link.bulk.size(), 4);
        QCOMPARE(h.inFlight(), 4 * chunk);
        h.ack(chunk);
        QCOMPARE(h.link.bulk.size(), 5);
        h.ack(3 * chunk);
        QCOMPARE(h.link.bulk.size(), 7);
        for (qint64 offset = 4 * chunk; offset <= 10 * chunk; offset += chunk) {
            QVERIFY(h.inFlight() <= 4 * chunk);
            h.ack(offset);
        }
        h.ack(10 * chunk + 1);
        QCOMPARE(h.link.bulk.size(), 11);
        QCOMPARE(h.inFlight(), 0);
    }

    void aBlockedBulkChannelPausesTheTransfer() {
        Harness h;
        h.offered(3 * chunk);
        h.link.writable = false;
        h.want(QStringLiteral("need"), 0);
        QCOMPARE(h.work.readCount, 0);
        h.link.writable = true;
        h.sender->bulkWritable();
        QCOMPARE(h.link.bulk.size(), 3);

        // A chunk read while the channel is blocked waits for it.
        Harness w;
        w.work.autoRead = false;
        w.offered(2 * chunk);
        w.want(QStringLiteral("need"), 0);
        QCOMPARE(w.work.reads.size(), 1u);
        w.link.writable = false;
        w.work.finishReads();
        QCOMPARE(w.link.bulk.size(), 0);
        QCOMPARE(w.link.of<protocol::SnapshotChunk>().size(), 0);
        w.link.writable = true;
        w.sender->bulkWritable();
        QCOMPARE(w.link.bulk.size(), 1);
        w.work.finishReads();
        QCOMPARE(w.link.bulk.size(), 2);
    }

    void untrustedOffsetsEndWithPackInvalid_data() {
        QTest::addColumn<QString>("frame");
        QTest::addColumn<qint64>("offset");
        const qint64 size = 10 * chunk + 9;
        QTest::newRow("want beyond the pack") << "want" << size + 1;
        QTest::newRow("want off a boundary") << "want" << chunk + 1;
        QTest::newRow("want negative") << "want" << qint64(-4);
        QTest::newRow("ack beyond the pack") << "ack" << size + 1;
        QTest::newRow("ack off a boundary") << "ack" << 2 * chunk - 1;
        QTest::newRow("ack backwards") << "ack" << chunk;
        QTest::newRow("ack beyond what was sent") << "ack" << 7 * chunk;
    }
    void untrustedOffsetsEndWithPackInvalid() {
        QFETCH(QString, frame);
        QFETCH(qint64, offset);
        Harness h;
        h.offered(10 * chunk + 9);
        if (frame == QStringLiteral("want")) {
            h.want(QStringLiteral("need"), offset);
        } else {
            h.want(QStringLiteral("need"), 0);
            h.ack(2 * chunk);
            h.ack(offset);
        }
        const auto sent = h.link.bulk.size();
        QCOMPARE(h.events.failed, (QList<std::pair<QString, QString>>{{jobId, code::packInvalid}}));
        h.ack(h.sender->sent());
        h.sender->bulkWritable();
        QCOMPARE(h.link.bulk.size(), sent);
        QCOMPARE(h.sender->phase(), SnapshotSender::Phase::Failed);
    }

    void wantAtTheEndSendsNothing() {
        Harness h;
        h.offered(2 * chunk + 3);
        h.want(QStringLiteral("need"), 2 * chunk + 3);
        QCOMPARE(h.link.bulk.size(), 0);
        h.ready();
        QCOMPARE(h.events.ready.size(), 1);
    }

    void mismatchedOrMisplacedFramesAreDropped() {
        Harness h;
        h.sender->start({});
        h.ready(); // before any offer
        h.want(QStringLiteral("need"));
        h.error(QStringLiteral("disk_full"));
        QCOMPARE(h.sender->phase(), SnapshotSender::Phase::Packing);
        h.work.finishBuild(3 * chunk);
        h.ack(chunk); // no want yet
        h.want(QStringLiteral("need"), 0, snapshotB);
        h.ready(snapshotB);
        h.error(QStringLiteral("disk_full"), snapshotB);
        QCOMPARE(h.sender->phase(), SnapshotSender::Phase::Offered);
        h.want(QStringLiteral("need"), 0);
        const auto sent = h.link.bulk.size();
        h.want(QStringLiteral("need"), chunk); // repeated want
        h.want(QStringLiteral("have"));
        QCOMPARE(h.sender->phase(), SnapshotSender::Phase::Sending);
        QCOMPARE(h.link.bulk.size(), sent);
        QVERIFY(h.events.failed.isEmpty());
        QVERIFY(h.events.ready.isEmpty());
        // A frame of another type changes nothing.
        h.sender->receive(protocol::JobStarted{});
        QCOMPARE(h.sender->phase(), SnapshotSender::Phase::Sending);
    }

    void aRepeatedAckSendsNothingTwice() {
        Harness h;
        h.offered(8 * chunk);
        h.want(QStringLiteral("need"), 0);
        h.ack(chunk);
        const auto sent = h.link.bulk.size();
        h.ack(chunk);
        h.ack(chunk);
        QCOMPARE(h.link.bulk.size(), sent);
        QVERIFY(h.events.failed.isEmpty());
    }

    void chunkCorruptResendsFromTheLastAck() {
        Harness h;
        h.offered(8 * chunk);
        h.want(QStringLiteral("need"), 0);
        h.ack(2 * chunk);
        QCOMPARE(h.link.bulk.size(), 6);
        h.error(QStringLiteral("chunk_corrupt"));
        const auto chunks = h.link.of<protocol::SnapshotChunk>();
        QCOMPARE(chunks.size(), 10);
        QCOMPARE(chunks.at(6).offset, 2 * chunk);
        QCOMPARE(chunks.at(9).offset, 5 * chunk);
        QCOMPARE(h.inFlight(), 4 * chunk);
    }

    void aLateReadFromBeforeChunkCorruptIsIgnored() {
        Harness h;
        h.work.autoRead = false;
        h.offered(4 * chunk);
        h.want(QStringLiteral("need"), 0);
        h.work.finishReads();
        h.work.finishReads();
        QCOMPARE(h.link.bulk.size(), 2);
        h.error(QStringLiteral("chunk_corrupt")); // one read of offset 2 chunks is outstanding
        QCOMPARE(h.work.reads.size(), 2u);
        h.work.finishReads();
        const auto chunks = h.link.of<protocol::SnapshotChunk>();
        QCOMPARE(chunks.size(), 3);
        QCOMPARE(chunks.last().offset, 0);
    }

    void baseMissingOffersAFullPack_data() {
        QTest::addColumn<bool>("allSent");
        QTest::newRow("while sending") << false;
        QTest::newRow("after the whole pack") << true;
    }
    void baseMissingOffersAFullPack() {
        QFETCH(bool, allSent);
        Harness h;
        h.retention.confirmedRefs[{runnerId, workspace}] = snapshotB;
        h.sender->start({{workspace, snapshotB}});
        h.work.finishBuild(2 * chunk);
        h.want(QStringLiteral("need"), 0);
        if (allSent)
            h.ack(2 * chunk);
        h.error(QStringLiteral("base_missing"));
        QCOMPARE(h.work.builds.front().baseId, QString());
        QVERIFY(!h.retention.confirmed(runnerId, workspace).has_value());
        const PackFile full = h.work.finishBuild(5 * chunk, 2);
        const auto offers = h.link.of<protocol::SnapshotOffer>();
        QCOMPARE(offers.size(), 2);
        QVERIFY(!offers.last().baseId.has_value());
        QCOMPARE(offers.last().packSha256, full.sha256);
        QVERIFY(h.events.failed.isEmpty());
        h.error(QStringLiteral("base_missing"));
        QCOMPARE(h.events.failed.first().second, code::packInvalid);
    }

    void runnerErrorsEndWithTheirOwnCode_data() {
        QTest::addColumn<QString>("code");
        for (const auto* value :
             {"snapshot_too_large", "pack_invalid", "disk_full", "checkout_failed", "new_code_9"})
            QTest::newRow(value) << QString::fromLatin1(value);
    }
    void runnerErrorsEndWithTheirOwnCode() {
        QFETCH(QString, code);
        Harness h;
        h.offered(3 * chunk);
        h.want(QStringLiteral("need"), 0);
        h.error(code);
        QCOMPARE(h.events.failed, (QList<std::pair<QString, QString>>{{jobId, code}}));
        QCOMPARE(h.sender->failure(), code);
        QCOMPARE(h.retention.released, QStringList{jobId});
        QVERIFY(!h.retention.pack(jobId).has_value());
        h.ready();
        QVERIFY(h.events.ready.isEmpty());
    }

    void aPackReadErrorEndsWithoutAWrongChunk_data() {
        QTest::addColumn<bool>("shortRead");
        QTest::newRow("read failed") << false;
        QTest::newRow("short read") << true;
    }
    void aPackReadErrorEndsWithoutAWrongChunk() {
        QFETCH(bool, shortRead);
        Harness h;
        h.offered(2 * chunk);
        h.work.failRead = !shortRead;
        h.work.shortRead = shortRead;
        h.want(QStringLiteral("need"), 0);
        QCOMPARE(h.link.bulk.size(), 0);
        QCOMPARE(h.link.of<protocol::SnapshotChunk>().size(), 0);
        QCOMPARE(h.events.failed.first().second, code::packUnreadable);
    }

    void aNewConnectionOffersTheSamePackAgain() {
        Harness h;
        const PackFile pack = h.offered(6 * chunk);
        h.want(QStringLiteral("need"), 0);
        h.ack(2 * chunk);
        h.sender->disconnected();
        QCOMPARE(h.sender->phase(), SnapshotSender::Phase::Detached);
        h.ack(3 * chunk); // the old connection is gone
        h.sender->reconnected({});
        QCOMPARE(h.sender->phase(), SnapshotSender::Phase::Verifying);
        QCOMPARE(h.link.of<protocol::SnapshotOffer>().size(), 1);
        h.work.finishHash();
        QCOMPARE(h.work.hashCount, 1);
        QCOMPARE(h.work.buildCount, 1);
        const auto offers = h.link.of<protocol::SnapshotOffer>();
        QCOMPARE(offers.size(), 2);
        QCOMPARE(offers.last().packSha256, pack.sha256);
        const auto before = h.link.bulk.size();
        h.want(QStringLiteral("need"), 2 * chunk);
        QCOMPARE(h.link.of<protocol::SnapshotChunk>().at(before).offset, 2 * chunk);
    }

    void aNewConnectionBuildsANewPackWhenItMust_data() {
        QTest::addColumn<bool>("evicted");
        QTest::addColumn<bool>("baseGone");
        QTest::newRow("pack evicted") << true << false;
        QTest::newRow("base no longer listed") << false << true;
    }
    void aNewConnectionBuildsANewPackWhenItMust() {
        QFETCH(bool, evicted);
        QFETCH(bool, baseGone);
        Harness h;
        h.retention.confirmedRefs[{runnerId, workspace}] = snapshotB;
        h.sender->start({{workspace, snapshotB}});
        h.work.finishBuild(3 * chunk);
        h.want(QStringLiteral("need"), 0);
        h.sender->disconnected();
        if (evicted)
            h.retention.packs.clear();
        h.sender->reconnected(baseGone ? QMap<QString, QString>{}
                                       : QMap<QString, QString>{{workspace, snapshotB}});
        QCOMPARE(h.work.buildCount, 2);
        QCOMPARE(h.work.builds.front().baseId, baseGone ? QString() : snapshotB);
        const PackFile fresh = h.work.finishBuild(4 * chunk, 3);
        QCOMPARE(h.link.of<protocol::SnapshotOffer>().last().packSha256, fresh.sha256);
    }

    void aHelloDuringPackingRechecksTheBase() {
        Harness h;
        h.retention.confirmedRefs[{runnerId, workspace}] = snapshotB;
        h.sender->start({{workspace, snapshotB}});
        h.sender->disconnected();
        h.sender->reconnected({});
        h.work.finishBuild(3 * chunk);
        QCOMPARE(h.link.of<protocol::SnapshotOffer>().size(), 0);
        QCOMPARE(h.work.builds.front().baseId, QString());
        h.work.finishBuild(3 * chunk, 4);
        QVERIFY(!h.link.of<protocol::SnapshotOffer>().first().baseId.has_value());
    }

    void readyConfirmsBeforeReportingAndBecomesTheNextBase() {
        Harness h;
        h.offered(chunk);
        h.want(QStringLiteral("need"), 0);
        h.ack(chunk);
        h.ready();
        // Nothing usable was kept, so whatever the cache held for the job went before the build.
        QCOMPARE(h.retention.log, (QStringList{QStringLiteral("drop"), QStringLiteral("confirm"),
                                               QStringLiteral("release")}));
        QCOMPARE(h.events.ready, QStringList{jobId});
        QCOMPARE(h.retention.confirmed(runnerId, workspace), std::optional<QString>(snapshotA));
        h.ready();
        QCOMPARE(h.events.ready.size(), 1);

        // The next job on the workspace sends an incremental pack.
        h.renew(snapshotB);
        h.sender->start({{workspace, snapshotA}});
        QCOMPARE(h.work.builds.front().baseId, snapshotA);
        h.work.finishBuild(10, 5);
        QCOMPARE(h.link.of<protocol::SnapshotOffer>().last().baseId,
                 std::optional<QString>(snapshotA));
    }

    void aConfirmationThatIsNotStoredFails() {
        Harness h;
        h.retention.failConfirm = true;
        h.offered(10);
        h.want(QStringLiteral("have"));
        h.ready();
        QVERIFY(h.events.ready.isEmpty());
        QCOMPARE(h.events.failed.first().second, code::storageError);
    }

    void cancelStopsAtEveryStep_data() {
        QTest::addColumn<int>("step");
        QTest::newRow("packing") << 0;
        QTest::newRow("sending") << 1;
        QTest::newRow("awaiting ready") << 2;
        QTest::newRow("detached") << 3;
    }
    void cancelStopsAtEveryStep() {
        QFETCH(int, step);
        Harness h;
        h.work.autoRead = false;
        h.sender->start({});
        if (step >= 1) {
            h.work.finishBuild(3 * chunk);
            h.want(step == 2 ? QStringLiteral("have") : QStringLiteral("need"), 0);
        }
        if (step == 3)
            h.sender->disconnected();
        const auto frames = h.link.frames.size();
        h.sender->cancel();
        QCOMPARE(h.sender->phase(), SnapshotSender::Phase::Cancelled);
        QCOMPARE(h.retention.released, QStringList{jobId});
        QVERIFY(!h.retention.pack(jobId).has_value());
        if (!h.work.builds.empty())
            h.work.finishBuild(3 * chunk);
        h.work.finishReads();
        h.ack(chunk);
        h.ready();
        h.sender->reconnected({});
        QCOMPARE(h.link.frames.size(), frames);
        QVERIFY(h.events.ready.isEmpty());
        QVERIFY(h.events.failed.isEmpty());
        h.sender->cancel();
        QCOMPARE(h.retention.released.size(), 1);
    }

    // ---- A kept pack ----

    void aKeptPackIsOfferedOnlyAfterItsHashMatches() {
        Harness h;
        const PackFile kept = keptPack(h, {});
        h.sender->start({});
        QCOMPARE(h.sender->phase(), SnapshotSender::Phase::Verifying);
        QCOMPARE(h.work.hashes.front().pack.path, kept.path);
        // Nothing is offered yet, and the runner's snapshot frames have nothing to act on.
        h.want(QStringLiteral("need"), 0);
        h.ack(chunk);
        h.ready();
        h.error(QStringLiteral("base_missing"));
        h.sender->disconnected();
        QVERIFY(h.link.frames.isEmpty());
        QVERIFY(h.events.failed.isEmpty() && h.events.ready.isEmpty());
        h.work.finishHash();
        QCOMPARE(h.work.hashCount, 1);
        QCOMPARE(h.work.buildCount, 0);
        QCOMPARE(h.link.of<protocol::SnapshotOffer>().size(), 1);
        QCOMPARE(h.link.of<protocol::SnapshotOffer>().first().packSha256, kept.sha256);
    }

    void aKeptPackThatChangedIsDroppedAndBuiltAgain_data() {
        QTest::addColumn<QString>("hash");
        QTest::newRow("other content") << sha256(QByteArray("changed"));
        QTest::newRow("unreadable") << QString();
    }
    void aKeptPackThatChangedIsDroppedAndBuiltAgain() {
        QFETCH(QString, hash);
        Harness h;
        const PackFile kept = keptPack(h, {});
        h.sender->start({});
        h.work.finishHash(hash);
        QCOMPARE(h.retention.log, QStringList{QStringLiteral("drop")});
        QVERIFY(h.link.frames.isEmpty());
        QCOMPARE(h.work.buildCount, 1);
        const PackFile fresh = h.work.finishBuild(2 * chunk, 7);
        QVERIFY(fresh.sha256 != kept.sha256);
        QCOMPARE(h.link.of<protocol::SnapshotOffer>().first().packSha256, fresh.sha256);
    }

    // An incremental kept pack passes the same base check as a new pack: the confirmed ref and
    // the current hello.
    void aKeptIncrementalPackNeedsTheBaseANewPackWouldUse_data() {
        QTest::addColumn<QString>("confirmed");
        QTest::addColumn<QString>("listed");
        QTest::addColumn<bool>("reused");
        QTest::newRow("confirmed and listed") << snapshotB << snapshotB << true;
        QTest::newRow("listed, not confirmed") << QString() << snapshotB << false;
        QTest::newRow("confirmed another") << snapshotC << snapshotB << false;
        QTest::newRow("confirmed, not listed") << snapshotB << QString() << false;
    }
    void aKeptIncrementalPackNeedsTheBaseANewPackWouldUse() {
        QFETCH(QString, confirmed);
        QFETCH(QString, listed);
        QFETCH(bool, reused);
        Harness h;
        if (!confirmed.isEmpty())
            h.retention.confirmedRefs[{runnerId, workspace}] = confirmed;
        keptPack(h, snapshotB);
        const QMap<QString, QString> hello = listed.isEmpty()
                                                 ? QMap<QString, QString>{}
                                                 : QMap<QString, QString>{{workspace, listed}};
        h.sender->start(hello);
        if (reused) {
            QCOMPARE(h.work.hashCount, 1);
            h.work.finishHash();
            QCOMPARE(h.link.of<protocol::SnapshotOffer>().first().baseId, snapshotB);
            return;
        }
        QCOMPARE(h.work.hashCount, 0);
        QCOMPARE(h.retention.log, QStringList{QStringLiteral("drop")});
        QCOMPARE(h.work.buildCount, 1);
        const QString expected = confirmed == listed ? confirmed : QString();
        QCOMPARE(h.work.builds.front().baseId, expected);
    }

    void aHelloWhileHashingRechecksTheBase() {
        Harness h;
        h.retention.confirmedRefs[{runnerId, workspace}] = snapshotB;
        keptPack(h, snapshotB);
        h.sender->start({{workspace, snapshotB}});
        h.sender->reconnected({});
        QCOMPARE(h.work.hashCount, 1);
        h.work.finishHash();
        QVERIFY(h.link.frames.isEmpty());
        QCOMPARE(h.work.builds.front().baseId, QString());
    }

    void cancelWhileHashingIgnoresTheAnswer() {
        Harness h;
        keptPack(h, {});
        h.sender->start({});
        h.sender->cancel();
        QCOMPARE(h.retention.released, QStringList{jobId});
        QVERIFY(!h.retention.pack(jobId));
        h.work.finishHash();
        QVERIFY(h.link.frames.isEmpty());
        QCOMPARE(h.work.buildCount, 0);
        QVERIFY(h.events.failed.isEmpty());
    }

    void aLateAnswerAfterTheSenderIsGoneIsHarmless() {
        Harness h;
        h.work.autoRead = false;
        h.sender->start({});
        h.sender.reset();
        h.work.finishBuild(10);
        QVERIFY(h.link.frames.isEmpty());
    }
};

QTEST_GUILESS_MAIN(SnapshotSenderTest)
#include "SnapshotSenderTest.moc"
