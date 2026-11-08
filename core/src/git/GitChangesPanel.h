#pragma once

#include "git/GitTypes.h"

#include <QWidget>

class QVBoxLayout;
class QLabel;
class QLineEdit;
class QAbstractButton;
namespace ket {
class Banner;
}
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace ketplus {

class GitChangesPanel final : public QWidget {
    Q_OBJECT

  public:
    explicit GitChangesPanel(QWidget* parent = nullptr);

    void setSnapshot(const GitSnapshot& snapshot);
    void focusChanges();
    void setMutationRunning(bool running);
    void mutationFinished(const QString& operation, bool ok, const QString& error);

  signals:
    void diffRequested(const QString& path, ketplus::GitDiffMode mode);
    void fileOpenRequested(const QString& path);
    void refreshRequested();
    void stageRequested(const QString& path);
    void unstageRequested(const QString& path);
    void commitRequested(const QString& message);
    void hideRequested();

  private:
    void addGroup(const QString& title, const QVector<const GitFileStatus*>& files,
                  GitDiffMode mode, const QString& repositoryRoot);
    void showContextMenu(const QPoint& position);
    void requestItemDiff(QTreeWidgetItem* item);
    void openItem(QTreeWidgetItem* item);

    void createCommitControls(QVBoxLayout* layout);
    void updateCommitEnabled();
    QLineEdit* commitMessage_{nullptr};
    QAbstractButton* commitButton_{nullptr};
    ket::Banner* mutationNotice_{nullptr};
    bool mutationRunning_{false};
    bool hasStagedChanges_{false};
    QLabel* repositoryLabel_{nullptr};
    QLabel* branchLabel_{nullptr};
    QWidget* repositorySummary_{nullptr};
    QLabel* emptyLabel_{nullptr};
    QStackedWidget* pages_{nullptr};
    QTreeWidget* tree_{nullptr};
};

} // namespace ketplus
