#pragma once

#include "snapshot/ReceiverStore.h"
#include "snapshot/SealedGit.h"
#include "snapshot/SnapshotResult.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

// The receiver's disk and Git work. Everything here runs on the receiver's worker thread, one
// job after another, and returns plain values; nothing here calls the session.
namespace ketplus::runner::receiver {

enum class WriteStatus { Ok, Corrupt, DiskFull, Failed };

// Writes `data` at `offset` of the pack file and syncs it before returning.
using ChunkWriter =
    std::function<WriteStatus(const QString& path, qint64 offset, const QByteArray& data)>;
[[nodiscard]] WriteStatus writeAndSync(const QString& path, qint64 offset, const QByteArray& data);

// Points where a test may hold the worker.
enum class WorkStep { Write, Apply, Applied };

class ReceiverWorker {
  public:
    ReceiverWorker();
    ~ReceiverWorker(); // waits for the job that runs
    ReceiverWorker(const ReceiverWorker&) = delete;
    ReceiverWorker& operator=(const ReceiverWorker&) = delete;
    void post(std::function<void()> job);

  private:
    QThread m_thread;
    QObject* m_object = nullptr;
};

struct WorkContext {
    const ReceiverStore& store;
    snapshot::GitTool git;
    ChunkWriter writer;
    std::function<QDateTime()> now;
    std::function<void(WorkStep)> hold; // tests only
};

struct ProbeResult {
    std::optional<snapshot::Error> error;
    bool haveSnapshot = false;
    bool haveBase = true;
    std::optional<PartialRecord> partial; // what is kept for this workspace now
    bool removedOrphan = false;
};
// Whether the mirror holds the snapshot or the base, and what of a partial pack can resume.
[[nodiscard]] ProbeResult probe(const WorkContext& context, const QString& repositoryKey,
                                const QString& workspaceKey, const QString& snapshotId,
                                const std::optional<QString>& baseId, const QString& packSha256,
                                const std::optional<PartialRecord>& partial, bool orphan);

struct Eviction {
    WorkspaceRecord workspace;
    bool repositoryToo = false; // its last workspace
};
// Removes the workspaces (tree, index, ref, record), then their repositories when asked.
[[nodiscard]] bool evict(const WorkContext& context, const QList<Eviction>& evictions);

// The repository record (owner) and an empty partial pack, before the first chunk.
[[nodiscard]] std::optional<PartialRecord>
startPack(const WorkContext& context, const RepositoryRecord& repository,
          const QString& workspaceKey, const QString& packSha256, qint64 packBytes,
          std::optional<PartialRecord> resumed, QString* error);

struct ChunkResult {
    WriteStatus status = WriteStatus::Ok;
    PartialRecord partial;
};
// Checks the chunk's length and SHA-256, writes and syncs it, then records the new offset.
[[nodiscard]] ChunkResult writeChunk(const WorkContext& context, PartialRecord partial,
                                     const QByteArray& data, qint64 bytes, const QString& sha256);

struct ApplyInput {
    RepositoryRecord repository;
    QString workspaceKey;
    QString owner;
    std::optional<WorkspaceRecord> previous;
    bool fromMirror = false;
    QString packSha256;
    qint64 packBytes = 0;
    QString commit;
    bool clean = false;
    std::shared_ptr<std::atomic_bool> cancelled;
};
struct ApplyOutcome {
    std::optional<snapshot::Error> error; // none: applied
    bool cancelled = false;
    std::optional<WorkspaceRecord> record; // the workspace's record now; none: no record
};
[[nodiscard]] ApplyOutcome apply(const WorkContext& context, const ApplyInput& input);

// Deletes partial packs idle for more than `keepMs`, then repositories left without a
// workspace or a partial pack, measures what remains and stores the sizes.
[[nodiscard]] StoredRecords tidy(const WorkContext& context, qint64 keepMs);

// Sizes on disk, links not followed.
[[nodiscard]] qint64 diskBytes(const QString& path);

} // namespace ketplus::runner::receiver
