#pragma once

#include "protocol/Frames.h"
#include "session/JobSession.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QStringList>

#include <memory>
#include <optional>
#include <utility>

// Fakes for every interface of the job session and a host that wires them, with a clock that
// only moves when a test moves it.
namespace runner_session_test {

using namespace ketplus::runner;
using session::SessionTimer;

class FakeChannel final : public session::PeerChannel {
  public:
    void send(protocol::Channel channel, const QByteArray& message) override;
    void close(const QString& reason) override { closes.append(reason); }
    void requestConnection() override { ++connectionRequests; }
    [[nodiscard]] bool eventsWritable() const override { return writable; }

    // Every frame the runner sent, decoded as the desktop would, in order.
    QList<std::pair<protocol::Channel, protocol::Frame>> sent;
    QStringList closes;
    int connectionRequests = 0;
    int undecodable = 0;
    bool writable = true; // whether `events` takes frames
};

class FakeLease final : public session::JobLease {
  public:
    void start() override { calls.append(QStringLiteral("start")); }
    void finish(const QString& outcome, const QString& reason) override {
        calls.append(QStringLiteral("finish %1 %2").arg(outcome, reason));
    }
    void release(const QString& reason) override {
        calls.append(QStringLiteral("release %1").arg(reason));
    }
    QStringList calls;
};

class FakeSnapshots final : public session::SnapshotReceiver {
  public:
    [[nodiscard]] QMap<QString, QString> heldSnapshots(const QString& machineId) const override {
        return machineId == QStringLiteral("desktop-1") ? held : QMap<QString, QString>{};
    }
    void begin(const protocol::JobSpec& spec, const QString& machineId, Send sendFrame) override {
        begun.append(spec.jobId);
        machines.append(machineId);
        send = std::move(sendFrame);
    }
    void receive(const protocol::Frame& frame) override { received.append(frame); }
    void receivePayload(const QString& transferId, const QByteArray& payload) override {
        payloads.append({transferId, payload});
    }
    void cancel() override { ++cancels; }

    QMap<QString, QString> held;
    QStringList begun;
    QStringList machines;
    QList<std::pair<QString, QByteArray>> payloads;
    Send send;
    QList<protocol::Frame> received;
    int cancels = 0;
};

class FakeCommand final : public session::CommandRunner {
  public:
    [[nodiscard]] bool admit() override { return admits; }
    void launch(const session::LaunchRequest& request) override { launches.append(request); }
    void kill() override { ++kills; }

    bool admits = true;
    QList<session::LaunchRequest> launches;
    int kills = 0;
};

class FakeClock final : public session::SessionClock {
  public:
    [[nodiscard]] QDateTime now() const override { return time; }
    void arm(SessionTimer timer, qint64 delayMs) override {
        deadlines[timer] = time.addMSecs(delayMs);
    }
    void disarm(SessionTimer timer) override { deadlines.remove(timer); }
    [[nodiscard]] bool armed(SessionTimer timer) const { return deadlines.contains(timer); }
    // Milliseconds until `timer` fires, or -1 when it is not armed.
    [[nodiscard]] qint64 remaining(SessionTimer timer) const;

    QDateTime time;
    QMap<SessionTimer, QDateTime> deadlines;
};

// The in-memory record, with a disk that can fail.
class FakeAccepted final : public session::AcceptedJobs {
  public:
    [[nodiscard]] bool contains(const QString& jobId, qint64 attempt) const override {
        return memory.contains(jobId, attempt);
    }
    [[nodiscard]] bool add(const QString& jobId, qint64 attempt) override {
        return !failAdd && memory.add(jobId, attempt);
    }
    session::MemoryAcceptedJobs memory;
    bool failAdd = false;
};

struct Host {
    // `acceptedJobs` replaces the in-memory record, such as with the one on disk; `jobLease`
    // replaces the fake lease, such as with a JobDelivery's.
    explicit Host(session::AcceptedJobs* acceptedJobs = nullptr,
                  session::JobLease* jobLease = nullptr);

    // Moves the clock, firing every timer that comes due on the way, earliest first.
    void advance(qint64 milliseconds);

