#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

namespace ketplus {

// Whether each linked worktree has uncommitted changes, for the Worktrees page. One read-only
// `git status` per worktree, off the event loop, at most once every few seconds per path.
class WorktreeStateProbe final : public QObject {
    Q_OBJECT

  public:
    explicit WorktreeStateProbe(QObject* parent = nullptr);

    void probe(const QStringList& paths);

  signals:
    void stateKnown(const QString& path, bool dirty);

  private:
    QSet<QString> running_;
    QHash<QString, qint64> probedAt_;
    QElapsedTimer clock_;
};

} // namespace ketplus
