#include "git/GitChangesPanel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ketplus {
namespace {

constexpr int pathRole = Qt::UserRole;
constexpr int diffModeRole = Qt::UserRole + 1;

QString statusLabel(const GitFileStatus& status, const GitDiffMode mode) {
    if (status.untracked) {
        return QStringLiteral("U");
    }

    const QChar code = mode == GitDiffMode::Staged ? status.indexCode : status.worktreeCode;
    if (code == QLatin1Char('?')) {
        return QStringLiteral("U");
    }
    return QString(code);
}

QString displayPath(const GitFileStatus& status) {
    if (!status.originalPath.isEmpty()) {
        return QStringLiteral("%1  ←  %2").arg(status.path, status.originalPath);
    }
    return status.path;
}

} // namespace

GitChangesPanel::GitChangesPanel(QWidget* parent)
    : QWidget(parent), branchLabel_(new QLabel(this)), emptyLabel_(new QLabel(this)),
      pages_(new QStackedWidget(this)), tree_(new QTreeWidget(this)) {
    setProperty("kvRole", QStringLiteral("explorer"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(210);

    auto* heading = new QLabel(QStringLiteral("SOURCE CONTROL"), this);
    heading->setProperty("kvRole", QStringLiteral("sidebarHeading"));
    branchLabel_->setProperty("kvRole", QStringLiteral("sidebarFolder"));
    branchLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* refreshButton = new QToolButton(this);
    refreshButton->setText(QString::fromUtf8("↻"));
    refreshButton->setProperty("kvRole", QStringLiteral("sidebarAction"));
    refreshButton->setToolTip(QStringLiteral("Refresh Git status"));
    refreshButton->setAccessibleName(QStringLiteral("Refresh Git status"));

    auto* closeButton = new QToolButton(this);
    closeButton->setText(QString::fromUtf8("×"));
    closeButton->setProperty("kvRole", QStringLiteral("sidebarAction"));
    closeButton->setToolTip(QStringLiteral("Hide Source Control"));
    closeButton->setAccessibleName(QStringLiteral("Hide Source Control"));

    auto* headerLayout = new QHBoxLayout;
    headerLayout->setContentsMargins(10, 6, 6, 6);
    headerLayout->setSpacing(4);
    headerLayout->addWidget(heading);
    headerLayout->addWidget(branchLabel_, 1);
    headerLayout->addWidget(refreshButton);
    headerLayout->addWidget(closeButton);

    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setProperty("kvRole", QStringLiteral("emptyTitle"));
    emptyLabel_->setWordWrap(true);

    tree_->setProperty("kvRole", QStringLiteral("gitChanges"));
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(true);
    tree_->setIndentation(14);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setColumnCount(2);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);

    pages_->addWidget(emptyLabel_);
    pages_->addWidget(tree_);
    pages_->setCurrentWidget(emptyLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(headerLayout);
    layout->addWidget(pages_, 1);

    connect(refreshButton, &QToolButton::clicked, this, &GitChangesPanel::refreshRequested);
    connect(closeButton, &QToolButton::clicked, this, &GitChangesPanel::hideRequested);
    connect(tree_, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item) { requestItemDiff(item); });
    connect(tree_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item) { openItem(item); });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            &GitChangesPanel::showContextMenu);

    setSnapshot({});
}

