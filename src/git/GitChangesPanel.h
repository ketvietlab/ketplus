#pragma once

#include "git/GitTypes.h"

#include <QWidget>

class QLabel;
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

  signals:
    void diffRequested(const QString& path, ketplus::GitDiffMode mode);
    void fileOpenRequested(const QString& path);
    void refreshRequested();
    void hideRequested();

  private:
    void addGroup(const QString& title, const QVector<const GitFileStatus*>& files,
                  GitDiffMode mode, const QString& repositoryRoot);
    void showContextMenu(const QPoint& position);
    void requestItemDiff(QTreeWidgetItem* item);
    void openItem(QTreeWidgetItem* item);

    QLabel* branchLabel_{nullptr};
    QLabel* emptyLabel_{nullptr};
    QStackedWidget* pages_{nullptr};
    QTreeWidget* tree_{nullptr};
};

} // namespace ketplus
