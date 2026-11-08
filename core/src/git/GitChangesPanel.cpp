#include "git/GitChangesPanel.h"

#include "KetAppearance.h"
#include "KetButton.h"
#include "KetChrome.h"
#include "KetControlRecipes.h"
#include "KetIcon.h"
#include "KetIcons.h"
#include "KetSurfaceRecipes.h"
#include "ui/Theme.h"
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace ketplus {
namespace {

constexpr int pathRole = Qt::UserRole;
constexpr int diffModeRole = Qt::UserRole + 1;
constexpr int subtitleRole = Qt::UserRole + 2;
constexpr int countRole = Qt::UserRole + 3;

// The kit row recipes rendered over the Git item model keep keyboard selection and
// refresh identity without allocating a FileRow widget for every changed file.
class SourceControlDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        auto& a = ket::Appearance::instance();
        const qreal height = index.parent().isValid()
                                 ? ket::fileRowRecipe(false, {}, a.theme()).height
                                 : ket::groupHeaderRecipe(false, {}, a.theme()).height;
        return {0, qRound(a.scaled(height))};
    }
    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        auto& a = ket::Appearance::instance();
        const auto px = [&a](qreal n) { return qRound(a.scaled(n)); };
        const bool group = !index.parent().isValid();
        if (group && index.column() != 0)
            return;
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const auto r = ket::fileRowRecipe(selected,
                                          {option.state.testFlag(QStyle::State_MouseOver), false,
                                           true, option.state.testFlag(QStyle::State_HasFocus)},
                                          a.theme());
        const auto g = ket::groupHeaderRecipe(false, {}, a.theme());
        p->save();
        p->setClipRect(option.rect);
        p->fillRect(option.rect, r.background);
        QFont font = option.font;
        font.setPixelSize(px(group ? g.fontPixels : r.fontPixels));
        font.setWeight(group ? QFont::Weight(g.fontWeight) : QFont::Normal);
        p->setFont(font);
        if (index.column() == 1) {
            p->setPen(themeTokenColor(index.siblingAtColumn(0).data(diffModeRole).toInt() ==
                                              int(GitDiffMode::Staged)
                                          ? QStringLiteral("positive")
                                          : QStringLiteral("warning"),
                                      a.theme() == ket::Theme::Dark));
            p->drawText(option.rect, Qt::AlignCenter, index.data().toString());
        } else {
            const int icon = px(group ? g.iconSize : r.iconSize);
            const int left = px(r.paddingInline);
            const QString code = ket::Icons::codeForLucide(
                group ? (option.state.testFlag(QStyle::State_Open) ? u"chevron-down"
                                                                   : u"chevron-right")
                      : u"file");
            p->drawPixmap(option.rect.left() + left, option.rect.center().y() - icon / 2,
                          ket::Icons::pixmap(code, icon, p->device()->devicePixelRatioF(),
                                             group ? g.text : r.text));
            const QString title =
                index.data().toString() +
                (group ? QStringLiteral("  %1").arg(index.data(countRole).toInt()) : QString{});
            const QRect text =
                option.rect.adjusted(left + icon + px(r.spacing), 0, -px(r.spacing), 0);
            p->setPen(group ? g.text : r.text);
            p->drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetrics(font).elidedText(title, Qt::ElideMiddle, text.width()));
        }
        if (r.focusRing.alpha()) {
            p->setPen(r.focusRing);
            p->setBrush(Qt::NoBrush);
            p->drawRect(option.rect.adjusted(0, 0, -1, -1));
        }
        p->restore();
    }
};

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
    : QWidget(parent), repositoryLabel_(new QLabel(this)), branchLabel_(new QLabel(this)),
      repositorySummary_(new QWidget(this)), emptyLabel_(new QLabel(this)),
      pages_(new QStackedWidget(this)), tree_(new QTreeWidget(this)) {
    setProperty("kvRole", QStringLiteral("explorer"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumWidth(210);

    repositoryLabel_->setObjectName(QStringLiteral("sourceControlRepositoryName"));
    repositoryLabel_->setProperty("kvRole", QStringLiteral("gitRepositoryName"));
    branchLabel_->setObjectName(QStringLiteral("sourceControlBranch"));
    branchLabel_->setProperty("kvRole", QStringLiteral("gitRepositoryBranch"));
    branchLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* header =
        new ket::PaneHeader(tr("Source Control"), ket::Icons::codeForLucide(u"git-branch"), this);
    header->setObjectName(QStringLiteral("sourceControlHeader"));
    auto* refreshButton =
        new ket::IconButton(ket::Icons::icon(ket::Icons::codeForLucide(u"refresh-cw")),
                            tr("Refresh Git status"), header);
    header->actions()->addWidget(refreshButton);

    auto* repositoryLayout = new QHBoxLayout(repositorySummary_);
    repositoryLayout->setContentsMargins(14, 10, 14, 19);
    repositoryLayout->setSpacing(8);
    repositoryLayout->addWidget(
        new ket::Icon(ket::Icons::codeForLucide(u"folder"), 18, repositorySummary_), 0,
        Qt::AlignTop);
    auto* repositoryText = new QVBoxLayout;
    repositoryText->setContentsMargins(0, 0, 0, 0);
    repositoryText->setSpacing(4);
    repositoryText->addWidget(repositoryLabel_);
    repositoryText->addWidget(branchLabel_);
    repositoryLayout->addLayout(repositoryText, 1);

    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setProperty("kvRole", QStringLiteral("emptyTitle"));
    emptyLabel_->setWordWrap(true);

    tree_->setProperty("kvRole", QStringLiteral("gitChanges"));
    tree_->setObjectName(QStringLiteral("sourceControlFiles"));
    tree_->setHeaderHidden(true);
    tree_->setRootIsDecorated(false);
    tree_->setIndentation(0);
    tree_->setUniformRowHeights(false);
    tree_->setItemDelegate(new SourceControlDelegate(tree_));
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setColumnCount(2);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    tree_->header()->resizeSection(1, qRound(ket::Appearance::instance().scaled(32)));
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, tree_, [this] {
        tree_->header()->resizeSection(1, qRound(ket::Appearance::instance().scaled(32)));
        tree_->doItemsLayout();
    });

    pages_->addWidget(emptyLabel_);
    pages_->addWidget(tree_);
    pages_->setCurrentWidget(emptyLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(repositorySummary_);
    createCommitControls(layout);
    layout->addWidget(pages_, 1);

    connect(refreshButton, &QAbstractButton::clicked, this, &GitChangesPanel::refreshRequested);
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        if (item != nullptr && item->parent() == nullptr) {
            item->setExpanded(!item->isExpanded());
            return;
        }
        requestItemDiff(item);
    });
    connect(tree_, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem* item) { openItem(item); });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this,
            &GitChangesPanel::showContextMenu);

    setSnapshot({});
}

