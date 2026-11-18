#include "PeerSessionFakes.h"

#include <QTest>

using namespace peer_session_test;
using protocol::Channel;

namespace {

protocol::JobResult exited(const qint64 code = 0) {
    protocol::JobResult result;
    result.jobId = jobId;
    result.attempt = 1;
    result.state = QStringLiteral("exited");
    result.exitCode = code;
    result.cleanupStatus = QStringLiteral("verified");
    result.outputBytes = 0;
    return result;
}

protocol::SnapshotError snapshotError(const QString& code) {
    return protocol::SnapshotError{QString(40, 'a'), code};
}

} // namespace

class PeerSessionJobTest : public QObject {
    Q_OBJECT

  private slots:
    // ---- Spec and resume ----

    void aRefusedDispatchClosesWithoutASpec_data() {
        QTest::addColumn<QString>("code");
        QTest::newRow("unpaired") << "not_paired";
        QTest::newRow("dispatched") << "already_dispatched";
        QTest::newRow("store") << "storage_error";
    }
    void aRefusedDispatchClosesWithoutASpec() {
        QFETCH(QString, code);
        Host host;
        host.records.dispatchCode = code;
        host.connect();
        QCOMPARE(host.sentTypes(), QStringList{"welcome"});
        QCOMPARE(host.links.closes, QStringList{"c1 " + code});
        QCOMPARE(host.specs.asked, 1); // the spec is read before the dispatch
    }

    void noSpecNeverDispatches() {
        Host host;
        host.specs.available = false;
        host.connect();
        QCOMPARE(host.sentTypes(), QStringList{"welcome"});
        QCOMPARE(host.links.closes, QStringList{"c1 spec_unavailable"});
        QVERIFY(!host.records.calls.join(' ').contains("dispatch"));
        QVERIFY(host.records.record.dispatchedTo.isEmpty());
    }

    void theMarkedRunnerGetsTheSameSpecUntilItAccepts() {
        Host host;
        host.connect("c1");
        const protocol::JobSpec first = host.sentOf<protocol::JobSpec>().first();
        host.session->connectionLost("c1");
        host.connect("c2");
        QCOMPARE(host.sentTypes().mid(host.sentTypes().size() - 2),
                 (QStringList{"welcome", "job.spec"}));
        protocol::JobSpec again = host.sentOf<protocol::JobSpec>().last();
        QCOMPARE(again.epoch, 2);
        again.epoch = first.epoch;
        QCOMPARE(again, first);
        QCOMPARE(host.records.calls.count("dispatch runner-1"), 1);
        host.deliver(protocol::JobAccepted{jobId, 1}, "c2");
        QCOMPARE(host.records.calls.count("accepted runner-1"), 1);
        // After `job.accepted` only `job.resume`.
        host.session->connectionLost("c2");
        host.connect("c3");
        QCOMPARE(host.sentOf<protocol::JobSpec>().size(), 2);
        QCOMPARE(host.sentTypes().last(), QStringLiteral("job.resume"));
        QCOMPARE(host.records.calls.count("dispatch runner-1"), 1);
    }

    void theAcceptedRunnerGetsAResume() {
        Host host;
        host.records.record.dispatchedTo = runnerId; // a session rebuilt from the record
        host.records.record.accepted = true;
        QVERIFY(host.spool.append("0123456789"));
        host.connect();
        QCOMPARE(host.sentTypes(), (QStringList{"welcome", "job.resume"}));
        QCOMPARE(host.sentOf<protocol::JobResume>().first(), (protocol::JobResume{1, jobId, 10}));
        QCOMPARE(host.specs.asked, 0);
        QVERIFY(!host.records.calls.join(' ').contains("dispatch"));
        QCOMPARE(host.session->phase(), JobPeerSession::Phase::Syncing);
    }

    void aSecondRunnerNeverGetsTheSpec() {
        Host host;
        host.connect("c1");
        QCOMPARE(host.session->offered("c2", "runner-2", 1).code,
                 QStringLiteral("already_dispatched"));
        // Even when the record lets an offer through, the spec stays with the marked runner.
        Host other;
        other.records.record.dispatchedTo = runnerId;
        other.records.allowAnyOffer = true;
        other.connect("c5", "runner-2");
        QCOMPARE(other.sentTypes(), QStringList{"welcome"});
        QCOMPARE(other.links.closes, QStringList{"c5 already_dispatched"});
        QCOMPARE(other.specs.asked, 0);
    }

