#include "app/WorktreesWorkspacePage.h"
#include "KetBadge.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "ui/ChromeGeometry.h"
#include "ui/SymbolIcon.h"

#include "ui/Theme.h"
#include "ui/TypeScale.h"

#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {
namespace {

constexpr auto gitBranchIcon = "git-branch";
constexpr auto archiveIcon = "archive";

QString normalizedPath(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QString worktreeTitle(const GitWorktree& worktree) {
    if (!worktree.branch.isEmpty())
        return worktree.branch;
    return QObject::tr("Detached %1").arg(worktree.head.left(8));
}

class WorktreesPageContent final : public QWidget {
  public:
    using QWidget::QWidget;

    QSize sizeHint() const override {
        QSize hint = QWidget::sizeHint();
        hint.setWidth(1320);
        return hint;
    }
};

} // namespace

WorktreesWorkspacePage::WorktreesWorkspacePage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("worktreesWorkspacePage"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* pageLayout = new QVBoxLayout(this);
    chrome_geometry::margins(pageLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(pageLayout, 0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setObjectName(QStringLiteral("worktreesWorkspaceScroll"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* scrollBody = new QWidget(scrollArea);
    scrollBody->setObjectName(QStringLiteral("worktreesWorkspaceScrollBody"));
    auto* scrollLayout = new QVBoxLayout(scrollBody);
    chrome_geometry::margins(scrollLayout, 42, 36, 42, 38);
    chrome_geometry::spacing(scrollLayout, 0);

    auto* content = new WorktreesPageContent(scrollBody);
    content->setObjectName(QStringLiteral("worktreesWorkspaceContent"));
    chrome_geometry::maximumWidth(content, 1320);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* contentLayout = new QVBoxLayout(content);
    chrome_geometry::margins(contentLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(contentLayout, 0);

    auto* heading = new QWidget(content);
    auto* headingLayout = new QHBoxLayout(heading);
    chrome_geometry::margins(headingLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(headingLayout, 16);
    auto* copy = new QWidget(heading);
    auto* copyLayout = new QVBoxLayout(copy);
    chrome_geometry::margins(copyLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(copyLayout, 5);
    auto* title = new QLabel(tr("Worktrees"), copy);
    title->setObjectName(QStringLiteral("worktreesWorkspaceTitle"));
    auto* subtitle = new QLabel(tr("Independent files, conversations and drafts."), copy);
    subtitle->setObjectName(QStringLiteral("worktreesWorkspaceSubtitle"));
    copyLayout->addWidget(title);
    copyLayout->addWidget(subtitle);
    headingLayout->addWidget(copy, 1);
    auto* actions = new QWidget(heading);
    auto* actionsLayout = new QHBoxLayout(actions);
    chrome_geometry::margins(actionsLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(actionsLayout, 8);
    pruneButton_ = new ket::Button(tr("Prune worktrees"), ket::ButtonVariant::Secondary, actions);
    pruneButton_->setObjectName(QStringLiteral("worktreesPruneButton"));
    pruneButton_->setAccessibleName(tr("Prune missing worktrees"));
    pruneButton_->setToolTip(tr("Remove Git metadata for worktrees whose folders no longer exist"));
    chrome_geometry::fixedHeight(pruneButton_, 34);
    pruneButton_->setEnabled(false);
    connect(pruneButton_, &QAbstractButton::clicked, this, &WorktreesWorkspacePage::pruneRequested);
    actionsLayout->addWidget(pruneButton_);

    auto* create = new ket::Button(tr("Create worktree"), ket::ButtonVariant::Primary, actions);
    create->setIcon(ket::Icons::icon(u"plus"));
    create->setObjectName(QStringLiteral("worktreesCreateButton"));
    create->setProperty("kvRole", QStringLiteral("primaryAction"));
    create->setAccessibleName(tr("Create worktree"));
    chrome_geometry::fixedHeight(create, 34);
    connect(create, &QAbstractButton::clicked, this, &WorktreesWorkspacePage::createRequested);
    actionsLayout->addWidget(create);
    headingLayout->addWidget(actions, 0, Qt::AlignTop);
    contentLayout->addWidget(heading);
    chrome_geometry::addSpacing(contentLayout, 28);

    auto* list = new QFrame(content);
    list->setObjectName(QStringLiteral("worktreesWorkspaceList"));
    listLayout_ = new QVBoxLayout(list);
    chrome_geometry::margins(listLayout_, 0, 0, 0, 0);
    chrome_geometry::spacing(listLayout_, 0);
    contentLayout->addWidget(list);

    auto* archived = new ket::Button(tr("Archived worktrees and conversations"),
                                     ket::ButtonVariant::Ghost, content);
    archived->setObjectName(QStringLiteral("worktreesArchivedButton"));
    archived->setProperty("kvRole", QStringLiteral("worktreesTextButton"));
    archived->setText(tr("Archived worktrees and conversations"));

    chrome_geometry::iconSize(archived, QSize(15, 15));
    chrome_geometry::fixedHeight(archived, 36);
    archived->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    connect(archived, &QAbstractButton::clicked, this, &WorktreesWorkspacePage::archivedRequested);
    contentLayout->addWidget(archived, 0, Qt::AlignLeft);
    contentLayout->addStretch(1);

    scrollLayout->addWidget(content, 1, Qt::AlignHCenter);
    scrollArea->setWidget(scrollBody);
    pageLayout->addWidget(scrollArea, 1);
}

void WorktreesWorkspacePage::setWorktrees(const QVector<GitWorktree>& worktrees,
                                          const QString& activePath,
                                          const QSet<QString>& runningPaths,
                                          const QHash<QString, int>& conversationCounts,
                                          const bool activeDirty) {
    worktrees_ = worktrees;
    activePath_ = normalizedPath(activePath);
    runningPaths_.clear();
    for (const QString& path : runningPaths)
        runningPaths_.insert(normalizedPath(path));
    conversationCounts_ = conversationCounts;
    activeDirty_ = activeDirty;
    rebuild();
}

void WorktreesWorkspacePage::setPruneAvailable(const bool available) {
    pruneAvailable_ = available;
    if (pruneButton_ != nullptr && !pruning_)
        pruneButton_->setEnabled(available);
}

void WorktreesWorkspacePage::setPruning(const bool pruning) {
    if (pruneButton_ == nullptr)
        return;
    pruning_ = pruning;
    pruneButton_->setText(pruning ? tr("Pruning…") : tr("Prune worktrees"));
    pruneButton_->setEnabled(!pruning && pruneAvailable_);
}

void WorktreesWorkspacePage::rebuild() {
    while (auto* item = listLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    int visibleIndex = 0;
    QVector<QWidget*> rows;
    for (const GitWorktree& worktree : std::as_const(worktrees_)) {
        if (worktree.bare || worktree.prunable)
            continue;
        const QString path = normalizedPath(worktree.path);
        auto* row = new QWidget(this);
        row->setObjectName(QStringLiteral("worktreesWorkspaceRow"));
        chrome_geometry::fixedHeight(row, 90);
        auto* rowLayout = new QHBoxLayout(row);
        chrome_geometry::margins(rowLayout, 17, 12, 15, 12);
        chrome_geometry::spacing(rowLayout, 13);

        auto* identity = new QWidget(row);
        identity->setObjectName(QStringLiteral("worktreesWorkspaceIdentity"));
        chrome_geometry::fixedHeight(identity, 40);
        identity->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* identityLayout = new QHBoxLayout(identity);
        chrome_geometry::margins(identityLayout, 0, 0, 0, 0);
        chrome_geometry::spacing(identityLayout, 13);

        auto* icon = new QLabel(identity);
        icon->setObjectName(QStringLiteral("worktreesWorkspaceIcon"));
        chrome_geometry::fixedSize(icon, 17, 17);
        icon->setPixmap(symbol_icon::pixmap(gitBranchIcon, QColor(textMuted_), icon->size()));
        identityLayout->addWidget(icon, 0, Qt::AlignTop);

        auto* open = new ket::Button({}, ket::ButtonVariant::Ghost, identity);
        open->setObjectName(QStringLiteral("worktreesWorkspaceOpenButton"));
        open->setProperty("kvRole", QStringLiteral("worktreesOpenButton"));

        open->setToolTip(path);
        open->setAccessibleName(worktreeTitle(worktree));
        open->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        chrome_geometry::fixedHeight(open, 40);
        auto* openLayout = new QVBoxLayout(open);
        chrome_geometry::margins(openLayout, 0, 0, 0, 0);
        chrome_geometry::spacing(openLayout, 5);
        auto* name = new QLabel(worktreeTitle(worktree), open);
        name->setObjectName(QStringLiteral("worktreesWorkspaceName"));
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* conversations =
            new QLabel(tr("%1 conversations").arg(conversationCounts_.value(path)), open);
        conversations->setObjectName(QStringLiteral("worktreesWorkspaceMeta"));
        conversations->setAttribute(Qt::WA_TransparentForMouseEvents);
        openLayout->addWidget(name);
        openLayout->addWidget(conversations);
        connect(open, &QAbstractButton::clicked, this,
                [this, path] { emit worktreeRequested(path); });
        identityLayout->addWidget(open, 1);
        rowLayout->addWidget(identity, 1, Qt::AlignVCenter);

        auto* activity =
            new QLabel(runningPaths_.contains(path) ? tr("1 working") : tr("0 working"), row);
        activity->setObjectName(QStringLiteral("worktreesWorkspaceActivity"));
        activity->setProperty("running", runningPaths_.contains(path));
        chrome_geometry::minimumWidth(activity, 76);
        rowLayout->addWidget(activity);

        const bool current = path == activePath_;
        auto* state = new ket::Badge(
            current ? (activeDirty_ ? tr("Uncommitted changes") : tr("Clean"))
                    : (worktree.locked ? tr("Locked") : tr("Available")),
            current && activeDirty_ ? ket::BadgeTone::Warning : ket::BadgeTone::Neutral, row);
        state->setObjectName(QStringLiteral("worktreesWorkspaceState"));
        state->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        rowLayout->addWidget(state, 0, Qt::AlignVCenter);

        auto* manage = new ket::Button(tr("Manage"), ket::ButtonVariant::Ghost, row);
        manage->setObjectName(QStringLiteral("worktreesManageButton"));
        manage->setProperty("kvRole", QStringLiteral("worktreesManageButton"));
        manage->setIcon(ket::Icons::icon(u"more"));
        manage->setAccessibleName(tr("Manage %1").arg(worktreeTitle(worktree)));
        chrome_geometry::fixedHeight(manage, 30);
        connect(manage, &QAbstractButton::clicked, this,
                [this, path] { emit manageRequested(path); });
        rowLayout->addWidget(manage);

        listLayout_->addWidget(row);
        rows.append(row);
        ++visibleIndex;
    }

    if (!rows.isEmpty())
        rows.constLast()->setProperty("lastRecord", true);

    if (visibleIndex == 0) {
        auto* empty = new QLabel(tr("No linked worktrees are available."), this);
        empty->setObjectName(QStringLiteral("worktreesWorkspaceEmpty"));
        empty->setAlignment(Qt::AlignCenter);
        chrome_geometry::minimumHeight(empty, 120);
        listLayout_->addWidget(empty);
    }
}

void WorktreesWorkspacePage::applyPalette(const ThemePalette& palette,
                                          const int interfaceFontSizePixels) {
    pageBackground_ = palette.pageBackground;
    panelBackground_ = palette.panelBackground;
    textMain_ = palette.textMain;
    textMuted_ = palette.textMuted;
    border_ = palette.border;
    surfaceRaised_ = palette.surfaceRaised;
    positive_ = palette.positive;
    warning_ = palette.warning;
    const QString sheet = TypeScale::apply(QStringLiteral(R"QSS(
QWidget#worktreesWorkspacePage, QWidget#worktreesWorkspaceScrollBody,
QWidget#worktreesWorkspaceContent { background:%1; color:%3; }
QScrollArea#worktreesWorkspaceScroll { border:0; background:%1; }
QLabel#worktreesWorkspaceTitle { color:%3; font-size:30px; font-weight:600; }
QLabel#worktreesWorkspaceSubtitle { color:%4; font-size:14px; }
QPushButton#worktreesPruneButton { min-width:112px; padding:0 12px; border:1px solid %5;
 border-radius:5px; background:%2; color:%3; font-size:{{caption}}px; font-weight:550; }
QPushButton#worktreesPruneButton:hover { background:%6; }
QPushButton#worktreesPruneButton:disabled { color:%4; background:%1; }
QFrame#worktreesWorkspaceList { border:1px solid %5; border-radius:7px; background:%2; }
QWidget#worktreesWorkspaceRow { border-bottom:1px solid %5; background:transparent; }
QWidget#worktreesWorkspaceRow[lastRecord="true"] { border-bottom:0; }
QToolButton[kvRole="worktreesOpenButton"], QToolButton[kvRole="worktreesManageButton"],
QToolButton[kvRole="worktreesTextButton"] { border:0; background:transparent; color:%3; }
QToolButton[kvRole="worktreesOpenButton"] { padding:0; text-align:left; }
QToolButton[kvRole="worktreesManageButton"] { padding:0 7px; font-size:{{caption}}px; }
QToolButton[kvRole="worktreesManageButton"]:hover,
QToolButton[kvRole="worktreesTextButton"]:hover { background:%6; }
QToolButton[kvRole="worktreesTextButton"] { padding:0 8px; color:%4; font-size:{{caption}}px; }
QLabel#worktreesWorkspaceName { color:%3; font-size:15px; font-weight:500; }
QLabel#worktreesWorkspaceMeta, QLabel#worktreesWorkspaceEmpty { color:%4; font-size:13px; }
QLabel#worktreesWorkspaceActivity { color:%4; font-size:{{caption}}px; }
QLabel#worktreesWorkspaceActivity[running="true"] { color:%7; }
QLabel#worktreesWorkspaceState { min-height:22px; padding:0 8px; border:1px solid %5;
 border-radius:4px; background:%1; color:%4; font-size:{{micro}}px; }
QLabel#worktreesWorkspaceState[warning="true"] { border-color:%8; color:%8; }
)QSS"),
                                           interfaceFontSizePixels);
    setStyleSheet(sheet.arg(pageBackground_, panelBackground_, textMain_, textMuted_, border_,
                            surfaceRaised_, positive_, warning_));
    chrome_geometry::apply(this, interfaceFontSizePixels);
    if (auto* archived = findChild<QAbstractButton*>(QStringLiteral("worktreesArchivedButton")))
        archived->setIcon(symbol_icon::pixmap(archiveIcon, QColor(textMuted_), QSize(15, 15)));
    for (auto* icon : findChildren<QLabel*>(QStringLiteral("worktreesWorkspaceIcon")))
        icon->setPixmap(symbol_icon::pixmap(gitBranchIcon, QColor(textMuted_), icon->size()));
}

} // namespace ketplus
