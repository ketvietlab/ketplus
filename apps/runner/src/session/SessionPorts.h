#pragma once

#include "protocol/FrameRoutes.h"
#include "protocol/Frames.h"

#include <QByteArray>
#include <QDateTime>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <utility>

// What a job session needs from the rest of the runner. The session itself opens no socket,
// starts no process and reads no clock: every effect goes through these interfaces, and every
// answer comes back as a call on JobSession.
namespace ketplus::runner::session {

// The peer connection of the current job. Answers: JobSession::channelOpened, channelLost,
// frameReceived and eventsWritable.
class PeerChannel {
  public:
    virtual ~PeerChannel() = default;
    // One encoded frame on `control` or `events`.
    virtual void send(protocol::Channel channel, const QByteArray& message) = 0;
    // Whether `events` takes another frame now. When it does again after saying no, the channel
    // calls JobSession::eventsWritable.
    [[nodiscard]] virtual bool eventsWritable() const = 0;
    // Closes the open connection. The session treats it as lost at once; a later channelLost for
    // it is ignored.
    virtual void close(const QString& reason) = 0;
    // Asks for a new connection for the current job.
    virtual void requestConnection() = 0;
};

// The Relay lease of the current job (`job.start`, `job.finish`, `job.release`). Answers:
// JobSession::leaseStarted and leaseStartRefused; JobSession::leaseLost reports a refusal of a
// renewal or a Relay `job.cancel`. Refusals of finish and release are the lease's to absorb.
class JobLease {
  public:
    virtual ~JobLease() = default;
    virtual void start() = 0;
    virtual void finish(const QString& outcome, const QString& reason) = 0;
    virtual void release(const QString& reason) = 0;
};

// Receives the snapshot of the current job. Answers: JobSession::snapshotReady and
// snapshotFailed. It sends `snapshot.want`, `snapshot.ack` and the `snapshot.error` frames it
// recovers from through `send`; the session sends `snapshot.ready` and a final `snapshot.error`.
class SnapshotReceiver {
  public:
    using Send = std::function<void(const protocol::Frame&)>;
    virtual ~SnapshotReceiver() = default;
    // The `snapshots` of `hello`: workspaceKey -> snapshotId held for this desktop.
    [[nodiscard]] virtual QMap<QString, QString> heldSnapshots(const QString& machineId) const = 0;
    // `machineId` is the requesting desktop's.
    virtual void begin(const protocol::JobSpec& spec, const QString& machineId, Send send) = 0;
    // `snapshot.offer` and `snapshot.chunk` of the current job.
    virtual void receive(const protocol::Frame& frame) = 0;
    // A whole `bulk` transfer: the payload of the `snapshot.chunk` naming `transferId`, which
    // may arrive before or after it.
    virtual void receivePayload(const QString& transferId, const QByteArray& payload) = 0;
    virtual void cancel() = 0;
};

struct LaunchRequest {
    protocol::Invocation invocation;
    QString workspace;
    QString cwd;
    QMap<QString, QString> environment; // complete: nothing else is inherited
};

// How the command ended, reported once its processes are gone or given up on.
struct CommandExit {
    QString state = QStringLiteral("exited"); // "exited", or "failed" when it never ran
    std::optional<qint64> exitCode;
    std::optional<qint64> signal;
    QString reason;        // [a-z0-9_]{0,64}, for "failed"
    QString cleanupStatus; // "verified" or "unverified"
};

// Runs the job's command. Answers: JobSession::commandOutput and commandFinished (also after
// kill(), and when launch() fails).
class CommandRunner {
  public:
    virtual ~CommandRunner() = default;
    // The executor's admission probe.
    [[nodiscard]] virtual bool admit() = 0;
    virtual void launch(const LaunchRequest& request) = 0;
    virtual void kill() = 0;
};

enum class SessionTimer {
    PeerOpen,        // first connection: no `welcome` 60 seconds after the job was assigned,
                     // or no spec of the leased job 60 seconds after `welcome`
    Reconnect,       // ask for a new connection: 10 seconds after a loss, then every 30
    PeerAbsent,      // 15 minutes without an open connection while the job runs
    JobTimeout,      // `timeoutMs` after `job.started`
    ResultRetention, // 60 minutes holding a result the desktop has not acknowledged
};

// Time for the session. Answer: JobSession::timerFired. Arming an armed timer restarts it.
class SessionClock {
  public:
    virtual ~SessionClock() = default;
    [[nodiscard]] virtual QDateTime now() const = 0;
    virtual void arm(SessionTimer timer, qint64 delayMs) = 0;
    virtual void disarm(SessionTimer timer) = 0;
};

// Every (jobId, attempt) whose spec this runner took. A durable store replaces the in-memory one.
class AcceptedJobs {
  public:
    virtual ~AcceptedJobs() = default;
    [[nodiscard]] virtual bool contains(const QString& jobId, qint64 attempt) const = 0;
    virtual void add(const QString& jobId, qint64 attempt) = 0;
};

class MemoryAcceptedJobs final : public AcceptedJobs {
  public:
    [[nodiscard]] bool contains(const QString& jobId, const qint64 attempt) const override {
        return m_jobs.contains({jobId, attempt});
    }
    void add(const QString& jobId, const qint64 attempt) override {
        m_jobs.insert({jobId, attempt});
    }

  private:
    QSet<std::pair<QString, qint64>> m_jobs;
};

} // namespace ketplus::runner::session
