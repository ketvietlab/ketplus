#include "runner/sync/StoreRetention.h"

#include "snapshot/SnapshotStore.h"

#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <utility>

namespace ketplus::daemon::runner::sync {

namespace snapshot = ketplus::runner::snapshot;

namespace {

Q_LOGGING_CATEGORY(lcRetention, "ketplus.runner.sync")

const QString confirmedPrefix = QStringLiteral("confirmed/");

QString digestOf(const QString& id) {
    return QString::fromLatin1(
        QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
}

bool isWorkspaceKey(const QString& key) {
    static const QRegularExpression form(QStringLiteral("^[A-Za-z0-9_-]{32}$"));
    return form.match(key).hasMatch();
}

} // namespace

StoreRetention::StoreRetention(QString storePath, PackCache& packs, snapshot::GitTool tool)
    : storePath_(std::move(storePath)), packs_(packs), tool_(std::move(tool)) {}

bool StoreRetention::load() {
    const auto refs = snapshot::listRefs(storePath_, tool_);
    if (!refs)
        return false;
    confirmed_.clear();
    for (auto it = refs.value().cbegin(); it != refs.value().cend(); ++it) {
        if (it.key().startsWith(confirmedPrefix))
            confirmed_.insert(it.key(), it.value());
    }
    return true;
}

QString StoreRetention::inFlightRef(const QString& jobId) {
    return QStringLiteral("inflight/") + digestOf(jobId);
}

QString StoreRetention::confirmedRef(const QString& runnerId, const QString& workspaceKey) {
    return confirmedPrefix + digestOf(runnerId) + QLatin1Char('/') + workspaceKey;
}

std::optional<QString> StoreRetention::confirmed(const QString& runnerId,
                                                 const QString& workspaceKey) const {
    const auto found = confirmed_.constFind(confirmedRef(runnerId, workspaceKey));
    if (found == confirmed_.cend())
        return std::nullopt;
    return *found;
}

bool StoreRetention::confirm(const QString& runnerId, const QString& workspaceKey,
                             const QString& snapshotId) {
    if (!isWorkspaceKey(workspaceKey))
        return false;
    const QString ref = confirmedRef(runnerId, workspaceKey);
    if (!snapshot::keepRef(storePath_, ref, snapshotId, tool_)) {
        qCWarning(lcRetention) << "could not keep a confirmed snapshot";
        return false;
    }
    confirmed_.insert(ref, snapshotId);
    return true;
}

void StoreRetention::forget(const QString& runnerId, const QString& workspaceKey) {
    if (!isWorkspaceKey(workspaceKey))
        return;
    const QString ref = confirmedRef(runnerId, workspaceKey);
    confirmed_.remove(ref);
    if (!snapshot::dropRef(storePath_, ref, tool_))
        qCWarning(lcRetention) << "could not drop a confirmed snapshot";
}

std::optional<PackFile> StoreRetention::pack(const QString& jobId) const {
    return packs_.find(jobId);
}

void StoreRetention::keepPack(const QString& jobId, const PackFile& pack) {
    packs_.setInFlight(jobId, true);
    if (!packs_.record(jobId, pack))
        qCWarning(lcRetention) << "could not record a pack for job" << jobId;
}

void StoreRetention::dropPack(const QString& jobId) { packs_.remove(jobId); }

void StoreRetention::release(const QString& jobId) {
    packs_.setInFlight(jobId, false);
    if (!dropJobSnapshot(storePath_, packs_, jobId, tool_))
        qCWarning(lcRetention) << "could not release the snapshot of job" << jobId;
}

void StoreRetention::releaseRunner(const QString& runnerId) {
    const QString prefix = confirmedPrefix + digestOf(runnerId) + QLatin1Char('/');
    for (auto it = confirmed_.begin(); it != confirmed_.end();) {
        if (!it.key().startsWith(prefix)) {
            ++it;
            continue;
        }
        if (!snapshot::dropRef(storePath_, it.key(), tool_))
            qCWarning(lcRetention) << "could not drop a confirmed snapshot";
        it = confirmed_.erase(it);
    }
}

bool StoreRetention::prune(const QDateTime& expireBefore) {
    return snapshot::pruneStore(storePath_, expireBefore, tool_).ok();
}

bool dropJobSnapshot(const QString& storePath, PackCache& packs, const QString& jobId,
                     const snapshot::GitTool& tool) {
    packs.setInFlight(jobId, false);
    packs.remove(jobId);
    return snapshot::dropRef(storePath, StoreRetention::inFlightRef(jobId), tool).ok();
}

} // namespace ketplus::daemon::runner::sync