    void aRebuiltSessionFollowsTheStoredRecord() {
        Host host;
        host.connect();
        Host beforeAccept;
        beforeAccept.records.record = host.records.record; // as stored
        beforeAccept.connect();
        QCOMPARE(beforeAccept.sentOf<protocol::JobSpec>().size(), 1);
        QVERIFY(!beforeAccept.records.calls.join(' ').contains("dispatch"));
        host.deliver(protocol::JobAccepted{jobId, 1});
        Host afterAccept;
        afterAccept.records.record = host.records.record;
        afterAccept.connect();
        QCOMPARE(afterAccept.sentOf<protocol::JobSpec>().size(), 0);
        QCOMPARE(afterAccept.sentOf<protocol::JobResume>().size(), 1);
    }

    // ---- The runner's answers ----

    void runnerAnswersReachTheRecordOnce_data() {
        QTest::addColumn<QString>("frame");
        QTest::addColumn<QString>("job");
        QTest::addColumn<int>("attempt");
        QTest::addColumn<bool>("counted");
        QTest::newRow("accepted") << "accepted" << jobId << 1 << true;
        QTest::newRow("accepted other job") << "accepted" << "job-9" << 1 << false;
        QTest::newRow("accepted other attempt") << "accepted" << jobId << 2 << false;
        QTest::newRow("rejected") << "rejected" << jobId << 1 << true;
        QTest::newRow("rejected other job") << "rejected" << "job-9" << 1 << false;
        QTest::newRow("rejected other attempt") << "rejected" << jobId << 2 << false;
        QTest::newRow("started") << "started" << jobId << 1 << true;
        QTest::newRow("started other job") << "started" << "job-9" << 1 << false;
    }
    void runnerAnswersReachTheRecordOnce() {
        QFETCH(QString, frame);
        QFETCH(QString, job);
        QFETCH(int, attempt);
        QFETCH(bool, counted);
        Host host;
        host.connect();
        host.records.calls.clear();
        for (int i = 0; i < 2; ++i) {
            if (frame == "accepted")
                host.deliver(protocol::JobAccepted{job, attempt});
            else if (frame == "rejected")
                host.deliver(protocol::JobRejected{job, attempt, "runner_busy"});
            else
                host.deliver(protocol::JobStarted{job, "2026-11-18T09:00:00Z"});
        }
        const QString expected =
            frame == "rejected" ? "rejected runner-1 runner_busy" : frame + " runner-1";
        QCOMPARE(host.records.calls.count(expected), counted ? 1 : 0);
        QCOMPARE(host.records.calls.size(), counted ? 1 : 0);
    }

    void answersInTheWrongStateAreDropped() {
        Host host;
        host.start();
        host.records.calls.clear();
        host.deliver(protocol::JobAccepted{jobId, 1});
        host.deliver(protocol::JobRejected{jobId, 1, "spec_invalid"});
        host.deliver(protocol::JobStarted{jobId, "2026-11-18T09:00:00Z"});
        QVERIFY(host.records.calls.isEmpty());
        Host waiting;
        waiting.records.dispatchCode = "storage_error";
        waiting.open();
        waiting.deliver(makeHello("c1"));
        waiting.deliver(protocol::JobAccepted{jobId, 1});
        QVERIFY(!waiting.records.calls.join(' ').contains("accepted"));
    }

    // ---- Snapshot ----

    void theSnapshotRunsFromAcceptedOnTheCurrentConnection() {
        Host host;
        host.accept();
        QCOMPARE(host.snapshots.starts.size(), 1);
        QCOMPARE(host.snapshots.starts.first().second, makeHello("c1").snapshots);
        protocol::SnapshotOffer offer;
        offer.jobId = jobId;
        offer.snapshotId = QString(40, 'a');
        offer.packBytes = 10;
        offer.packSha256 = QString(64, 'f');
        offer.chunkBytes = 4'194'304;
        host.snapshots.send(offer);
        host.snapshots.bulk("0000000000000001", "bytes");
        QCOMPARE(host.sentOf<protocol::SnapshotOffer>().last().epoch, 1);
        QCOMPARE(host.links.sent.last().channel, Channel::Control);
        QCOMPARE(host.links.bulk.size(), 1);
        QCOMPARE(std::get<0>(host.links.bulk.first()), QStringLiteral("c1"));
        host.deliver(protocol::SnapshotWant{QString(40, 'a'), "need", 0});
        host.deliver(protocol::SnapshotAck{QString(40, 'a'), 0});
        QCOMPARE(host.snapshots.received.size(), 2);
        host.session->bulkWritable("c1");
        QCOMPARE(host.snapshots.writable, 1);

        // A new connection: the port starts again with the new `hello`.
        protocol::Hello hello = makeHello("c2");
        hello.snapshots.clear();
        host.open("c2");
        host.deliver(hello, "c2");
        QCOMPARE(host.snapshots.cancels, 1);
        const qsizetype sent = host.links.sent.size();
        host.snapshots.send(offer); // a late send of the cancelled run goes nowhere
        QCOMPARE(host.links.sent.size(), sent);
        host.deliver(protocol::JobAccepted{jobId, 1}, "c2");
        QCOMPARE(host.snapshots.starts.size(), 2);
        QVERIFY(host.snapshots.starts.last().second.isEmpty());
        QCOMPARE(host.records.calls.count("accepted runner-1"), 1);
    }

