#pragma once

#include "runner/sync/PackCache.h"
#include "runner/sync/SnapshotSyncPorts.h"
#include "snapshot/SealedGit.h"

#include <QObject>
#include <QString>
#include <QThreadPool>

#include <memory>

// Runs a store's Git work on one serial queue and reads pack chunks on a separate worker, and
// answers on the thread that created it. Destruction waits for running work and drops every
// answer not yet delivered.
namespace ketplus::daemon::runner::sync {

class SnapshotWorker {
  public:
    SnapshotWorker(QString storePath, PackCache& packs,
                   ketplus::runner::snapshot::GitTool tool = {});
    ~SnapshotWorker();
    SnapshotWorker(const SnapshotWorker&) = delete;
    SnapshotWorker& operator=(const SnapshotWorker&) = delete;

    // Writes the job's pack into the cache; on the store queue.
    void buildPack(const QString& jobId, const QString& snapshotId, const QString& baseId,
                   SyncWork::PackCallback callback);
    void readChunk(const PackFile& pack, qint64 offset, qint64 length,
                   SyncWork::ChunkCallback callback);
    // Any other work on the store queue, such as preparing a snapshot or pruning.
    void runOnStore(std::function<void()> work);

  private:
    QString storePath_;
    PackCache& packs_;
    ketplus::runner::snapshot::GitTool tool_;
    QThreadPool store_;
    QThreadPool chunks_;
    std::unique_ptr<QObject> context_;
};

// SyncWork of one job over a worker.
class JobSyncWork final : public SyncWork {
  public:
    JobSyncWork(SnapshotWorker& worker, QString jobId);
    void buildPack(const QString& snapshotId, const QString& baseId,
                   PackCallback callback) override;
    void readChunk(const PackFile& pack, qint64 offset, qint64 length,
                   ChunkCallback callback) override;

  private:
    SnapshotWorker& worker_;
    QString jobId_;
};

// Reads `length` bytes at `offset` of the pack and their SHA-256; empty bytes on any failure,
// including a file whose size is no longer the pack's.
[[nodiscard]] ChunkRead readPackChunk(const PackFile& pack, qint64 offset, qint64 length);

} // namespace ketplus::daemon::runner::sync
