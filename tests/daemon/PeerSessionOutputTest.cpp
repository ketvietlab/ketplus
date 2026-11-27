#include "PeerSessionFakes.h"

#include "protocol/FrameLimits.h"
#include "runner/peer/RemotePeerSessions.h"

#include <QTest>

using namespace peer_session_test;

namespace {

QStringList messages;

void collect(QtMsgType, const QMessageLogContext&, const QString& message) {
    messages.append(message);
}

QByteArray page(const char fill, const qsizetype bytes = protocol::limits::maxOutputBytes) {
    return QByteArray(bytes, fill);
}

protocol::JobResult cancelled() {
    protocol::JobResult result;
    result.jobId = jobId;
    result.attempt = 1;
    result.state = QStringLiteral("cancelled");
    result.reason = QStringLiteral("user");
    result.cleanupStatus = QStringLiteral("verified");
    return result;
}

} // namespace

class PeerSessionOutputTest : public QObject {
    Q_OBJECT

  private slots:
    void fullPagesGoToTheSpoolAndAreAcknowledged() {
        Host host;
        host.start();
        for (int i = 0; i < 8; ++i)
            host.deliver(
                protocol::JobOutput{jobId, i * protocol::limits::maxOutputBytes, page('a' + i)});
        QCOMPARE(host.spool.size(), 8 * protocol::limits::maxOutputBytes);
        QCOMPARE(host.sentOf<protocol::JobOutputAck>(),
                 (QList<protocol::JobOutputAck>{{1, jobId, 256 * 1024}}));
        host.deliver(protocol::JobOutput{jobId, 8 * protocol::limits::maxOutputBytes, "tail"});
        QVERIFY(host.clock.armed(PeerTimer::OutputAck));
        host.advance(999);
        QCOMPARE(host.sentOf<protocol::JobOutputAck>().size(), 1);
        host.advance(1);
        QCOMPARE(host.sentOf<protocol::JobOutputAck>().last().offset, 256 * 1024 + 4);
        // A repeat of what the spool holds is not written twice.
        host.deliver(protocol::JobOutput{jobId, 0, page('a')});
        QCOMPARE(host.spool.size(), 256 * 1024 + 4);
    }

    void aJumpAsksOnceToResumeFromTheSpool() {
        Host host;
        host.start();
        host.deliver(protocol::JobOutput{jobId, 0, "abc"});
        host.deliver(protocol::JobOutput{jobId, 10, "late"});
        host.deliver(protocol::JobOutput{jobId, 14, "later"});
        QCOMPARE(host.sentOf<protocol::JobOutputResume>(),
                 (QList<protocol::JobOutputResume>{{1, jobId, 3}}));
        QCOMPARE(host.spool.size(), 3);
    }