void GitChangesPanel::setSnapshot(const GitSnapshot& snapshot) {
    tree_->clear();

    if (!snapshot.isRepository()) {
        branchLabel_->clear();
        emptyLabel_->setText(QStringLiteral("No Git repository"));
        pages_->setCurrentWidget(emptyLabel_);
        return;
    }

    branchLabel_->setText(snapshot.branch.isEmpty() ? snapshot.head.left(8) : snapshot.branch);
    branchLabel_->setToolTip(snapshot.repositoryRoot);

    QVector<const GitFileStatus*> staged;
    QVector<const GitFileStatus*> unstaged;
    staged.reserve(snapshot.files.size());
    unstaged.reserve(snapshot.files.size());
    for (const auto& file : snapshot.files) {
        if (file.hasStagedChange()) {
            staged.append(&file);
        }
        if (file.hasUnstagedChange()) {
            unstaged.append(&file);
        }
    }

    if (staged.isEmpty() && unstaged.isEmpty()) {
        emptyLabel_->setText(QStringLiteral("No changes"));
        pages_->setCurrentWidget(emptyLabel_);
        return;
    }

    addGroup(QStringLiteral("STAGED CHANGES"), staged, GitDiffMode::Staged,
             snapshot.repositoryRoot);
    addGroup(QStringLiteral("CHANGES"), unstaged, GitDiffMode::Unstaged,
             snapshot.repositoryRoot);
    tree_->expandAll();
    pages_->setCurrentWidget(tree_);
}

void GitChangesPanel::focusChanges() {
    tree_->setFocus();
    if (tree_->currentItem() == nullptr && tree_->topLevelItemCount() > 0) {
        auto* group = tree_->topLevelItem(0);
        if (group->childCount() > 0) {
            tree_->setCurrentItem(group->child(0));
        }
    }
}

void GitChangesPanel::addGroup(const QString& title,
                               const QVector<const GitFileStatus*>& files,
                               const GitDiffMode mode, const QString& repositoryRoot) {
    if (files.isEmpty()) {
        return;
    }

    auto* group = new QTreeWidgetItem(tree_);
    group->setText(0, QStringLiteral("%1  %2").arg(title).arg(files.size()));
    group->setFirstColumnSpanned(true);
    group->setFlags(Qt::ItemIsEnabled);
    QFont groupFont = group->font(0);
    groupFont.setBold(true);
    group->setFont(0, groupFont);

    for (const auto* status : files) {
        auto* item = new QTreeWidgetItem(group);
        const QString absolutePath = QDir(repositoryRoot).filePath(status->path);
        item->setText(0, displayPath(*status));
        item->setText(1, statusLabel(*status, mode));
        item->setTextAlignment(1, Qt::AlignCenter);
        item->setData(0, pathRole, absolutePath);
        item->setData(0, diffModeRole, static_cast<int>(mode));
        item->setToolTip(0, absolutePath);
        item->setToolTip(1, mode == GitDiffMode::Staged
                                ? QStringLiteral("Staged change")
                                : QStringLiteral("Working tree change"));
    }
}

void GitChangesPanel::showContextMenu(const QPoint& position) {
    auto* item = tree_->itemAt(position);
    if (item == nullptr || item->parent() == nullptr) {
        return;
    }
    tree_->setCurrentItem(item);

    const QString path = item->data(0, pathRole).toString();
    QMenu menu(this);
    auto* diffAction = menu.addAction(QStringLiteral("View Diff"));
    connect(diffAction, &QAction::triggered, this, [this, item] { requestItemDiff(item); });
    auto* openAction = menu.addAction(QStringLiteral("Open File"));
    openAction->setEnabled(QFileInfo::exists(path));
    connect(openAction, &QAction::triggered, this, [this, item] { openItem(item); });
    menu.addSeparator();
    auto* copyPathAction = menu.addAction(QStringLiteral("Copy Path"));
    connect(copyPathAction, &QAction::triggered, this,
            [path] { QApplication::clipboard()->setText(path); });
    menu.exec(tree_->viewport()->mapToGlobal(position));
}

void GitChangesPanel::requestItemDiff(QTreeWidgetItem* item) {
    if (item == nullptr || item->parent() == nullptr) {
        return;
    }
    emit diffRequested(item->data(0, pathRole).toString(),
                       static_cast<GitDiffMode>(item->data(0, diffModeRole).toInt()));
}

void GitChangesPanel::openItem(QTreeWidgetItem* item) {
    if (item == nullptr || item->parent() == nullptr) {
        return;
    }
    const QString path = item->data(0, pathRole).toString();
    if (QFileInfo::exists(path)) {
        emit fileOpenRequested(path);
    }
}

} // namespace ketplus
