#include "PeerSessionFakes.h"

#include "session/JobSession.h"

#include <QTest>

// The desktop's job peer session against the runner's real job session, over an in-memory pair
// of channels that delivers frames in order and only when the test pumps it.
using namespace peer_session_test;
namespace runner = ketplus::runner::session;

namespace {

const QString machineId = QStringLiteral("machine-1");

struct Wire;

// The desktop's end: frames to the runner while their connection is the wire's current one.
class DesktopEnd final : public PeerLinks {
  public:
    explicit DesktopEnd(Wire& wire) : wire_(wire) {}
    void send(const QString& connectionId, protocol::Channel channel,
              const QByteArray& message) override;
    void sendBulk(const QString&, const QString&, const QByteArray&) override {}
    [[nodiscard]] bool bulkWritable(const QString&) const override { return true; }
    void close(const QString& connectionId, const QString& reason) override;

  private:
    Wire& wire_;
};

// The runner's end.
class RunnerEnd final : public runner::PeerChannel {
  public:
    explicit RunnerEnd(Wire& wire) : wire_(wire) {}
    void send(protocol::Channel channel, const QByteArray& message) override;
    [[nodiscard]] bool eventsWritable() const override { return true; }
    void close(const QString& reason) override;
    void requestConnection() override { ++requests; }
    int requests = 0;

  private:
    Wire& wire_;
};

class Lease final : public runner::JobLease {
  public:
    void start() override { ++starts; }
    void finish(const QString& outcome, const QString&) override { finished = outcome; }
    void release(const QString& reason) override { released = reason; }
    int starts = 0;
    QString finished;
    QString released;
};

class Receiver final : public runner::SnapshotReceiver {
  public:
    [[nodiscard]] QMap<QString, QString> heldSnapshots(const QString&) const override { return {}; }
    void begin(const protocol::JobSpec&, const QString&, Send) override { ++begins; }
    void receive(const protocol::Frame&) override {}
    void receivePayload(const QString&, const QByteArray&) override {}
    void cancel() override {}
    int begins = 0;
};

class Command final : public runner::CommandRunner {
  public:
    [[nodiscard]] bool admit() override { return true; }
    void launch(const runner::LaunchRequest&) override { ++launches; }
    void kill() override {}
    int launches = 0;
};

class RunnerClock final : public runner::SessionClock {
  public:
    [[nodiscard]] QDateTime now() const override { return time; }
    void arm(runner::SessionTimer, qint64) override {}
    void disarm(runner::SessionTimer) override {}
    QDateTime time = QDateTime(QDate(2026, 11, 18), QTime(9, 0), QTimeZone::UTC);
};

struct Message {
    bool toRunner = false;
    QString connectionId;
    protocol::Channel channel;
    QByteArray bytes;
};

struct Wire {
    Wire()
        : desktop(*this), runnerEnd(*this),
          session(jobId, daemonVersion,
                  SessionPorts{desktop, clock, records, specs, snapshots, spool}),
          runnerSession(
              runner::RunnerInfo{runnerId, "0.1.0", "linux", "x86_64", {"os:linux"}, {}},
              runner::SessionPorts{runnerEnd, lease, receiver, command, runnerClock, accepted}) {
        records.record.runsOn = {QStringLiteral("os:linux")};
    }

    // A new connection, as signaling and the transport would open it on both ends.
    void connect(const QString& connectionId) {
        current = connectionId;
        QVERIFY(session.offered(connectionId, runnerId, 1).ok());
        session.channelsOpened(connectionId);
        runnerSession.channelOpened(connectionId);
        pump();
    }
    // The connection drops on both ends; frames still on the wire are lost.
    void drop() {
        const QString lost = current;
        current.clear();
        queue.clear();
        session.connectionLost(lost);
        runnerSession.channelLost();
    }
    void pump() {
        while (!queue.isEmpty()) {
            const Message message = queue.takeFirst();
            if (message.connectionId != current)
                continue;
            if (message.toRunner) {
                runnerSession.frameReceived(message.channel, message.bytes);
            } else {
                session.frameReceived(message.connectionId, message.channel, message.bytes);
            }
        }
    }
    // Frames the runner sent, by type, in order.
    QStringList fromRunner;

    FakeClock clock;
    FakeRecords records;
    FakeSpecs specs;
    FakeSnapshots snapshots;
    MemorySpool spool;
    DesktopEnd desktop;
    RunnerEnd runnerEnd;
    Lease lease;
    Receiver receiver;
    Command command;
    RunnerClock runnerClock;
    runner::MemoryAcceptedJobs accepted;
    JobPeerSession session;
    runner::JobSession runnerSession;
    QString current;
    QList<Message> queue;
    QStringList closes;
};

void DesktopEnd::send(const QString& connectionId, const protocol::Channel channel,
                      const QByteArray& message) {
    wire_.queue.append({true, connectionId, channel, message});
}

void DesktopEnd::close(const QString& connectionId, const QString& reason) {
    wire_.closes.append(QStringLiteral("desktop ") + reason);
    if (connectionId == wire_.current) {
        wire_.current.clear();
        wire_.runnerSession.channelLost();
    }
}

void RunnerEnd::send(const protocol::Channel channel, const QByteArray& message) {
    const protocol::DecodeResult decoded =
        protocol::decode(message, channel, protocol::Direction::RunnerToDesktop);
    if (decoded.ok())
        wire_.fromRunner.append(QString(protocol::frameType(*decoded.frame)));
    wire_.queue.append({false, wire_.current, channel, message});
}

void RunnerEnd::close(const QString& reason) {
    wire_.closes.append(QStringLiteral("runner ") + reason);
    const QString lost = wire_.current;
    wire_.current.clear();
    wire_.session.connectionLost(lost);
}

const QString startedAt = QStringLiteral("2026-11-18T09:00:01Z");

runner::CommandExit exitedClean() {
    runner::CommandExit exit;
    exit.exitCode = 0;
    exit.cleanupStatus = QStringLiteral("verified");
    return exit;
}

} // namespace

