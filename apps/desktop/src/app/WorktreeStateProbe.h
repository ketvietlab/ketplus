#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

namespace ketplus {

// Whether each linked worktree has uncommitted changes, for the Worktrees page. One read-only
// `git status` per worktree, off the event loop, at most once every few seconds per path. A
// request made while one is running or too soon after the last is read once that is possible.
class WorktreeStateProbe final : public QObject {
    Q_OBJECT

  public:
    explicit WorktreeStateProbe(QObject* parent = nullptr);

    void probe(const QStringList& paths);

  signals:
    void stateKnown(const QString& path, bool dirty);
    // The read failed: whatever was known about the worktree may no longer be true.
    void stateUnknown(const QString& path);

  private:
    void request(const QString& path);
    void start(const QString& path);

    QSet<QString> running_;
    QSet<QString> pending_;
    QHash<QString, qint64> probedAt_;
    QElapsedTimer clock_;
};

} // namespace ketplus
