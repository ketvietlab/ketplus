#pragma once

#include <QModelIndex>
#include <QWidget>

class QFileSystemModel;
class QLabel;
class QStackedWidget;
class QTreeView;

namespace ketplus {

class ExplorerPanel final : public QWidget {
    Q_OBJECT

  public:
    explicit ExplorerPanel(QWidget* parent = nullptr);

    [[nodiscard]] QString rootPath() const;
    void setRootPath(const QString& path);
    void focusTree();

  signals:
    void fileOpenRequested(const QString& path);
    void gitDiffRequested(const QString& path);
    void openFolderRequested();
    void hideRequested();

  private:
    void activateIndex(const QModelIndex& index);
    void showContextMenu(const QPoint& position);
    void createEntry(bool directory);
    void renameSelected();
    void moveSelectedToTrash();
    void openContainingFolder();
    void copySelectedPath();
    void setShowHiddenFiles(bool show);
    void refresh();
    [[nodiscard]] QString selectedPath() const;
    [[nodiscard]] QString targetDirectory() const;

    QFileSystemModel* model_{nullptr};
    QTreeView* tree_{nullptr};
    QStackedWidget* pages_{nullptr};
    QWidget* emptyPage_{nullptr};
    QLabel* folderLabel_{nullptr};
    QString rootPath_;
    bool showHiddenFiles_{false};
};

} // namespace ketplus
