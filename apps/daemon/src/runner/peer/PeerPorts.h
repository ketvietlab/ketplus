#pragma once

#include "protocol/FrameRoutes.h"
#include "protocol/Frames.h"

#include <QByteArray>
#include <QDateTime>
#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

// What the desktop's peer sessions of remote jobs need from the rest of the daemon. A session
// opens no connection, reads no clock and writes no file: every effect goes through these ports
// and every answer comes back as a call on the session.
namespace ketplus::daemon::runner::peer {

namespace protocol = ketplus::runner::protocol;

// The authenticated peer connections of the daemon, by connection id.
class PeerLinks {
  public:
    virtual ~PeerLinks() = default;
    // One encoded frame on `control` or `events` of the connection.
    virtual void send(const QString& connectionId, protocol::Channel channel,
                      const QByteArray& message) = 0;
    // One bulk transfer of the connection.
    virtual void sendBulk(const QString& connectionId, const QString& transferId,
                          const QByteArray& bytes) = 0;
    [[nodiscard]] virtual bool bulkWritable(const QString& connectionId) const = 0;
    // Closes the connection; no later call reports it lost.
    virtual void close(const QString& connectionId, const QString& reason) = 0;
};

enum class PeerTimer {
    Hello,      // 10 seconds after the channels open
    OutputAck,  // 1 second after the first unacknowledged output byte
    Retry,      // the next attempt of a record call that could not be written
    SpoolRetry, // 5 seconds after the spool could not write
};

// Time for the sessions. Answer: the session's timerFired. Arming an armed timer restarts it.
class PeerClock {
  public:
    virtual ~PeerClock() = default;
    [[nodiscard]] virtual QDateTime now() const = 0;
    virtual void arm(const QString& jobId, PeerTimer timer, qint64 delayMs) = 0;
    virtual void disarm(const QString& jobId, PeerTimer timer) = 0;
};

// The output spool of one job. Bytes are durable once a call returns them as written. A spool
// with a size limit still takes every byte it is given as written.
class OutputSpool {
  public:
    virtual ~OutputSpool() = default;
    // The logical offset durably in the spool (bytes written and gaps recorded): the next offset
    // expected.
    [[nodiscard]] virtual qint64 size() const = 0;
    // Appends `bytes` at size(); false when nothing could be written durably.
    [[nodiscard]] virtual bool append(const QByteArray& bytes) = 0;
    // Durably records that [size(), toOffset) was lost and moves size() to `toOffset`.
    [[nodiscard]] virtual bool markGap(qint64 toOffset) = 0;
};

// An answer of the job record: empty `code` lets the session go on.
struct RecordAnswer {
    QString code;
    [[nodiscard]] bool ok() const { return code.isEmpty(); }
};

// What the session reads of its job record.
struct RecordView {
    QString dispatchedTo; // the runner the spec may have reached
    QStringList runsOn;
    bool accepted = false; // the record took `job.accepted` (placement syncing or running)
    bool started = false;  // the record took `job.started` (placement running)
    QString stopReason;    // "user", "deadline" or "snapshot_failed" once stopping
    bool terminal = false;
};

// The desktop record of remote jobs, the only authority on a job's state.
class JobRecords {
  public:
    virtual ~JobRecords() = default;
    [[nodiscard]] virtual std::optional<RecordView> view(const QString& jobId) const = 0;
    [[nodiscard]] virtual RecordAnswer offer(const QString& jobId, const QString& runnerId,
                                             qint64 attempt) = 0;
    [[nodiscard]] virtual RecordAnswer dispatch(const QString& jobId, const QString& runnerId) = 0;
    virtual void accepted(const QString& jobId, const QString& runnerId) = 0;
    virtual void started(const QString& jobId, const QString& runnerId) = 0;
    virtual void rejected(const QString& jobId, const QString& runnerId, const QString& code) = 0;
    // The desktop could not send the snapshot: ok when the record stops the job for it,
    // not_applicable when it does not, storage_error when it could not be written.
    [[nodiscard]] virtual RecordAnswer snapshotFailed(const QString& jobId, const QString& runnerId,
                                                      const QString& code) = 0;
    // Ok once the result is durable, also for a result already recorded.
    [[nodiscard]] virtual RecordAnswer result(const QString& jobId, const QString& runnerId,
                                              const protocol::JobResult& result) = 0;
    virtual void peerOpened(const QString& jobId, const QString& runnerId) = 0;
    virtual void peerLost(const QString& jobId, const QString& runnerId,
                          const QDateTime& lastContact) = 0;
    // The job id of a task, empty when the record names none.
    [[nodiscard]] virtual QString jobOf(const QString& taskId) const = 0;
};

// The spec of a job, without `epoch`; nullopt when it cannot be built. The same job gets the
// same spec for its whole life, also after the daemon restarts.
class JobSpecSource {
  public:
    virtual ~JobSpecSource() = default;
    [[nodiscard]] virtual std::optional<protocol::JobSpec> spec(const QString& jobId,
                                                                qint64 attempt) = 0;
};

// Sends the snapshot of a job. Answers, possibly later and never from inside a call: the
// session's snapshotReady and snapshotFailed.
class SnapshotPort {
  public:
    using Send = std::function<void(const protocol::Frame&)>;
    using SendBulk = std::function<void(const QString& transferId, const QByteArray& bytes)>;
    virtual ~SnapshotPort() = default;
    // `held` is the `snapshots` of the current connection's `hello`.
    virtual void start(const QString& jobId, const QString& runnerId,
                       const QMap<QString, QString>& held, Send send, SendBulk sendBulk) = 0;
    // A `snapshot.want`, `snapshot.ack`, `snapshot.ready` or `snapshot.error` of the job.
    virtual void receive(const QString& jobId, const protocol::Frame& frame) = 0;
    virtual void bulkWritable(const QString& jobId) = 0;
    virtual void cancel(const QString& jobId) = 0;
};

} // namespace ketplus::daemon::runner::peer
