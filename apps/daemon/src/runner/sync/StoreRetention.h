#pragma once

#include "runner/sync/PackCache.h"
#include "runner/sync/SnapshotSyncPorts.h"
#include "snapshot/SealedGit.h"

#include <QDateTime>
#include <QMap>
#include <QString>

// SyncRetention over one snapshot store and the daemon's pack cache. Store refs hold the
// snapshot each runner confirmed per workspace key and the snapshot of each job in flight;
// ids enter ref names only as digests. load(), confirm(), forget(), release() and
// releaseRunner() run short Git ref commands on the store synchronously, each limited to
// refCommandTimeoutMs; one that fails or runs out of time counts as failed. prune() runs Git
// for longer: call it off the event loop.
namespace ketplus::daemon::runner::sync {

inline constexpr int refCommandTimeoutMs = 5'000;

class StoreRetention final : public SyncRetention {
  public:
    StoreRetention(QString storePath, PackCache& packs,
                   ketplus::runner::snapshot::GitTool tool = {});

    // Reads the confirmed refs of the store; false when Git could not list them.
    bool load();

    // The store ref name (below the KetPlus prefix) holding a job's snapshot while it is in
    // flight; prepareSnapshot() writes it.
    [[nodiscard]] static QString inFlightRef(const QString& jobId);

    [[nodiscard]] std::optional<QString> confirmed(const QString& runnerId,
                                                   const QString& workspaceKey) const override;
    bool confirm(const QString& runnerId, const QString& workspaceKey,
                 const QString& snapshotId) override;
    void forget(const QString& runnerId, const QString& workspaceKey) override;

    [[nodiscard]] std::optional<PackFile> pack(const QString& jobId) const override;
    void keepPack(const QString& jobId, const QString& runnerId, const PackFile& pack) override;
    void dropPack(const QString& jobId) override;
    void release(const QString& jobId) override;

    // Forgets every snapshot the runner confirmed and deletes its packs not in flight, when it is
    // removed or unpaired.
    void releaseRunner(const QString& runnerId);
    // Deletes unreferenced loose objects of the store last written before `expireBefore`.
    bool prune(const QDateTime& expireBefore);

  private:
    [[nodiscard]] static QString confirmedRef(const QString& runnerId, const QString& workspaceKey);

    QString storePath_;
    PackCache& packs_;
    ketplus::runner::snapshot::GitTool tool_;
    ketplus::runner::snapshot::GitTool refTool_; // tool_ limited to refCommandTimeoutMs
    QMap<QString, QString> confirmed_; // ref name -> snapshot id
};

// Ends a job that never reached a sender (cancelled in the queue, expired, rejected): drops its
// in-flight ref and its pack. True when the ref is gone. The ref command is limited to
// refCommandTimeoutMs.
bool dropJobSnapshot(const QString& storePath, PackCache& packs, const QString& jobId,
                     const ketplus::runner::snapshot::GitTool& tool = {});

} // namespace ketplus::daemon::runner::sync
