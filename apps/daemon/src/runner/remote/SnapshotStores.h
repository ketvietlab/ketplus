#pragma once

#include "runner/sync/PackCache.h"
#include "runner/sync/SnapshotWorker.h"
#include "runner/sync/StoreRetention.h"

#include <QDateTime>
#include <QHash>
#include <QString>

#include <memory>

// The daemon's snapshot stores, one per repository, each with its Git queue and its retention,
// and the one pack cache they share. Stores are keyed by task id inside: the in-flight ref and
// the pack of a remote job are named after its task, which exists before the job id does.
namespace ketplus::daemon::runner::remote {

class SnapshotStores final {
  public:
    SnapshotStores(QString storesRoot, QString packDirectory,
                   ketplus::runner::snapshot::GitTool tool = {});
    ~SnapshotStores();
    SnapshotStores(const SnapshotStores&) = delete;
    SnapshotStores& operator=(const SnapshotStores&) = delete;

    struct Store {
        QString path;
        std::unique_ptr<sync::SnapshotWorker> worker;
        std::unique_ptr<sync::StoreRetention> retention;
    };

    [[nodiscard]] const QString& storesRoot() const { return storesRoot_; }
    [[nodiscard]] const ketplus::runner::snapshot::GitTool& tool() const { return tool_; }
    [[nodiscard]] sync::PackCache& packs() { return packs_; }

    // The store at `path`, opened (its confirmed refs read) on first use. Nothing when `path`
    // is not below the stores root.
    [[nodiscard]] Store* open(const QString& path);
    // Releases a task's in-flight snapshot and pack in its store; for a task that never reached
    // a sender as well.
    void release(const QString& storePath, const QString& taskId);
    // A removed or unpaired runner: its confirmed snapshots and packs not in flight go.
    void releaseRunner(const QString& runnerId);
    // After a restart: the tasks whose packs stay wanted.
    void setLiveTasks(const QStringList& taskIds);
    // Pack cache limits now, and unreferenced loose objects older than a day on every open
    // store's queue.
    void sweep(const QDateTime& now);

  private:
    QString storesRoot_;
    ketplus::runner::snapshot::GitTool tool_;
    sync::PackCache packs_;
    QHash<QString, std::shared_ptr<Store>> stores_;
};

} // namespace ketplus::daemon::runner::remote
