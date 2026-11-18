#pragma once

#include "runner/sync/SnapshotSyncPorts.h"

#include <QDateTime>
#include <QSet>
#include <QString>

#include <optional>

// The daemon's packs waiting to be sent, one per job, in a single directory. Each pack has a
// side file holding its job, snapshot, base, size and SHA-256, so a pack survives a restart.
// Packs of jobs in flight are never removed by sweep(); every other pack goes once it is older
// than maxAge, and the oldest go first while the cache is above maxBytes.
namespace ketplus::daemon::runner::sync {

struct PackCacheLimits {
    qint64 maxBytes = 8LL * 1024 * 1024 * 1024;
    qint64 maxAgeSeconds = 24 * 60 * 60;
};

class PackCache {
  public:
    explicit PackCache(QString directory, PackCacheLimits limits = {});

    // Where the pack of this job, snapshot and base is written; the same for the same inputs.
    [[nodiscard]] QString pathFor(const QString& jobId, const QString& snapshotId,
                                  const QString& baseId) const;

    // Records a pack written at pathFor(); false when the side file could not be written.
    bool record(const QString& jobId, const PackFile& pack);
    // The job's recorded pack whose file still has the recorded size.
    [[nodiscard]] std::optional<PackFile> find(const QString& jobId) const;
    // Deletes the job's pack and side file.
    void remove(const QString& jobId);

    void setInFlight(const QString& jobId, bool inFlight);
    [[nodiscard]] bool inFlight(const QString& jobId) const;

    // Applies the age and size limits as of `now`.
    void sweep(const QDateTime& now);
    // Bytes of every file in the cache directory.
    [[nodiscard]] qint64 totalBytes() const;

  private:
    [[nodiscard]] QString stem(const QString& jobId) const;

    QString directory_;
    PackCacheLimits limits_;
    QSet<QString> inFlight_;
};

} // namespace ketplus::daemon::runner::sync