void GitChangesPanel::setSnapshot(const GitSnapshot& snapshot) {
    hasStagedChanges_ = std::any_of(snapshot.files.cbegin(), snapshot.files.cend(),
                                    [](const auto& file) { return file.hasStagedChange(); });
    updateCommitEnabled();
    // Refreshes run on every save and Git change; keep the user's place in the list.
    const QTreeWidgetItem* previous = tree_->currentItem();
    const bool hadFile = previous != nullptr && previous->parent() != nullptr;
    const QString selectedPath = hadFile ? previous->data(0, pathRole).toString() : QString{};
    const int selectedMode = hadFile ? previous->data(0, diffModeRole).toInt() : -1;
    const int scrollPosition = tree_->verticalScrollBar()->value();
    tree_->clear();

    if (!snapshot.isRepository()) {
        repositoryLabel_->clear();
        branchLabel_->clear();
        repositorySummary_->hide();
        emptyLabel_->setText(QStringLiteral("No Git repository"));
        pages_->setCurrentWidget(emptyLabel_);
        return;
    }

    repositorySummary_->show();
    repositoryLabel_->setText(QFileInfo(snapshot.repositoryRoot).fileName());
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

    addGroup(QStringLiteral("Changes"), unstaged, GitDiffMode::Unstaged, snapshot.repositoryRoot);
    addGroup(QStringLiteral("Staged Changes"), staged, GitDiffMode::Staged,
             snapshot.repositoryRoot);
    tree_->expandAll();
    pages_->setCurrentWidget(tree_);
    if (hadFile) {
        for (int group = 0; group < tree_->topLevelItemCount(); ++group) {
            auto* parent = tree_->topLevelItem(group);
            for (int child = 0; child < parent->childCount(); ++child) {
                auto* item = parent->child(child);
                if (item->data(0, pathRole).toString() == selectedPath &&
                    item->data(0, diffModeRole).toInt() == selectedMode) {
                    tree_->setCurrentItem(item);
                }
            }
        }
    }
    tree_->verticalScrollBar()->setValue(scrollPosition);
}

void GitChangesPanel::focusChanges() {
    tree_->setFocus();
    if ((tree_->currentItem() == nullptr || tree_->currentItem()->parent() == nullptr) &&
        tree_->topLevelItemCount() > 0) {
        auto* group = tree_->topLevelItem(0);
        if (group->childCount() > 0) {
            tree_->setCurrentItem(group->child(0));
        }
    }
}

void GitChangesPanel::addGroup(const QString& title, const QVector<const GitFileStatus*>& files,
                               const GitDiffMode mode, const QString& repositoryRoot) {
    if (files.isEmpty()) {
        return;
    }

    auto* group = new QTreeWidgetItem(tree_);
    group->setText(0, title);
    group->setData(0, countRole, files.size());
    group->setFirstColumnSpanned(true);
    group->setFlags(Qt::ItemIsEnabled);
    group->setSizeHint(0, QSize(0, 40));

    for (const auto* status : files) {
        auto* item = new QTreeWidgetItem(group);
        const QString absolutePath = QDir(repositoryRoot).filePath(status->path);
        const QFileInfo relativePath(displayPath(*status));
        item->setText(0, relativePath.fileName());
        item->setData(0, subtitleRole,
                      relativePath.path() == QStringLiteral(".") ? QString{} : relativePath.path());
        item->setText(1, statusLabel(*status, mode));
        item->setTextAlignment(1, Qt::AlignCenter);
        item->setData(0, pathRole, absolutePath);
        item->setData(0, diffModeRole, static_cast<int>(mode));
        item->setToolTip(0, absolutePath);
        item->setToolTip(1, mode == GitDiffMode::Staged ? QStringLiteral("Staged change")
                                                        : QStringLiteral("Working tree change"));
        item->setSizeHint(0, QSize(0, 52));
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
    auto* stageAction = menu.addAction(
        item->data(0, diffModeRole).toInt() == int(GitDiffMode::Staged) ? tr("Unstage File")
                                                                        : tr("Stage File"));
    stageAction->setEnabled(!mutationRunning_);
    const bool staged = item->data(0, diffModeRole).toInt() == int(GitDiffMode::Staged);
    connect(stageAction, &QAction::triggered, this, [this, path, staged] {
        if (staged)
            emit unstageRequested(path);
        else
            emit stageRequested(path);
    });
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
