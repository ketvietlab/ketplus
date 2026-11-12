#pragma once

#include "storage/LocalJournal.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <functional>

namespace ketplus::daemon {

// The workspace revision each heartbeat publishes for a worktree (a hash of HEAD, the status and
// dirty content). Computing it runs git twice per worktree, which for every stored conversation
// every heartbeat kept a daemon thread busy without end.
//
// A revision is computed again only when something that can change it moved: a conversation on
// the worktree recorded an event or changed state, git's HEAD or index changed, or the revision is
// older than maximumAgeMs (edits made outside any conversation). Publishing a slightly older
// revision is safe: the relay accepts any revision published in the last ten minutes and reads are
// served from the worktree as it is now.
class WorktreeRevisionCache final {
  public:
    using Compute = std::function<QString(const QString& worktreePath)>;

    static constexpr qint64 maximumAgeMs = 2 * 60 * 1'000;

    // The revision of the worktree the given conversations use, computed with `compute` only
    // when the cached one may be out of date.
    QString revision(const QString& worktreePath, const QList<StoredSession>& sessions,
                     qint64 nowMs, const Compute& compute);

    // Forgets worktrees no stored conversation uses any more.
    void retain(const QList<StoredSession>& stored);

    [[nodiscard]] qsizetype size() const { return entries_.size(); }

    // What a cached revision depends on besides time; exposed for tests.
    static QByteArray fingerprint(const QString& worktreePath,
                                  const QList<StoredSession>& sessions);

  private:
    struct Entry {
        QByteArray fingerprint;
        QString revision;
        qint64 computedAtMs{0};
    };
    QHash<QString, Entry> entries_;
};

} // namespace ketplus::daemon
