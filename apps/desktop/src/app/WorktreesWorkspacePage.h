#pragma once

#include "git/GitTypes.h"

#include <QHash>
#include <QSet>
#include <QVector>
#include <QWidget>

class QVBoxLayout;
class QAbstractButton;

namespace ketplus {

struct ThemePalette;

class WorktreesWorkspacePage final : public QWidget {
    Q_OBJECT

  public:
    explicit WorktreesWorkspacePage(QWidget* parent = nullptr);

    void setWorktrees(const QVector<GitWorktree>& worktrees, const QString& activePath,
                      const QSet<QString>& runningPaths,
                      const QHash<QString, int>& conversationCounts, bool activeDirty);
    // A linked worktree's own Git state, once it has been read.
    void setWorktreeDirty(const QString& path, bool dirty);
    // Its Git state could not be read: show none rather than an old answer.
    void clearWorktreeState(const QString& path);
    void setPruneAvailable(bool available);
    void setPruning(bool pruning);
    void applyPalette(const ThemePalette& palette, int zoomPercent = 100);

  signals:
    void createRequested();
    void pruneRequested();
    void worktreeRequested(const QString& path);
    // `menuPosition` is the global point under the row's Manage button.
    void manageRequested(const QString& path, const QPoint& menuPosition);
    void archivedRequested();

  private:
    void rebuild();

    QVBoxLayout* listLayout_{nullptr};
    QAbstractButton* pruneButton_{nullptr};
    QVector<GitWorktree> worktrees_;
    QSet<QString> runningPaths_;
    QHash<QString, int> conversationCounts_;
    QHash<QString, bool> dirtyPaths_;
    QString activePath_;
    QString pageBackground_;
    QString panelBackground_;
    QString textMain_;
    QString textMuted_;
    QString border_;
    QString surfaceRaised_;
    QString positive_;
    QString warning_;
    bool pruneAvailable_{false};
    bool pruning_{false};
    bool activeDirty_{false};
};

} // namespace ketplus
