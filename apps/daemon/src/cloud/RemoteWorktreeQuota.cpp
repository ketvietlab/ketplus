#include "cloud/RemoteWorktreeQuota.h"

namespace ketplus::daemon {

QString RemoteWorktreeQuota::reserve(const QString& repository, const qint64 nowMs,
                                     const qint64 freeBytes) {
    constexpr qint64 hour = 60 * 60 * 1000;
    if (freeBytes >= 0 && freeBytes < minimumFreeBytes)
        return QStringLiteral("This Mac is low on disk space for a new worktree.");
    QList<qint64>& created = created_[repository];
    created.removeIf([nowMs](const qint64 at) { return nowMs - at >= hour; });
    if (created.size() >= perHour)
        return QStringLiteral("Too many worktrees were created in this repository in the last "
                              "hour. Try again later.");
    created.append(nowMs);
    return {};
}

} // namespace ketplus::daemon