    void everyEndCancelsTheSnapshot_data() {
        QTest::addColumn<QString>("end");
        QTest::newRow("lost") << "lost";
        QTest::newRow("stop") << "stop";
        QTest::newRow("rejected") << "rejected";
        QTest::newRow("result") << "result";
        QTest::newRow("runner error") << "error";
    }
    void everyEndCancelsTheSnapshot() {
        QFETCH(QString, end);
        Host host;
        host.accept();
        if (end == "lost")
            host.session->connectionLost("c1");
        else if (end == "stop")
            host.session->cancel(runnerId);
        else if (end == "rejected")
            host.deliver(protocol::JobRejected{jobId, 1, "lease_lost"});
        else if (end == "result")
            host.deliver(exited());
        else
            host.deliver(snapshotError("disk_full"));
        QCOMPARE(host.snapshots.cancels, 1);
        const qsizetype received = host.snapshots.received.size();
        host.deliver(protocol::SnapshotReady{QString(40, 'a')});
        QCOMPARE(host.snapshots.received.size(), received);
        // A late failure of the cancelled run changes nothing.
        host.session->snapshotFailed("snapshot_failed");
        QVERIFY(!host.records.calls.join(' ').contains("snapshotFailed"));
        QCOMPARE(host.sentOf<protocol::JobCancel>().size(), end == "stop" ? 1 : 0);
    }

    void recoverableRunnerErrorsKeepTheSnapshotGoing() {
        Host host;
        host.accept();
        host.deliver(snapshotError("chunk_corrupt"));
        host.deliver(snapshotError("base_missing"));
        QCOMPARE(host.snapshots.received.size(), 2);
        QCOMPARE(host.snapshots.cancels, 0);
        QVERIFY(host.sentOf<protocol::JobCancel>().isEmpty());
    }

    void aDesktopSnapshotFailureStopsTheJobOnTheRunner() {
        Host host;
        host.accept();
        host.session->snapshotFailed("snapshot_too_large");
        QVERIFY(host.records.calls.contains("snapshotFailed runner-1 snapshot_too_large"));
        QCOMPARE(host.sentOf<protocol::JobCancel>(),
                 (QList<protocol::JobCancel>{{1, jobId, "session_closed"}}));
        QCOMPARE(host.snapshots.cancels, 0); // the port ended itself
        // Each new connection gets the cancel once more, after `job.resume`.
        host.session->connectionLost("c1");
        host.connect("c2");
        QCOMPARE(host.sentTypes().mid(host.sentTypes().size() - 3),
                 (QStringList{"welcome", "job.resume", "job.cancel"}));
        QCOMPARE(host.sentOf<protocol::JobCancel>().last(),
                 (protocol::JobCancel{2, jobId, "session_closed"}));
        host.deliver(protocol::JobAccepted{jobId, 1}, "c2");
        QCOMPARE(host.snapshots.starts.size(), 1); // no snapshot for a stopping job
    }

