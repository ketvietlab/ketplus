#include "workspace/ExplorerPanel.h"

#include "KetButton.h"
#include "KetChrome.h"
#include "KetEmptyState.h"
#include "KetFileRow.h"
#include "KetIcons.h"
#include "ui/MessageDialog.h"
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {
namespace {

QDir::Filters explorerFilters(const bool showHiddenFiles) {
    auto filters = QDir::AllEntries | QDir::AllDirs | QDir::NoDotAndDotDot;
    if (showHiddenFiles) {
        filters |= QDir::Hidden | QDir::System;
    }
    return filters;
}

} // namespace

ExplorerPanel::ExplorerPanel(QWidget* parent)
    : QWidget(parent), model_(new QFileSystemModel(this)), tree_(new QTreeView(this)),
      pages_(new QStackedWidget(this)), emptyPage_(new QWidget(this)),
      rootRow_(new ket::FileRow({}, ket::FileRow::Kind::Folder, 0, this)) {
    setProperty("kvRole", QStringLiteral("explorer"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(190);

    model_->setReadOnly(false);
    connect(model_, &QFileSystemModel::fileRenamed, this,
            [this](const QString& directory, const QString& oldName, const QString& newName) {
                emit pathRenamed(QDir(directory).filePath(oldName),
                                 QDir(directory).filePath(newName));
            });
    model_->setFilter(explorerFilters(false));
    model_->setRootPath(QString());
    model_->iconProvider()->setOptions(QFileIconProvider::DontUseCustomDirectoryIcons);

    tree_->setObjectName(QStringLiteral("explorerTree"));
    tree_->setModel(model_);
    installRowDelegate();
    tree_->setHeaderHidden(true);
    tree_->setAnimated(false);
    tree_->setExpandsOnDoubleClick(false);
    // The delegate draws the kit's indent and chevrons (ExplorerPanelRows.cpp).
    tree_->setIndentation(0);
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    for (int column = 1; column < model_->columnCount(); ++column) {
        tree_->hideColumn(column);
    }

    // As the mock's EXPLORER pane: refresh in the header; New File, New Folder and Collapse
    // Folders are in the context menu, and the activity bar hides the pane.
    auto* header = new ket::PaneHeader(tr("EXPLORER"), {}, this);
    auto* refreshButton = new ket::IconButton(
        ket::Icons::icon(ket::Icons::codeForLucide(u"refresh-cw")), tr("Refresh Explorer"), header);
    header->actions()->addWidget(refreshButton);
    // The open folder is the tree's expanded root row; unchecking it folds the tree away.
    rootRow_->setObjectName(QStringLiteral("explorerRoot"));
    rootRow_->setChecked(true);
    rootRow_->setContextMenuPolicy(Qt::CustomContextMenu);
    rootRow_->hide();
    auto* emptyState =
        new ket::EmptyState(ket::Icons::codeForLucide(u"folder"), tr("No folder open"),
                            tr("Open a folder to browse files."), emptyPage_);
    auto* openButton = emptyState->setAction(tr("Open Folder"));
    auto* emptyLayout = new QVBoxLayout(emptyPage_);
    emptyLayout->setContentsMargins(0, 0, 0, 0);
    emptyLayout->addWidget(emptyState);

    pages_->addWidget(emptyPage_);
    pages_->addWidget(tree_);
    pages_->setCurrentWidget(emptyPage_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(rootRow_);
    layout->addWidget(pages_, 1);

    connect(tree_, &QTreeView::clicked, this, &ExplorerPanel::activateIndex);
    connect(tree_, &QTreeView::customContextMenuRequested, this, &ExplorerPanel::showContextMenu);
    connect(openButton, &QAbstractButton::clicked, this, &ExplorerPanel::openFolderRequested);
    connect(refreshButton, &QAbstractButton::clicked, this, &ExplorerPanel::refresh);
    connect(rootRow_, &QAbstractButton::toggled, tree_, &QWidget::setVisible);
    connect(rootRow_, &QWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        // The folder itself: no row of the tree is the target.
        tree_->setCurrentIndex({});
        showContextMenu(tree_->viewport()->mapFrom(rootRow_, position));
    });
    connect(model_, &QFileSystemModel::directoryLoaded, this, [this](const QString&) {
        if (!pendingReveal_.isEmpty() && tryRevealPath(pendingReveal_)) {
            pendingReveal_.clear();
        }
    });
}

QString ExplorerPanel::rootPath() const { return rootPath_; }

void ExplorerPanel::setRootPath(const QString& path) {
    const QFileInfo root(path);
    if (!root.isDir()) {
        return;
    }

    if (entryRow_)
        entryRow_->hide();
    rootPath_ = root.absoluteFilePath();
    pendingReveal_.clear();
    rootRow_->setText((root.fileName().isEmpty() ? rootPath_ : root.fileName()).toUpper());
    rootRow_->setAccessibleName(root.fileName().isEmpty() ? rootPath_ : root.fileName());
    rootRow_->setToolTip(rootPath_);
    rootRow_->setChecked(true);
    rootRow_->show();
    tree_->setRootIndex(model_->setRootPath(rootPath_));
    pages_->setCurrentWidget(tree_);
}

void ExplorerPanel::revealPath(const QString& path) {
    pendingReveal_.clear();
    if (path.isEmpty() || rootPath_.isEmpty()) {
        return;
    }
    const QString target = QFileInfo(path).absoluteFilePath();
    if (target != rootPath_ && !target.startsWith(rootPath_ + QLatin1Char('/'))) {
        // A file outside the open folder has nothing to select here.
        return;
    }
    if (!tryRevealPath(target)) {
        // The folders on the way are still loading; directoryLoaded finishes the job.
        pendingReveal_ = target;
    }
}

bool ExplorerPanel::tryRevealPath(const QString& path) {
    // Ask for each folder from the root down, so the model starts loading them in order.
    const QString relative = QDir(rootPath_).relativeFilePath(path);
    QString walked = rootPath_;
    for (const QString& part : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        walked += QLatin1Char('/') + part;
        const QModelIndex index = model_->index(walked);
        if (!index.isValid()) {
            return false;
        }
        if (walked != path) {
            tree_->setExpanded(index, true);
        }
    }

    const QModelIndex index = model_->index(path);
    if (!index.isValid()) {
        return false;
    }
    const QSignalBlocker blocker(tree_->selectionModel());
    tree_->setCurrentIndex(index);
    tree_->scrollTo(index, QAbstractItemView::EnsureVisible);
    return true;
}

void ExplorerPanel::focusTree() {
    tree_->setFocus();
    if (!tree_->currentIndex().isValid() && tree_->model()->rowCount(tree_->rootIndex()) > 0) {
        tree_->setCurrentIndex(tree_->model()->index(0, 0, tree_->rootIndex()));
    }
}

void ExplorerPanel::activateIndex(const QModelIndex& index) {
    const QFileInfo entry(model_->filePath(index));
    if (entry.isDir()) {
        tree_->setExpanded(index, !tree_->isExpanded(index));
    } else if (entry.isFile()) {
        emit fileOpenRequested(entry.absoluteFilePath());
    }
}

void ExplorerPanel::showContextMenu(const QPoint& position) {
    const QModelIndex index = tree_->indexAt(position);
    if (index.isValid()) {
        tree_->setCurrentIndex(index);
    }
    const QString path = selectedPath();
    const QFileInfo entry(path);

    QMenu menu(this);
    if (entry.isFile()) {
        auto* openAction = menu.addAction(QStringLiteral("Open"));
        connect(openAction, &QAction::triggered, this, [this, index] { activateIndex(index); });
        auto* diffAction = menu.addAction(QStringLiteral("View Git Diff"));
        connect(diffAction, &QAction::triggered, this,
                [this, path] { emit gitDiffRequested(path); });
        menu.addSeparator();
    }

    auto* newFileAction = menu.addAction(QStringLiteral("New File…"));
    connect(newFileAction, &QAction::triggered, this, [this] { createEntry(false); });
    auto* newFolderAction = menu.addAction(QStringLiteral("New Folder…"));
    connect(newFolderAction, &QAction::triggered, this, [this] { createEntry(true); });

    if (index.isValid() && path != rootPath_) {
        menu.addSeparator();
        auto* renameAction = menu.addAction(QStringLiteral("Rename"));
        connect(renameAction, &QAction::triggered, this, &ExplorerPanel::renameSelected);
        auto* trashAction = menu.addAction(QStringLiteral("Move to Trash"));
        connect(trashAction, &QAction::triggered, this, &ExplorerPanel::moveSelectedToTrash);
    }

    menu.addSeparator();
    auto* containingFolderAction = menu.addAction(QStringLiteral("Open Containing Folder"));
    connect(containingFolderAction, &QAction::triggered, this,
            &ExplorerPanel::openContainingFolder);
    auto* copyPathAction = menu.addAction(QStringLiteral("Copy Path"));
    connect(copyPathAction, &QAction::triggered, this, &ExplorerPanel::copySelectedPath);

    menu.addSeparator();
    auto* hiddenAction = menu.addAction(QStringLiteral("Show Hidden Files"));
    hiddenAction->setCheckable(true);
    hiddenAction->setChecked(showHiddenFiles_);
    connect(hiddenAction, &QAction::toggled, this, &ExplorerPanel::setShowHiddenFiles);
    auto* collapseAction = menu.addAction(QStringLiteral("Collapse Folders"));
    connect(collapseAction, &QAction::triggered, tree_, &QTreeView::collapseAll);
    auto* refreshAction = menu.addAction(QStringLiteral("Refresh"));
    connect(refreshAction, &QAction::triggered, this, &ExplorerPanel::refresh);

    menu.exec(tree_->viewport()->mapToGlobal(position));
}

void ExplorerPanel::renameSelected() {
    const QModelIndex index = tree_->currentIndex();
    if (index.isValid() && model_->filePath(index) != rootPath_) {
        tree_->edit(index);
    }
}

void ExplorerPanel::moveSelectedToTrash() {
    const QString path = selectedPath();
    if (path.isEmpty() || path == rootPath_) {
        return;
    }
    QString question = QStringLiteral("Move “%1” to Trash?").arg(QFileInfo(path).fileName());
    if (const int unsaved = unsavedTabs_ ? unsavedTabs_(path) : 0; unsaved > 0) {
        question +=
            QStringLiteral("\n\n%1 open %2 unsaved changes. %3 open, and saving "
                           "writes the file again.")
                .arg(unsaved)
                .arg(unsaved == 1 ? QStringLiteral("tab has") : QStringLiteral("tabs have"))
                .arg(unsaved == 1 ? QStringLiteral("It stays") : QStringLiteral("They stay"));
    }
    const auto answer =
        message_dialog::question(this, QStringLiteral("Move to Trash"), question,
                                 QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    if (!QFile::moveToTrash(path)) {
        message_dialog::critical(this, QStringLiteral("Move to Trash"),
                                 QStringLiteral("Unable to move this item to Trash."));
        return;
    }
    emit pathTrashed(path);
}

void ExplorerPanel::setUnsavedTabsQuery(std::function<int(const QString& path)> query) {
    unsavedTabs_ = std::move(query);
}

void ExplorerPanel::openContainingFolder() {
    const QFileInfo entry(selectedPath());
    const QString path = entry.isDir() ? entry.absoluteFilePath() : entry.absolutePath();
    if (!path.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

void ExplorerPanel::copySelectedPath() {
    const QString path = selectedPath();
    if (!path.isEmpty()) {
        QApplication::clipboard()->setText(path);
    }
}

void ExplorerPanel::setShowHiddenFiles(const bool show) {
    showHiddenFiles_ = show;
    model_->setFilter(explorerFilters(showHiddenFiles_));
}

void ExplorerPanel::refresh() {
    if (!rootPath_.isEmpty()) {
        tree_->setRootIndex(model_->setRootPath(rootPath_));
        tree_->viewport()->update();
    }
}

QString ExplorerPanel::selectedPath() const {
    const QModelIndex index = tree_->currentIndex();
    return index.isValid() ? model_->filePath(index) : rootPath_;
}

QString ExplorerPanel::targetDirectory() const {
    const QFileInfo entry(selectedPath());
    if (entry.isDir()) {
        return entry.absoluteFilePath();
    }
    if (entry.isFile()) {
        return entry.absolutePath();
    }
    return rootPath_;
}

} // namespace ketplus
