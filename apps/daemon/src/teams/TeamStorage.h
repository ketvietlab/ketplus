#pragma once
// ADR 0114 §10: the disk under a team's worktrees, and the worktrees of its done tasks.
#include <QHash>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <optional>

namespace ketplus::daemon::teams {

namespace storage {
// Below this many bytes free (decimal GB, as the Finder counts them) the team page warns.
inline constexpr qint64 warnBytes = 20LL * 1000 * 1000 * 1000;
// Below this many bytes free the team page says builds should stop. Nothing is paused or
// removed automatically at either level.
inline constexpr qint64 stopBytes = 12LL * 1000 * 1000 * 1000;

// "stop", "low" or "" for enough space; "" when the free space is unknown.
[[nodiscard]] QString level(std::optional<qint64> freeBytes);
// Why a conversation keeps its folder from being removed, in words after its title; empty
// when it is idle. One that waits for an answer or an approval counts as running.
[[nodiscard]] QString busy(const QString& state, bool idle, bool approvalsPending);
} // namespace storage

// A conversation whose folder is in a worktree.
struct FolderConversation final {
    QString sessionId;
    QString title;
};

// What the host measured: the free space of the volume, nothing when it could not tell, and
// the size on disk of each worktree it could read, by path.
struct StorageFacts final {
    std::optional<qint64> freeBytes;
    QHash<QString, qint64> sizes;
};
using StorageDone = std::function<void(const StorageFacts&)>;
// The error of removing a worktree, in words; empty when it was removed.
using RemovalDone = std::function<void(const QString&)>;

} // namespace ketplus::daemon::teams