    void aSnapshotFailureTheRecordCannotStoreIsTriedAgain() {
        Host host;
        host.accept();
        host.records.snapshotCodes = {"storage_error"};
        host.session->snapshotFailed("snapshot_failed");
        QVERIFY(host.sentOf<protocol::JobCancel>().isEmpty());
        QVERIFY(host.clock.armed(PeerTimer::Retry));
        host.advance(1'000);
        QCOMPARE(host.records.calls.count("snapshotFailed runner-1 snapshot_failed"), 2);
        QCOMPARE(host.sentOf<protocol::JobCancel>().size(), 1);
    }

    void aSnapshotFailureTheRecordDoesNotTakeSendsNothing() {
        Host host;
        host.accept();
        host.records.snapshotCodes = {"not_applicable"};
        host.session->snapshotFailed("snapshot_failed");
        QVERIFY(host.sentOf<protocol::JobCancel>().isEmpty());
        QVERIFY(!host.clock.armed(PeerTimer::Retry));
    }

    // ---- Result ----

    void theResultIsAcknowledgedOnceDurable() {
        Host host;
        host.start();
        host.records.resultCodes = {"storage_error", "storage_error"};
        host.deliver(exited(3));
        QVERIFY(host.sentOf<protocol::JobResultAck>().isEmpty());
        QVERIFY(host.clock.armed(PeerTimer::Retry));
        QVERIFY(host.links.closes.isEmpty());
        host.advance(1'000);
        QVERIFY(host.sentOf<protocol::JobResultAck>().isEmpty());
        host.advance(1'999);
        QVERIFY(host.sentOf<protocol::JobResultAck>().isEmpty());
        host.advance(1);
        QCOMPARE(host.sentOf<protocol::JobResultAck>(),
                 (QList<protocol::JobResultAck>{{1, jobId}}));
        QCOMPARE(host.records.results.size(), 3);
        QCOMPARE(host.session->phase(), JobPeerSession::Phase::Finished);
        // A repeat (the ack was lost) is acknowledged again.
        host.deliver(exited(3));
        QCOMPARE(host.sentOf<protocol::JobResultAck>().size(), 2);
        // Output after the result changes nothing.
        host.deliver(protocol::JobOutput{jobId, 0, "late"});
        QCOMPARE(host.spool.size(), 0);
    }

    void aResultIsStoredEvenWithoutAConnection() {
        Host host;
        host.start();
        host.records.resultCodes = {"storage_error"};
        host.deliver(exited());
        host.session->connectionLost("c1");
        host.advance(1'000);
        QCOMPARE(host.records.results.size(), 2);
        QVERIFY(host.sentOf<protocol::JobResultAck>().isEmpty()); // nowhere to send it
        QVERIFY(!host.clock.armed(PeerTimer::Retry));
    }

    void aResultOfAnotherAttemptIsDropped() {
        Host host;
        host.start();
        protocol::JobResult result = exited();
        result.attempt = 2;
        host.deliver(result);
        result = exited();
        result.jobId = "job-9";
        host.deliver(result);
        QVERIFY(host.records.results.isEmpty());
        QVERIFY(host.sentOf<protocol::JobResultAck>().isEmpty());
    }

    // ---- Cancel ----

    void stopSendsOneCancelPerConnection() {
        Host host;
        host.start();
        host.records.record.stopReason = "user";
        host.session->cancel(runnerId);
        host.session->cancel(runnerId);
        host.session->cancel("runner-2");
        QCOMPARE(host.sentOf<protocol::JobCancel>(),
                 (QList<protocol::JobCancel>{{1, jobId, "user"}}));
        host.session->connectionLost("c1");
        host.session->cancel(runnerId); // no connection: the record remembers the Stop
        QCOMPARE(host.sentOf<protocol::JobCancel>().size(), 1);
        host.connect("c2");
        QCOMPARE(host.sentTypes().mid(host.sentTypes().size() - 3),
                 (QStringList{"welcome", "job.resume", "job.cancel"}));
        QCOMPARE(host.sentOf<protocol::JobCancel>().last(),
                 (protocol::JobCancel{2, jobId, "user"}));
        host.session->cancel(runnerId);
        QCOMPARE(host.sentOf<protocol::JobCancel>().size(), 2);
    }

    void aStopBeforeAcceptedFollowsTheSpecAgain() {
        Host host;
        host.connect("c1");
        host.records.record.stopReason = "user";
        host.session->cancel(runnerId);
        host.session->connectionLost("c1");
        host.connect("c2");
        QCOMPARE(host.sentTypes().mid(host.sentTypes().size() - 3),
                 (QStringList{"welcome", "job.spec", "job.cancel"}));
    }

    void aDeadlineIsNeverSentAsACancel() {
        Host host;
        host.records.record.dispatchedTo = runnerId;
        host.records.record.accepted = true;
        host.records.record.started = true;
        host.records.record.stopReason = "deadline";
        host.connect();
        QVERIFY(host.sentOf<protocol::JobCancel>().isEmpty());
        QCOMPARE(host.session->phase(), JobPeerSession::Phase::Running);
    }
};

QTEST_GUILESS_MAIN(PeerSessionJobTest)
#include "PeerSessionJobTest.moc"
