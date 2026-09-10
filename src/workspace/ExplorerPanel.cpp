#include "workspace/ExplorerPanel.h"

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
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QDir::Filters explorerFilters(const bool showHiddenFiles) {
    auto filters = QDir::AllEntries | QDir::AllDirs | QDir::NoDotAndDotDot;
    if (showHiddenFiles) {
        filters |= QDir::Hidden | QDir::System;
    }
    return filters;
}

bool isValidEntryName(const QString& name) {
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..") &&
           !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'));
}

} // namespace

ExplorerPanel::ExplorerPanel(QWidget* parent)
    : QWidget(parent), model_(new QFileSystemModel(this)), tree_(new QTreeView(this)),
      pages_(new QStackedWidget(this)), emptyPage_(new QWidget(this)),
      folderLabel_(new QLabel(this)) {
    setProperty("kvRole", QStringLiteral("explorer"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(190);

    model_->setReadOnly(false);
    model_->setFilter(explorerFilters(false));
    model_->setRootPath(QString());
    model_->iconProvider()->setOptions(QFileIconProvider::DontUseCustomDirectoryIcons);

    tree_->setObjectName(QStringLiteral("explorerTree"));
    tree_->setModel(model_);
    tree_->setHeaderHidden(true);
    tree_->setAnimated(false);
    tree_->setExpandsOnDoubleClick(false);
    tree_->setIndentation(14);
    tree_->setUniformRowHeights(true);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    for (int column = 1; column < model_->columnCount(); ++column) {
        tree_->hideColumn(column);
    }

    auto* heading = new QLabel(QStringLiteral("EXPLORER"), this);
    heading->setProperty("kvRole", QStringLiteral("sidebarHeading"));
    folderLabel_->setProperty("kvRole", QStringLiteral("sidebarFolder"));
    folderLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* collapseButton = new QToolButton(this);
    collapseButton->setText(QString::fromUtf8("⌃"));
    collapseButton->setProperty("kvRole", QStringLiteral("sidebarAction"));
    collapseButton->setToolTip(QStringLiteral("Collapse folders"));
    collapseButton->setAccessibleName(QStringLiteral("Collapse folders"));

    auto* refreshButton = new QToolButton(this);
    refreshButton->setText(QString::fromUtf8("↻"));
    refreshButton->setProperty("kvRole", QStringLiteral("sidebarAction"));
    refreshButton->setToolTip(QStringLiteral("Refresh Explorer"));
    refreshButton->setAccessibleName(QStringLiteral("Refresh Explorer"));

    auto* closeButton = new QToolButton(this);
    closeButton->setText(QString::fromUtf8("×"));
    closeButton->setProperty("kvRole", QStringLiteral("sidebarAction"));
    closeButton->setToolTip(QStringLiteral("Hide Explorer"));
    closeButton->setAccessibleName(QStringLiteral("Hide Explorer"));

    auto* headerLayout = new QHBoxLayout;
    headerLayout->setContentsMargins(10, 6, 6, 6);
    headerLayout->setSpacing(4);
    headerLayout->addWidget(heading);
    headerLayout->addWidget(folderLabel_, 1);
    headerLayout->addWidget(collapseButton);
    headerLayout->addWidget(refreshButton);
    headerLayout->addWidget(closeButton);

    auto* emptyLabel = new QLabel(QStringLiteral("No folder open"), emptyPage_);
    emptyLabel->setAlignment(Qt::AlignCenter);
    emptyLabel->setProperty("kvRole", QStringLiteral("emptyTitle"));
    auto* openButton = new QPushButton(QStringLiteral("Open Folder"), emptyPage_);
    auto* emptyLayout = new QVBoxLayout(emptyPage_);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->addStretch();
    emptyLayout->addWidget(emptyLabel);
    emptyLayout->addSpacing(8);
    emptyLayout->addWidget(openButton, 0, Qt::AlignHCenter);
    emptyLayout->addStretch();

    pages_->addWidget(emptyPage_);
    pages_->addWidget(tree_);
    pages_->setCurrentWidget(emptyPage_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(headerLayout);
    layout->addWidget(pages_, 1);

    connect(tree_, &QTreeView::clicked, this, &ExplorerPanel::activateIndex);
    connect(tree_, &QTreeView::customContextMenuRequested, this, &ExplorerPanel::showContextMenu);
    connect(openButton, &QPushButton::clicked, this, &ExplorerPanel::openFolderRequested);
    connect(collapseButton, &QToolButton::clicked, tree_, &QTreeView::collapseAll);
    connect(refreshButton, &QToolButton::clicked, this, &ExplorerPanel::refresh);
    connect(closeButton, &QToolButton::clicked, this, &ExplorerPanel::hideRequested);
}

QString ExplorerPanel::rootPath() const { return rootPath_; }

void ExplorerPanel::setRootPath(const QString& path) {
    const QFileInfo root(path);
    if (!root.isDir()) {
        return;
    }

    rootPath_ = root.absoluteFilePath();
    folderLabel_->setText(root.fileName().isEmpty() ? rootPath_ : root.fileName());
    folderLabel_->setToolTip(rootPath_);
    tree_->setRootIndex(model_->setRootPath(rootPath_));
    pages_->setCurrentWidget(tree_);
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
    auto* refreshAction = menu.addAction(QStringLiteral("Refresh"));
    connect(refreshAction, &QAction::triggered, this, &ExplorerPanel::refresh);

    menu.exec(tree_->viewport()->mapToGlobal(position));
}

void ExplorerPanel::createEntry(const bool directory) {
    const QString parentPath = targetDirectory();
    if (parentPath.isEmpty()) {
        return;
    }

    bool accepted = false;
    const QString title = directory ? QStringLiteral("New Folder") : QStringLiteral("New File");
    const QString name = QInputDialog::getText(this, title, QStringLiteral("Name:"),
                                               QLineEdit::Normal, {}, &accepted)
                             .trimmed();
    if (!accepted) {
        return;
    }
    if (!isValidEntryName(name)) {
        QMessageBox::warning(this, title, QStringLiteral("Enter a valid name without slashes."));
        return;
    }

    const QString path = QDir(parentPath).filePath(name);
    if (QFileInfo::exists(path)) {
        QMessageBox::warning(this, title, QStringLiteral("An item with this name already exists."));
        return;
    }

    bool created = false;
    if (directory) {
        created = QDir(parentPath).mkdir(name);
    } else {
        QFile file(path);
        created = file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
    }
    if (!created) {
        QMessageBox::critical(this, title, QStringLiteral("Unable to create %1.").arg(path));
        return;
    }

    QTimer::singleShot(100, this, [this, path, directory] {
        const QModelIndex index = model_->index(path);
        if (index.isValid()) {
            tree_->setCurrentIndex(index);
            tree_->scrollTo(index);
            if (!directory) {
                emit fileOpenRequested(path);
            }
        }
    });
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
    const auto answer =
        QMessageBox::question(this, QStringLiteral("Move to Trash"),
                              QStringLiteral("Move “%1” to Trash?").arg(QFileInfo(path).fileName()),
                              QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    if (!QFile::moveToTrash(path)) {
        QMessageBox::critical(this, QStringLiteral("Move to Trash"),
                              QStringLiteral("Unable to move this item to Trash."));
    }
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
