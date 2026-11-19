#include "runner/remote/SnapshotStores.h"

#include "snapshot/SnapshotStore.h"

#include <QDir>
#include <QFileInfo>

namespace ketplus::daemon::runner::remote {

namespace {
constexpr qint64 looseObjectAgeSeconds = 24 * 60 * 60;

bool below(const QString& root, const QString& path) {
    const QString parent = QDir::cleanPath(QFileInfo(path).absolutePath());
    return !root.isEmpty() && parent == QDir::cleanPath(QDir(root).absolutePath());
}
} // namespace

SnapshotStores::SnapshotStores(QString storesRoot, QString packDirectory,
                               ketplus::runner::snapshot::GitTool tool)
    : storesRoot_(std::move(storesRoot)), tool_(std::move(tool)), packs_(std::move(packDirectory)) {
}

// Each worker waits for its running work as it goes, before the pack cache does.
SnapshotStores::~SnapshotStores() { stores_.clear(); }

SnapshotStores::Store* SnapshotStores::open(const QString& path) {
    if (const auto found = stores_.constFind(path); found != stores_.cend())
        return found->get();
    if (!below(storesRoot_, path))
        return nullptr;
    auto store = std::make_shared<Store>();
    store->path = path;
    store->worker = std::make_unique<sync::SnapshotWorker>(path, packs_, tool_);
    store->retention = std::make_unique<sync::StoreRetention>(path, packs_, tool_);
    // A store whose refs cannot be listed still sends full packs: no base is confirmed.
    (void)store->retention->load();
    stores_.insert(path, store);
    return store.get();
}

void SnapshotStores::release(const QString& storePath, const QString& taskId) {
    if (Store* store = open(storePath))
        store->retention->release(taskId);
    else
        packs_.remove(taskId);
}

void SnapshotStores::releaseRunner(const QString& runnerId) {
    packs_.removeRunner(runnerId);
    for (const auto& store : std::as_const(stores_))
        store->retention->releaseRunner(runnerId);
}

void SnapshotStores::setLiveTasks(const QStringList& taskIds) { packs_.setLiveJobs(taskIds); }

void SnapshotStores::sweep(const QDateTime& now) {
    packs_.sweep(now);
    const QDateTime expireBefore = now.addSecs(-looseObjectAgeSeconds);
    for (const auto& store : std::as_const(stores_)) {
        // Only the path and the tool cross to the store's queue.
        store->worker->runOnStore([path = store->path, tool = tool_, expireBefore] {
            (void)ketplus::runner::snapshot::pruneStore(path, expireBefore, tool);
        });
    }
}

} // namespace ketplus::daemon::runner::remote
