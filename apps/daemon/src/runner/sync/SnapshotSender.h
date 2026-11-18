#pragma once

#include "runner/sync/SnapshotSyncPorts.h"

#include "protocol/FrameLimits.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>

// Sends one job's snapshot to its runner: picks the base, has the pack built, offers it, sends
// chunks against the runner's acknowledgements and resumes on a new connection. Every input
// runs on the event loop; frames from the runner are untrusted.
namespace ketplus::daemon::runner::sync {

struct SyncJob {
    QString jobId;
    QString runnerId;
    QString workspaceKey;
    QString snapshotId;
    QStringList warnings; // wire codes only
};

struct SenderLimits {
    qint64 maxPackBytes = protocol::limits::maxPackBytes;
    int maxChunksInFlight = 4;
};

class SnapshotSender final {
  public:
    enum class Phase {
        Idle,       // not started
        Verifying,  // a kept pack is hashed again before it is offered
        Packing,    // a pack is being built
        Offered,    // offer sent, waiting for snapshot.want
        Sending,    // runner needs bytes; chunks go out against acks
        AwaitReady, // runner has the snapshot; waiting for snapshot.ready
        Detached,   // connection lost; waiting for a new hello
        Ready,
        Failed,
        Cancelled,
    };

    SnapshotSender(SyncJob job, SyncLink& link, SyncWork& work, SyncRetention& retention,
                   SyncEvents& events, SenderLimits limits = {});
    ~SnapshotSender();
    SnapshotSender(const SnapshotSender&) = delete;
    SnapshotSender& operator=(const SnapshotSender&) = delete;

    // After the runner accepted the spec; `snapshots` is the current connection's hello map. A
    // pack kept for the job (after a restart) is offered again when its base passes the base
    // check and the file still has its recorded SHA-256.
    void start(const QMap<QString, QString>& snapshots);
    // A frame from the runner on control. Frames that are not `snapshot.*` are ignored.
    void receive(const protocol::Frame& frame);
    void bulkWritable();
    void disconnected();
    // A new connection with this hello map: the same pack is offered again when it is still
    // kept, its base passes the base check and the file still has its SHA-256; otherwise a new
    // pack is built.
    void reconnected(const QMap<QString, QString>& snapshots);
    // Stops without a report upward; drops the job's pack and its in-flight snapshot.
    void cancel();

    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] bool finished() const;
    [[nodiscard]] const std::optional<PackFile>& pack() const { return pack_; }
    [[nodiscard]] qint64 acked() const { return acked_; }
    [[nodiscard]] qint64 sent() const { return next_; }
    [[nodiscard]] QString failure() const { return failure_; }

  private:
    void reuseOrBuild();
    void packHashed(const PackFile& kept, const QString& sha256);
    [[nodiscard]] bool usable(const PackFile& kept) const;
    void build(const QString& baseId);
    void packBuilt(const PackOutcome& outcome, const QString& baseId);
    void offer();
    void want(const protocol::SnapshotWant& frame);
    void ack(const protocol::SnapshotAck& frame);
    void error(const protocol::SnapshotError& frame);
    void ready();
    void pump();
    void chunkRead(const ChunkRead& chunk, qint64 offset, qint64 length);
    void sendChunk(const QByteArray& bytes, const QString& sha256, qint64 offset);
    void stopTransfer();
    void fail(const QString& code);
    [[nodiscard]] QString baseFor(const QMap<QString, QString>& snapshots) const;
    [[nodiscard]] bool onBoundary(qint64 offset) const;
    [[nodiscard]] bool matches(const QString& snapshotId) const;

    struct PendingChunk {
        QByteArray bytes;
        QString sha256;
        qint64 offset = 0;
    };

    SyncJob job_;
    SyncLink& link_;
    SyncWork& work_;
    SyncRetention& retention_;
    SyncEvents& events_;
    SenderLimits limits_;
    Phase phase_ = Phase::Idle;
    QMap<QString, QString> snapshots_; // the latest hello map
    std::optional<PackFile> pack_;
    qint64 acked_ = 0; // next byte the runner needs
    qint64 next_ = 0;  // next byte to send
    bool reading_ = false;
    std::optional<PendingChunk> pending_;
    QString failure_;
    quint64 generation_ = 0; // bumped whenever an outstanding answer must be ignored
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace ketplus::daemon::runner::sync
