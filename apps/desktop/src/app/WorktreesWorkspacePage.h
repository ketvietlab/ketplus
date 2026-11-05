#pragma once

#include "git/GitTypes.h"

#include <QHash>
#include <QSet>
#include <QVector>
#include <QWidget>

class QVBoxLayout;
class QPushButton;

namespace ketplus {

struct ThemePalette;

class WorktreesWorkspacePage final : public QWidget {
    Q_OBJECT

  public:
    explicit WorktreesWorkspacePage(QWidget* parent = nullptr);

    void setWorktrees(const QVector<GitWorktree>& worktrees, const QString& activePath,
                      const QSet<QString>& runningPaths,
                      const QHash<QString, int>& conversationCounts, bool activeDirty);
    void setPruneAvailable(bool available);
    void setPruning(bool pruning);
    void applyPalette(const ThemePalette& palette, int interfaceFontSizePixels = 14);

  signals:
    void createRequested();
    void pruneRequested();
    void worktreeRequested(const QString& path);
    void manageRequested(const QString& path);
    void archivedRequested();

  private:
    void rebuild();

    QVBoxLayout* listLayout_{nullptr};
    QPushButton* pruneButton_{nullptr};
    QVector<GitWorktree> worktrees_;
    QSet<QString> runningPaths_;
    QHash<QString, int> conversationCounts_;
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
