#pragma once

#include <QHash>
#include <QList>
#include <QString>

namespace ketplus::daemon {

// Bounds the worktrees paired devices create on this Mac (#86). Each creation is a full
// checkout beside the repository, so a looping or compromised phone app could otherwise fill
// the disk. Creations are counted per repository (keyed by its Git common directory, which all
// its checkouts share) over the last hour, and none starts while
// the repository's volume is low on space.
class RemoteWorktreeQuota final {
  public:
    static constexpr int defaultPerHour = 10;
    static constexpr qint64 defaultMinimumFreeBytes = qint64{2} << 30;

    // Reserves one creation in repository at nowMs, or returns why it is refused. freeBytes is
    // the space left on the repository's volume, negative when unknown.
    [[nodiscard]] QString reserve(const QString& repository, qint64 nowMs, qint64 freeBytes);

    int perHour{defaultPerHour};
    qint64 minimumFreeBytes{defaultMinimumFreeBytes};

  private:
    QHash<QString, QList<qint64>> created_;
};

} // namespace ketplus::daemon
