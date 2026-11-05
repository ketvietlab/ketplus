#pragma once

#include <QModelIndex>
#include <QWidget>

#include <functional>

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
    // Expands the folders down to `path` and selects it. Folders load in the background, so
    // a path whose folders are not there yet is revealed once they arrive.
    void revealPath(const QString& path);
    void focusTree();
    // How many open tabs with unsaved edits are at or under a path, so Move to Trash can say.
    void setUnsavedTabsQuery(std::function<int(const QString& path)> query);

  signals:
    void fileOpenRequested(const QString& path);
    // After an inline rename. Tabs at or under `from` now belong at `to`.
    void pathRenamed(const QString& from, const QString& to);
    // After a file or folder went to the Trash.
    void pathTrashed(const QString& path);
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
    bool tryRevealPath(const QString& path);
    [[nodiscard]] QString selectedPath() const;
    [[nodiscard]] QString targetDirectory() const;

    QFileSystemModel* model_{nullptr};
    QTreeView* tree_{nullptr};
    QStackedWidget* pages_{nullptr};
    QWidget* emptyPage_{nullptr};
    QLabel* folderLabel_{nullptr};
    QString rootPath_;
    QString pendingReveal_;
    bool showHiddenFiles_{false};
    std::function<int(const QString& path)> unsavedTabs_;
};

} // namespace ketplus
