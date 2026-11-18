#pragma once

#include "protocol/Frames.h"
#include "session/OutputBuffer.h"
#include "session/SessionPorts.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

// The runner's side of one job on the peer connection, from the lease to the desktop's
// acknowledgement of the result: handshake, spec, snapshot, launch, output, cancel, deadline,
// lost connections and lost leases. One job at a time; a spec for a job already taken never
// launches a second command. Driven only by the calls below.
namespace ketplus::runner::session {

inline constexpr qint64 peerOpenMs = 60'000;
inline constexpr qint64 firstReconnectMs = 10'000;
inline constexpr qint64 reconnectMs = 30'000;
inline constexpr qint64 peerAbsentMs = 15LL * 60 * 1000;
inline constexpr qint64 resultRetentionMs = 60LL * 60 * 1000;

struct RunnerInfo {
    QString runnerId;
    QString runnerVersion;
    QString os;
    QString arch;
    QStringList labels;
    QMap<QString, QString> baseEnvironment;
};

// A lease the runner holds.
struct JobAssignment {
    QString jobId;
    qint64 attempt = 1;
    QString machineId; // the requesting desktop
};

struct SessionPorts {
    PeerChannel& channel;
    JobLease& lease;
    SnapshotReceiver& snapshots;
    CommandRunner& command;
    SessionClock& clock;
    AcceptedJobs& accepted;
};

class JobSession {
  public:
    enum class Phase {
        Idle,       // no lease
        Assigned,   // lease held; waiting for the connection, `welcome` and the spec
        Starting,   // spec taken, `job.start` asked
        Syncing,    // `job.accepted` sent; receiving the snapshot
        Running,    // command launched, `job.started` sent
        Stopping,   // command killed; waiting for its cleanup
        Delivering, // holding `job.result` until `job.result.ack`
    };

    JobSession(RunnerInfo runner, SessionPorts ports);

    // A new lease. False while another job is held.
    bool assign(const JobAssignment& job);

    void channelOpened(const QString& connectionId);
    void channelLost();
    void frameReceived(protocol::Channel channel, const QByteArray& message);
    // `events` takes frames again.
    void eventsWritable();
    // A whole transfer on `bulk`: a snapshot chunk's payload, passed on while syncing.
    void bulkReceived(const QString& transferId, const QByteArray& payload);

    void leaseStarted(const QString& startedAt);
    void leaseStartRefused(const QString& code);
    // "lease_expired", "entitlement_required", "job_cancelled" or "unsupported"; `reason` is the
    // Relay's reason of a "job_cancelled", when it gives one.
    void leaseLost(const QString& code, const QString& reason = {});

    void snapshotReady(const QString& workspace, const QStringList& warnings);
    void snapshotFailed(const QString& code);

    void commandOutput(const QByteArray& bytes);
    void commandFinished(const CommandExit& exit);

    void timerFired(SessionTimer timer);

    [[nodiscard]] Phase phase() const { return m_phase; }
    [[nodiscard]] const OutputBuffer& output() const { return m_output; }

  private:
    enum class Link { None, AwaitingWelcome, Open };

    struct Job {
        JobAssignment assignment;
        std::optional<protocol::JobSpec> spec;
        QString startedAt; // from the lease
        bool launched = false;
        bool relayUnsupported = false;
        bool welcomed = false;                // the first `welcome` came
        bool leaseGone = false;               // the Relay no longer holds the lease
        std::optional<QString> pendingCancel; // a cancel that came while `job.start` was asked
        std::optional<std::pair<QString, QString>> stopping; // state and reason of a kill
        std::optional<protocol::JobResult> result;
        QStringList warnings;
        bool finished = false;
    };

    // Handshake and frames (JobSessionChannel.cpp).
    void sendHello();
    void onFrame(const protocol::Frame& frame);
    void onBrokenFrame(protocol::Channel channel, const protocol::FrameError& error);
    void onWelcome(const protocol::Welcome& welcome);
    void closeChannel(const QString& reason);
    void linkLost();
    bool send(const protocol::Frame& frame);
    void reject(const QString& jobId, qint64 attempt, protocol::ErrorCode code);
    [[nodiscard]] bool isCurrent(const QString& jobId) const;

    // Spec, snapshot and launch (JobSessionSpec.cpp).
    void onSpec(const protocol::JobSpec& spec);
    void onInvalidSpec(const QString& jobId, qint64 attempt);
    void refuseSpec(protocol::ErrorCode code);
    void answerCurrentState();
    void beginSnapshot();
    [[nodiscard]] LaunchRequest launchRequest(const QString& workspace) const;

    // Output (JobSessionOutput.cpp).
    void sendOutput();
    // Output, then the held result once every byte has gone out on this connection.
    void flush();
    void resumeOutput(qint64 fromOffset);
    void onResume(const protocol::JobResume& resume);

    // Cancel, deadline and losses (JobSessionStop.cpp).
    void cancel(const QString& reason);
    void stop(const QString& state, const QString& reason);

    // Result and the end of the job (JobSessionResult.cpp).
    void endWithoutCommand(const QString& state, const QString& reason);
    void deliver(protocol::JobResult result);
    void sendResult();
    void onResultAck();
    void finishResult();
    void finishLease(const QString& outcome, const QString& reason);
    void endJob();

    RunnerInfo m_runner;
    SessionPorts m_ports;
    Phase m_phase = Phase::Idle;
    std::optional<Job> m_job;
    quint64 m_generation = 0; // changes with each job, so late snapshot sends are dropped

    Link m_link = Link::None;
    QString m_connectionId;
    bool m_streaming = false; // the desktop has asked for output on this connection
    OutputBuffer m_output;
    qint64 m_sentUpTo = 0;
    bool m_resultPending = false; // the held result is still to be sent on this connection
};

// The `job.finish` outcome for a result state ("succeeded", "failed", "cancelled",
// "interrupted").
[[nodiscard]] QString finishOutcome(const protocol::JobResult& result);

} // namespace ketplus::runner::session
