#include "git/GitChangesPanel.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ketplus {
namespace {

constexpr int pathRole = Qt::UserRole;
constexpr int diffModeRole = Qt::UserRole + 1;
constexpr int subtitleRole = Qt::UserRole + 2;
constexpr int countRole = Qt::UserRole + 3;

class RepositoryIcon final : public QWidget {
  public:
    explicit RepositoryIcon(QWidget* parent) : QWidget(parent) { setFixedSize(18, 18); }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(palette().color(QPalette::Text), 1.5);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        QPainterPath folder;
        folder.moveTo(2.5, 5.5);
        folder.lineTo(7, 5.5);
        folder.lineTo(8.5, 7);
        folder.lineTo(15, 7);
        folder.quadTo(16.5, 7, 16, 8.5);
        folder.lineTo(14.8, 14.5);
        folder.quadTo(14.6, 15.5, 13.4, 15.5);
        folder.lineTo(3.3, 15.5);
        folder.quadTo(2, 15.5, 2.2, 14.2);
        folder.lineTo(2.5, 5.5);
        painter.drawPath(folder);
    }
};

class SourceControlDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        return QSize(0, index.parent().isValid() ? 52 : 40);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const bool group = !index.parent().isValid();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool dark = option.palette.color(QPalette::Window).lightness() < 128;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        if (selected && !group)
            painter->fillRect(option.rect, option.palette.color(QPalette::Highlight));

        if (group) {
            if (index.column() != 0) {
                painter->restore();
                return;
            }
            QPen chevronPen(option.palette.color(QPalette::ButtonText), 1.4);
            chevronPen.setCapStyle(Qt::RoundCap);
            chevronPen.setJoinStyle(Qt::RoundJoin);
            painter->setPen(chevronPen);
            const int centerY = option.rect.center().y();
            QPainterPath chevron;
            if (option.state.testFlag(QStyle::State_Open)) {
                chevron.moveTo(option.rect.left() + 13, centerY - 2);
                chevron.lineTo(option.rect.left() + 17, centerY + 2);
                chevron.lineTo(option.rect.left() + 21, centerY - 2);
            } else {
                chevron.moveTo(option.rect.left() + 15, centerY - 4);
                chevron.lineTo(option.rect.left() + 19, centerY);
                chevron.lineTo(option.rect.left() + 15, centerY + 4);
            }
            painter->drawPath(chevron);

            QFont font = option.font;
            font.setPixelSize(12);
            font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            painter->setPen(option.palette.color(QPalette::ButtonText));
            painter->drawText(option.rect.adjusted(29, 0, -42, 0),
                              Qt::AlignLeft | Qt::AlignVCenter, index.data().toString());

            const QString count = index.data(countRole).toString();
            const int badgeWidth = QFontMetrics(font).horizontalAdvance(count) + 10;
            const QRect badge(option.rect.right() - badgeWidth - 12, centerY - 10, badgeWidth, 20);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(dark ? QStringLiteral("#292F37")
                                          : QStringLiteral("#F1EFF0")));
            painter->drawRoundedRect(badge, 4, 4);
            painter->setPen(option.palette.color(QPalette::ButtonText));
            painter->drawText(badge, Qt::AlignCenter, count);
            painter->restore();
            return;
        }

        if (index.column() == 0) {
            const QColor iconColor = selected ? QColor(QStringLiteral("#5968DF"))
                                              : option.palette.color(QPalette::ButtonText);
            QPen iconPen(iconColor, 1.35);
            iconPen.setCapStyle(Qt::RoundCap);
            iconPen.setJoinStyle(Qt::RoundJoin);
            painter->setPen(iconPen);
            painter->setBrush(Qt::NoBrush);
            const qreal x = option.rect.left() + 14;
            const qreal y = option.rect.center().y() - 8;
            QPainterPath file;
            file.moveTo(x + 2, y + 1);
            file.lineTo(x + 9, y + 1);
            file.lineTo(x + 13, y + 5);
            file.lineTo(x + 13, y + 16);
            file.lineTo(x + 2, y + 16);
            file.closeSubpath();
            file.moveTo(x + 9, y + 1);
            file.lineTo(x + 9, y + 5);
            file.lineTo(x + 13, y + 5);
            painter->drawPath(file);
            painter->drawLine(QPointF(x + 4.5, y + 9.5), QPointF(x + 6.5, y + 11.5));
            painter->drawLine(QPointF(x + 6.5, y + 11.5), QPointF(x + 4.5, y + 13.5));
            painter->drawLine(QPointF(x + 10.5, y + 9.5), QPointF(x + 8.5, y + 11.5));
            painter->drawLine(QPointF(x + 8.5, y + 11.5), QPointF(x + 10.5, y + 13.5));

            const QRect textRect = option.rect.adjusted(36, 5, -8, -4);
            QFont title = option.font;
            title.setPixelSize(12);
            title.setWeight(QFont::Normal);
            painter->setFont(title);
            painter->setPen(selected ? QColor(QStringLiteral("#5968DF"))
                                     : option.palette.color(QPalette::Text));
            painter->drawText(textRect.adjusted(0, 0, 0, -18),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(title).elidedText(index.data().toString(),
                                                             Qt::ElideMiddle, textRect.width()));
            QFont subtitle = option.font;
            subtitle.setPixelSize(11);
            painter->setFont(subtitle);
            painter->setPen(option.palette.color(QPalette::PlaceholderText));
            painter->drawText(textRect.adjusted(0, 18, 0, 0),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(subtitle).elidedText(
                                  index.data(subtitleRole).toString(), Qt::ElideMiddle,
                                  textRect.width()));
        } else {
            QFont status = option.font;
            status.setPixelSize(12);
            status.setWeight(QFont::Medium);
            painter->setFont(status);
            const auto mode = static_cast<GitDiffMode>(index.siblingAtColumn(0)
                                                           .data(diffModeRole)
                                                           .toInt());
            painter->setPen(mode == GitDiffMode::Staged
                                ? QColor(dark ? QStringLiteral("#40C97B")
                                              : QStringLiteral("#1D5D3B"))
                                : QColor(dark ? QStringLiteral("#EF665C")
                                              : QStringLiteral("#B42318")));
            painter->drawText(option.rect, Qt::AlignCenter, index.data().toString());
        }
        painter->restore();
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

    auto* heading = new QLabel(QStringLiteral("SOURCE CONTROL"), this);
    heading->setObjectName(QStringLiteral("sourceControlHeading"));
    heading->setProperty("kvRole", QStringLiteral("sourceControlHeading"));
    repositoryLabel_->setObjectName(QStringLiteral("sourceControlRepositoryName"));
    repositoryLabel_->setProperty("kvRole", QStringLiteral("gitRepositoryName"));
    branchLabel_->setObjectName(QStringLiteral("sourceControlBranch"));
    branchLabel_->setProperty("kvRole", QStringLiteral("gitRepositoryBranch"));
    branchLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* refreshButton = new QToolButton(this);
    refreshButton->setText(QString::fromUtf8("↻"));
    refreshButton->setProperty("kvRole", QStringLiteral("sidebarAction"));
    refreshButton->setToolTip(QStringLiteral("Refresh Git status"));
    refreshButton->setAccessibleName(QStringLiteral("Refresh Git status"));

    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("sourceControlHeader"));
    header->setFixedHeight(44);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(12, 0, 8, 0);
    headerLayout->setSpacing(4);
    headerLayout->addWidget(heading);
    headerLayout->addStretch();
    headerLayout->addWidget(refreshButton);

    auto* repositoryLayout = new QHBoxLayout(repositorySummary_);
    repositoryLayout->setContentsMargins(14, 10, 14, 19);
    repositoryLayout->setSpacing(8);
    repositoryLayout->addWidget(new RepositoryIcon(repositorySummary_), 0, Qt::AlignTop);
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
    tree_->header()->resizeSection(1, 32);

    pages_->addWidget(emptyLabel_);
    pages_->addWidget(tree_);
    pages_->setCurrentWidget(emptyLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(repositorySummary_);
    layout->addWidget(pages_, 1);

    connect(refreshButton, &QToolButton::clicked, this, &GitChangesPanel::refreshRequested);
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

    addGroup(QStringLiteral("Changes"), unstaged, GitDiffMode::Unstaged,
             snapshot.repositoryRoot);
    addGroup(QStringLiteral("Staged Changes"), staged, GitDiffMode::Staged,
             snapshot.repositoryRoot);
    tree_->expandAll();
    pages_->setCurrentWidget(tree_);
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

void GitChangesPanel::addGroup(const QString& title,
                               const QVector<const GitFileStatus*>& files,
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
        item->setToolTip(1, mode == GitDiffMode::Staged
                                ? QStringLiteral("Staged change")
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