    void gapsAreRecordedAndOutputGoesOnAfterThem() {
        Host host;
        host.start();
        host.deliver(protocol::JobOutput{jobId, 0, "abc"});
        host.deliver(protocol::JobOutputGap{jobId, 3, 100});
        host.deliver(protocol::JobOutputGap{jobId, 50, 120}); // overlaps: [100, 120) is new
        host.deliver(protocol::JobOutputGap{jobId, 10, 90});  // already behind: dropped
        host.deliver(protocol::JobOutput{jobId, 120, "def"});
        QCOMPARE(host.spool.gaps, (QList<std::pair<qint64, qint64>>{{3, 100}, {100, 120}}));
        QCOMPARE(host.spool.size(), 123);
        host.deliver(protocol::JobOutputGap{jobId, 200, 300}); // beyond the spool
        QCOMPARE(host.sentOf<protocol::JobOutputResume>().last().fromOffset, 123);
        host.advance(1'000);
        QCOMPARE(host.sentOf<protocol::JobOutputAck>().last().offset, 123);
    }

    void aFailingSpoolKeepsTheConnectionAndTheResult() {
        messages.clear();
        const QtMessageHandler previous = qInstallMessageHandler(collect);
        Host host;
        host.start();
        host.deliver(protocol::JobOutput{jobId, 0, "kept"});
        host.spool.failing = true;
        host.deliver(protocol::JobOutput{jobId, 4, "secret-output"});
        host.deliver(protocol::JobOutput{jobId, 17, "more"});
        QVERIFY(host.links.closes.isEmpty());
        host.advance(1'000);
        QVERIFY(host.sentOf<protocol::JobOutputAck>().isEmpty()); // no ack while it fails
        QCOMPARE(host.spool.size(), 4);
        // Every 5 seconds the runner is asked again from the spool.
        host.advance(4'000);
        QCOMPARE(host.sentOf<protocol::JobOutputResume>(),
                 (QList<protocol::JobOutputResume>{{1, jobId, 4}}));
        host.deliver(protocol::JobOutput{jobId, 4, "secret-output"});
        host.advance(5'000);
        QCOMPARE(host.sentOf<protocol::JobOutputResume>().size(), 2);
        host.spool.failing = false;
        host.deliver(protocol::JobOutput{jobId, 4, "secret-output"});
        QCOMPARE(host.spool.size(), 17);
        host.advance(1'000);
        QCOMPARE(host.sentOf<protocol::JobOutputAck>().last().offset, 17);
        // The result never waits for the spool.
        host.spool.failing = true;
        host.deliver(protocol::JobOutput{jobId, 17, "x"});
        host.deliver(cancelled());
        QCOMPARE(host.sentOf<protocol::JobResultAck>().size(), 1);
        qInstallMessageHandler(previous);
        // Reported once per failure, with ids and numbers only.
        QCOMPARE(messages.filter("spool").size(), 2);
        for (const QString& message : std::as_const(messages)) {
            QVERIFY(!message.contains("secret"));
            QVERIFY(!message.contains("make"));
        }
    }

    void logsCarryNoSpecOrOutput() {
        messages.clear();
        const QtMessageHandler previous = qInstallMessageHandler(collect);
        Host host;
        host.connect();
        host.deliver(protocol::JobAccepted{jobId, 1});
        host.deliver(protocol::JobOutput{jobId, 0, "private-bytes"});
        host.deliverRaw(protocol::Channel::Events, "private garbage");
        host.records.dispatchCode = "not_paired";
        Host refused;
        refused.records.dispatchCode = "not_paired";
        refused.connect();
        qInstallMessageHandler(previous);
        QVERIFY(!messages.isEmpty());
        for (const QString& message : std::as_const(messages)) {
            QVERIFY(!message.contains("private"));
            QVERIFY(!message.contains("make"));
            QVERIFY(!message.contains("src"));
        }
    }

    // ---- Sessions of many jobs ----

    void sessionsAreMadePerJobAndReachedByConnection() {
        Host host; // its ports; the sessions below are made on them
        RemotePeerSessions sessions(host.records, [&host](const QString& id) {
            return std::make_unique<JobPeerSession>(id, daemonVersion,
                                                    SessionPorts{host.links, host.clock,
                                                                 host.records, host.specs,
                                                                 host.snapshots, host.spool});
        });
        QVERIFY(sessions.offered("c1", runnerId, jobId, 1).ok());
        QCOMPARE(sessions.size(), 1);
        QCOMPARE(sessions.offered("c1", runnerId, jobId, 1).code, QStringLiteral("job_mismatch"));
        sessions.channelsOpened("c1");
        sessions.frameReceived("c1", protocol::Channel::Control, encodeFromRunner(makeHello("c1")));
        QCOMPARE(host.sentTypes(), (QStringList{"welcome", "job.spec"}));
        sessions.frameReceived("c1", protocol::Channel::Control,
                               encodeFromRunner(protocol::JobAccepted{jobId, 1}));
        // The record's Stop reaches the session by task.
        sessions.cancel("task-1", runnerId);
        QCOMPARE(host.sentTypes().last(), QStringLiteral("job.cancel"));
        sessions.cancel("task-9", runnerId);
        QCOMPARE(host.sentOf<protocol::JobCancel>().size(), 1);
        sessions.connectionLost("c1");
        QVERIFY(host.records.calls.contains("peerLost runner-1"));
        sessions.release(jobId);
        QCOMPARE(sessions.size(), 0);
        // No record, no session.
        host.records.exists = false;
        QCOMPARE(sessions.offered("c2", runnerId, "job-9", 1).code, QStringLiteral("job_mismatch"));
        QCOMPARE(sessions.size(), 0);
    }

    void releasingASessionClosesItsConnections() {
        Host host;
        RemotePeerSessions sessions(host.records, [&host](const QString& id) {
            return std::make_unique<JobPeerSession>(id, daemonVersion,
                                                    SessionPorts{host.links, host.clock,
                                                                 host.records, host.specs,
                                                                 host.snapshots, host.spool});
        });
        QVERIFY(sessions.offered("c1", runnerId, jobId, 1).ok());
        sessions.channelsOpened("c1");
        sessions.frameReceived("c1", protocol::Channel::Control, encodeFromRunner(makeHello("c1")));
        QVERIFY(sessions.offered("c2", runnerId, jobId, 1).ok());
        sessions.release(jobId);
        QCOMPARE(host.links.closes, (QStringList{"c2 session_closed", "c1 session_closed"}));
        QVERIFY(!host.records.calls.contains("peerLost runner-1"));
        QVERIFY(host.clock.deadlines.isEmpty());
    }
};

QTEST_GUILESS_MAIN(PeerSessionOutputTest)
#include "PeerSessionOutputTest.moc"