    // Desktop side.
    void deliver(const protocol::Frame& frame);
    void deliverRaw(protocol::Channel channel, const QByteArray& message);
    void open(const QString& connectionId = QStringLiteral("c1"));
    void welcome(const QStringList& capabilities = {QStringLiteral("runner.protocol/1")});
    // Assign, open and welcome.
    void connect();
    // Through `job.accepted`: assign, connect, spec, start answered.
    void accept(const protocol::JobSpec& spec);
    // Through `job.started`.
    void launch(const protocol::JobSpec& spec);
    // Back to the frames list starting now.
    void clearSent() { channel.sent.clear(); }

    [[nodiscard]] QStringList sentTypes() const;
    template <typename T> [[nodiscard]] QList<T> sentOf() const {
        QList<T> out;
        for (const auto& [ch, frame] : channel.sent)
            if (const T* f = std::get_if<T>(&frame))
                out.append(*f);
        return out;
    }

    FakeChannel channel;
    FakeLease lease;
    FakeSnapshots snapshots;
    FakeCommand command;
    FakeClock clock;
    FakeAccepted accepted;
    std::unique_ptr<session::JobSession> session;
    QString connectionId;
};

[[nodiscard]] protocol::JobSpec makeSpec(const QString& jobId = QStringLiteral("job-1"),
                                         qint64 attempt = 1);
[[nodiscard]] session::JobAssignment assignment(const QString& jobId = QStringLiteral("job-1"),
                                                qint64 attempt = 1);
inline const QString startedAt = QStringLiteral("2026-11-18T09:00:00Z");

class RunnerSessionHandshakeTest : public QObject {
    Q_OBJECT
  private slots:
    void helloCarriesTheJobAndSnapshots();
    void welcomeWithoutTheProtocolReleases();
    void framesBeforeWelcomeClose();
    void brokenFramesClose_data();
    void brokenFramesClose();
    void unknownTypeOnControlIsRejected();
    void noWelcomeWithinAMinuteReleases();
    void noSpecWithinAMinuteOfWelcomeReleases();
    void pingIsAnswered();
};

class RunnerSessionSpecTest : public QObject {
    Q_OBJECT
  private slots:
    void refusals_data();
    void refusals();
    void orderOfStartAcceptedAndStarted();
    void environmentOfTheCommand();
    void repeatedSpecNeverLaunchesTwice();
    void otherJobWhileBusy();
    void snapshotFailureEndsTheJob();
    void unrecordedSpecIsRefused();
    void specAcceptedBeforeARestartIsRefused();
    void nextJobAfterTheResult();
};

class RunnerSessionOutputTest : public QObject {
    Q_OBJECT
  private slots:
    void bufferKeepsTheNewestBytes();
    void pagesAreContiguousAndSmall();
    void ackReleasesTheBuffer();
    void resumeSendsFromTheAskedOffset();
    void overflowReportsOneGap_data();
    void overflowReportsOneGap();
    void blockedEventsReportOneGap_data();
    void blockedEventsReportOneGap();
    void resultWaitsForBlockedOutput();
    void outputBytesCountsEverything();
};

// A session over a JobDelivery's lease.
class RunnerSessionDeliveryTest : public QObject {
    Q_OBJECT
  private slots:
    void unrecordedSpecStopsTakingJobs();
    void sessionPhasesReachTheHeartbeat();
};

class RunnerSessionStopTest : public QObject {
    Q_OBJECT
  private slots:
    void cancelInEachPhase();
    void cancelWhileStartIsAsked();
    void repeatedCancelChangesNothing();
    void timeoutCountsFromStarted();
    void resultAndCancelRace();
    void cleanupStatusIsKept();
    void lostChannelReconnectsAndResumes();
    void peerAbsentFifteenMinutes();
    void resultHeldSixtyMinutes();
    void leaseLosses_data();
    void leaseLosses();
    void leaseLostWhileHoldingTheResult();
    void finishReasons_data();
    void finishReasons();
    void unsupportedAfterStartRunsToTheEnd();
    void relayCancelWithoutChannel();
    void withdrawOnlyBeforeTheSpec();
};

} // namespace runner_session_test