class PeerSessionRunnerTest : public QObject {
    Q_OBJECT

  private slots:
    void aJobRunsToItsEnd() {
        Wire wire;
        QVERIFY(wire.runnerSession.assign({jobId, 1, machineId}));
        wire.connect("c1");
        QCOMPARE(wire.lease.starts, 1);
        wire.runnerSession.leaseStarted(startedAt);
        wire.pump();
        QCOMPARE(wire.snapshots.starts.size(), 1);
        wire.runnerSession.snapshotReady(QStringLiteral("/work/ws"), {});
        wire.pump();
        QCOMPARE(wire.command.launches, 1);
        wire.runnerSession.commandOutput("hello ");
        wire.runnerSession.commandOutput("world");
        wire.pump();
        wire.runnerSession.commandFinished(exitedClean());
        wire.pump();
        QCOMPARE(wire.spool.bytes, QByteArray("hello world"));
        QCOMPARE(wire.records.results.size(), 1);
        QCOMPARE(wire.records.results.first().state, QStringLiteral("exited"));
        QCOMPARE(wire.records.calls.mid(0, 5),
                 (QStringList{"offer runner-1 1", "peerOpened runner-1", "dispatch runner-1",
                              "accepted runner-1", "started runner-1"}));
        QCOMPARE(wire.session.phase(), JobPeerSession::Phase::Finished);
        QCOMPARE(wire.lease.finished, QStringLiteral("succeeded"));
        QVERIFY(wire.closes.isEmpty());
    }

    void aSpecSentAgainRunsTheCommandOnce_data() {
        QTest::addColumn<QString>("dropAt");
        QTest::newRow("before job.start") << "starting";
        QTest::newRow("job.accepted lost") << "accepted";
    }
    void aSpecSentAgainRunsTheCommandOnce() {
        QFETCH(QString, dropAt);
        Wire wire;
        QVERIFY(wire.runnerSession.assign({jobId, 1, machineId}));
        wire.connect("c1");
        if (dropAt == "accepted") {
            wire.runnerSession.leaseStarted(startedAt); // `job.accepted` never reaches the desktop
            wire.drop();
        } else {
            wire.drop();
            wire.runnerSession.leaseStarted(startedAt); // no connection: answered on the next
        }
        QVERIFY(!wire.records.record.accepted);
        wire.connect("c2");
        QCOMPARE(wire.records.calls.count("dispatch runner-1"), 1);
        QCOMPARE(wire.lease.starts, 1);
        QCOMPARE(wire.records.calls.count("accepted runner-1"), 1);
        QVERIFY(wire.records.record.accepted);
        wire.runnerSession.snapshotReady(QStringLiteral("/work/ws"), {});
        wire.pump();
        wire.runnerSession.commandFinished(exitedClean());
        wire.pump();
        QCOMPARE(wire.command.launches, 1);
        QCOMPARE(wire.records.results.size(), 1);
        QVERIFY(!wire.fromRunner.contains("job.rejected"));
    }

    void aLostConnectionResumesWithoutDuplicateOutput() {
        Wire wire;
        QVERIFY(wire.runnerSession.assign({jobId, 1, machineId}));
        wire.connect("c1");
        wire.runnerSession.leaseStarted(startedAt);
        wire.pump();
        wire.runnerSession.snapshotReady(QStringLiteral("/work/ws"), {});
        wire.pump();
        wire.runnerSession.commandOutput("abc");
        wire.pump();
        wire.runnerSession.commandOutput("def"); // lost on the wire
        wire.drop();
        QVERIFY(wire.records.calls.contains("peerLost runner-1"));
        wire.runnerSession.commandOutput("ghi"); // while no connection is open
        wire.connect("c2");
        QCOMPARE(wire.spool.bytes, QByteArray("abcdefghi"));
        QCOMPARE(wire.records.calls.count("started runner-1"), 1);
        wire.runnerSession.commandOutput("jkl");
        wire.runnerSession.commandFinished(exitedClean());
        wire.pump();
        QCOMPARE(wire.spool.bytes, QByteArray("abcdefghijkl"));
        QCOMPARE(wire.records.results.size(), 1);
        QCOMPARE(wire.records.results.first().outputBytes, 12);
        QCOMPARE(wire.command.launches, 1);
        QCOMPARE(wire.lease.finished, QStringLiteral("succeeded"));
    }

    void aResultWhoseAckWasLostIsAcknowledgedAgain() {
        Wire wire;
        QVERIFY(wire.runnerSession.assign({jobId, 1, machineId}));
        wire.connect("c1");
        wire.runnerSession.leaseStarted(startedAt);
        wire.pump();
        wire.runnerSession.snapshotReady(QStringLiteral("/work/ws"), {});
        wire.pump();
        wire.runnerSession.commandFinished(exitedClean());
        wire.drop(); // the result never arrives
        wire.connect("c2");
        QCOMPARE(wire.records.results.size(), 1);
        QCOMPARE(wire.lease.finished, QStringLiteral("succeeded"));
    }
};

QTEST_GUILESS_MAIN(PeerSessionRunnerTest)
#include "PeerSessionRunnerTest.moc"
