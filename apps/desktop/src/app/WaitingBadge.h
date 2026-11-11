#pragma once

#include <QHash>
#include <QString>
#include <QVariantList>

class QObject;

namespace ketplus {

// The Dock badge counts conversations waiting for the user (#125), not unread ones. Every
// window reports per worktree; a worktree shown in two windows counts once.
class WaitingBadge {
  public:
    static WaitingBadge& instance();

    static int countWaiting(const QVariantList& conversations);
    void report(QObject* owner, const QString& worktreePath, int waiting);
    void forget(QObject* owner);
    int total() const;

  private:
    void apply();

    QHash<QString, QHash<QObject*, int>> waiting_;
    int applied_{-1};
};

} // namespace ketplus
