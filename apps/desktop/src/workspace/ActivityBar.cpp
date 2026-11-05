#include "workspace/ActivityBar.h"
#include "ui/ChromeGeometry.h"
#include "ui/LucideIcon.h"

#include "ui/Theme.h"
#include "ui/TypeScale.h"
#include "workspace/LucideIcons.h"

#include <QAction>
#include <QButtonGroup>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace ketplus {
namespace {

constexpr int kRowHeight = 32;
constexpr int kResizeHandleWidth = 5;
const auto kWidthSetting = QStringLiteral("sidebar/width");

// Drag target on the sidebar's right edge. The width is persisted when the drag ends.
class SidebarResizeHandle final : public QWidget {
  public:
    explicit SidebarResizeHandle(ActivityBar* bar) : QWidget(bar), bar_(bar) {
        setObjectName(QStringLiteral("productSidebarResizeHandle"));
        setCursor(Qt::SplitHCursor);
        setAttribute(Qt::WA_NoSystemBackground);
    }

  protected:
    void mousePressEvent(QMouseEvent* event) override {
        startX_ = event->globalPosition().x();
        startWidth_ = bar_->sidebarWidth();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons().testFlag(Qt::LeftButton))
            bar_->setSidebarWidth(startWidth_ + qRound(event->globalPosition().x() - startX_));
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        QSettings().setValue(kWidthSetting, bar_->sidebarWidth());
    }
    void mouseDoubleClickEvent(QMouseEvent*) override {
        bar_->setSidebarWidth(ActivityBar::kWidth);
        QSettings().setValue(kWidthSetting, bar_->sidebarWidth());
    }

  private:
    ActivityBar* bar_;
    qreal startX_{0};
    int startWidth_{ActivityBar::kWidth};
};

// Icons render at the size they are shown so the stroke keeps its on-screen width.
QIcon lucideIcon(const QByteArray& body, const QSize& size,
                 const QColor& color = QColor(QStringLiteral("#6b7280"))) {
    return QIcon(lucide_icon::pixmap(body, color, size));
}

} // namespace

ActivityBar::ActivityBar(QWidget* parent) : QWidget(parent), group_(new QButtonGroup(this)) {
    setObjectName(QStringLiteral("activityBar"));
    setProperty("kvRole", QStringLiteral("productSidebar"));
    setAttribute(Qt::WA_StyledBackground, true);
    sidebarWidth_ = qBound(kMinWidth, QSettings().value(kWidthSetting, kWidth).toInt(), kMaxWidth);
    setFixedWidth(sidebarWidth_);

    group_->setExclusive(true);
    auto* layout = new QVBoxLayout(this);
    chrome_geometry::margins(layout, 6, 0, 4, 0);
    chrome_geometry::spacing(layout, 0);

    auto* brand = new QWidget(this);
    brand->setObjectName(QStringLiteral("productBrand"));
    brand->setProperty("kvRole", QStringLiteral("productBrand"));
    chrome_geometry::fixedHeight(brand, 50);
    auto* brandLayout = new QHBoxLayout(brand);
    chrome_geometry::margins(brandLayout, 4, 0, 2, 0);
    chrome_geometry::spacing(brandLayout, 6);
    brandWordmark_ = new QLabel(brand);
    brandWordmark_->setObjectName(QStringLiteral("productBrandWordmark"));
    // The KétPlus Pro wordmark is 4:1 and already names the edition.
    chrome_geometry::fixedSize(brandWordmark_, 120, 30);
    brandWordmark_->setAccessibleName(QStringLiteral("KétPlus Pro"));
    brandLayout->addWidget(brandWordmark_);
    brandLayout->addStretch();
    layout->addWidget(brand);

    quickSearchButton_ = new QToolButton(this);
    quickSearchButton_->setObjectName(QStringLiteral("quickSearchButton"));
    quickSearchButton_->setProperty("kvRole", QStringLiteral("productSearch"));
    quickSearchButton_->setText(tr("Quick search"));
    quickSearchButton_->setProperty("lucideBody", QByteArray(lucide::search));
    chrome_geometry::iconSize(quickSearchButton_, QSize(14, 14));
    quickSearchButton_->setIcon(lucideIcon(lucide::search, quickSearchButton_->iconSize()));
    quickSearchButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    quickSearchButton_->setAccessibleName(tr("Quick search"));
    chrome_geometry::fixedHeight(quickSearchButton_, kRowHeight);
    quickSearchButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    quickSearchShortcut_ = new QLabel(QStringLiteral("⌘ K"), quickSearchButton_);
    quickSearchShortcut_->setObjectName(QStringLiteral("quickSearchShortcut"));
    quickSearchShortcut_->setAlignment(Qt::AlignCenter);
    quickSearchShortcut_->setAttribute(Qt::WA_TransparentForMouseEvents);
    // Laid out, not positioned, so the shortcut follows the resizable sidebar edge.
    auto* searchRow = new QHBoxLayout(quickSearchButton_);
    searchRow->setSizeConstraint(QLayout::SetNoConstraint);
    chrome_geometry::margins(searchRow, 0, 0, 8, 0);
    searchRow->addStretch();
    searchRow->addWidget(quickSearchShortcut_);
    connect(quickSearchButton_, &QToolButton::clicked, this, &ActivityBar::quickSearchRequested);
    layout->addWidget(quickSearchButton_);
    chrome_geometry::addSpacing(layout, 12);

    agentButton_ =
        addModeButton(lucide::messagesSquare, tr("Agent Workspace"), Mode::AgentWorkspace);
    explorerButton_ = addModeButton(lucide::folder, tr("Files & Editor"), Mode::Explorer);
    sourceControlButton_ =
        addModeButton(lucide::gitBranch, tr("Source Control"), Mode::SourceControl);
    remoteButton_ = addModeButton(lucide::unplug, tr("Connections"), Mode::Remote);
    explorerButton_->setObjectName(QStringLiteral("explorerActivityButton"));
    sourceControlButton_->setObjectName(QStringLiteral("sourceControlActivityButton"));
    sourceControlCount_ = new QLabel(sourceControlButton_);
    sourceControlCount_->setObjectName(QStringLiteral("sourceControlCountBadge"));
    sourceControlCount_->setAlignment(Qt::AlignCenter);
    chrome_geometry::fixedSize(sourceControlCount_, 22, 16);
    sourceControlCount_->setAttribute(Qt::WA_TransparentForMouseEvents);
    static_cast<QHBoxLayout*>(sourceControlButton_->layout())->addWidget(sourceControlCount_);
    sourceControlCount_->hide();
    agentButton_->setObjectName(QStringLiteral("agentWorkspaceActivityButton"));
    remoteButton_->setObjectName(QStringLiteral("remoteActivityButton"));
    const QList<QToolButton*> navigationButtons = {agentButton_, explorerButton_,
                                                   sourceControlButton_, remoteButton_};
    for (int index = 0; index < navigationButtons.size(); ++index) {
        auto* button = navigationButtons.at(index);
        layout->addWidget(button);
        if (index + 1 < navigationButtons.size())
            chrome_geometry::addSpacing(layout, 4);
    }

    auto* contextScroll = new QScrollArea(this);
    contextScroll->setObjectName(QStringLiteral("productSidebarContext"));
    contextScroll->setFrameShape(QFrame::NoFrame);
    contextScroll->setWidgetResizable(true);
    contextScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    contextScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // The handle is transparent at rest and appears while scrolling. Only its color changes, so
    // the viewport width (and every row) stays put.
    auto* scrollBar = contextScroll->verticalScrollBar();
    scrollBar->setProperty("scrolling", false);
    auto* hideScrollBar = new QTimer(this);
    hideScrollBar->setSingleShot(true);
    hideScrollBar->setInterval(900);
    const auto setScrolling = [scrollBar](const bool scrolling) {
        if (scrollBar->property("scrolling").toBool() == scrolling)
            return;
        scrollBar->setProperty("scrolling", scrolling);
        scrollBar->style()->unpolish(scrollBar);
        scrollBar->style()->polish(scrollBar);
        scrollBar->update();
    };
    connect(scrollBar, &QScrollBar::valueChanged, this, [setScrolling, hideScrollBar] {
        setScrolling(true);
        hideScrollBar->start();
    });
    connect(hideScrollBar, &QTimer::timeout, this, [setScrolling] { setScrolling(false); });
    auto* context = new QWidget(contextScroll);
    context->setObjectName(QStringLiteral("productSidebarContextBody"));
    auto* contextLayout = new QVBoxLayout(context);
    chrome_geometry::margins(contextLayout, 0, 0, 0, 8);
    chrome_geometry::spacing(contextLayout, 0);
    chrome_geometry::addSpacing(contextLayout, 8);
    auto* contextRule = new QFrame(context);
    contextRule->setObjectName(QStringLiteral("productSidebarRule"));
    chrome_geometry::fixedHeight(contextRule, 1);
    contextLayout->addWidget(contextRule);
    chrome_geometry::addSpacing(contextLayout, 6);

    // Repositories are the top level; each lists its conversations, and a conversation
    // carries the worktree it runs in. Worktrees are not a separate sidebar level.
    auto* machineRow = new QWidget(context);
    auto* machineLayout = new QHBoxLayout(machineRow);
    chrome_geometry::margins(machineLayout, 8, 0, 8, 0);
    chrome_geometry::spacing(machineLayout, 4);
    machineSelect_ = new QComboBox(machineRow);
    machineSelect_->setObjectName(QStringLiteral("productMachineSelect"));
    machineSelect_->setAccessibleName(tr("Machine for repositories"));
    machineSelect_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    machineSelect_->addItem(tr("This Mac"), QStringLiteral("local"));
    chrome_geometry::fixedHeight(machineSelect_, 28);
    connect(machineSelect_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0)
            emit machineRequested(machineSelect_->itemData(index).toString());
    });
    machineLayout->addWidget(machineSelect_, 1);
    repositoryAction_ = new QToolButton(machineRow);
    repositoryAction_->setProperty("kvRole", QStringLiteral("productSectionAction"));
    repositoryAction_->setProperty("lucideBody", QByteArray(lucide::plus));
    repositoryAction_->setAccessibleName(tr("Add repository"));
    repositoryAction_->setToolTip(tr("Add repository"));
    chrome_geometry::fixedSize(repositoryAction_, 22, 22);
    machineLayout->addWidget(repositoryAction_);
    contextLayout->addWidget(machineRow);
    repositoryAction_->setObjectName(QStringLiteral("productAddRepositoryButton"));
    connect(repositoryAction_, &QToolButton::clicked, this, &ActivityBar::repositoryRequested);
    machineNotice_ = new QLabel(context);
    machineNotice_->setObjectName(QStringLiteral("productMachineNotice"));
    machineNotice_->setWordWrap(true);
    machineNotice_->setText(tr("Repositories on this machine are not available yet."));
    machineNotice_->hide();
    contextLayout->addWidget(machineNotice_);
    chrome_geometry::addSpacing(contextLayout, 2);
    repositoryList_ = new SidebarRepositoryList(context);
    repositoryList_->setObjectName(QStringLiteral("productRepositoryList"));
    connect(repositoryList_, &SidebarRepositoryList::iconPresentationChanged, this,
            &ActivityBar::refreshIcons);
    connect(repositoryList_, &SidebarRepositoryList::repositoryRequested, this,
            &ActivityBar::repositoryActivationRequested);
    connect(repositoryList_, &SidebarRepositoryList::newConversationRequested, this,
            &ActivityBar::newConversationRequested);
    connect(repositoryList_, &SidebarRepositoryList::conversationRequested, this,
            &ActivityBar::conversationRequested);
    connect(repositoryList_, &SidebarRepositoryList::conversationContextMenuRequested, this,
            &ActivityBar::conversationContextMenuRequested);
    connect(repositoryList_, &SidebarRepositoryList::conversationRenameRequested, this,
            &ActivityBar::conversationRenameRequested);
    contextLayout->addWidget(repositoryList_);
    contextLayout->addStretch();
    contextScroll->setWidget(context);
    layout->addWidget(contextScroll, 1);

    updateReadyButton_ = new QToolButton(this);
    updateReadyButton_->setObjectName(QStringLiteral("productUpdateReadyButton"));
    updateReadyButton_->setProperty("kvRole", QStringLiteral("productUpdateReady"));
    updateReadyButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    updateReadyButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    updateReadyButton_->hide();
    connect(updateReadyButton_, &QToolButton::clicked, this, &ActivityBar::updatesRequested);
    layout->addWidget(updateReadyButton_);

    identityButton_ = new QToolButton(this);
    identityButton_->setObjectName(QStringLiteral("productIdentityButton"));
    identityButton_->setProperty("kvRole", QStringLiteral("productIdentity"));
    identityButton_->setText(tr("KetPlus\nPersonal workspace"));
    identityButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    chrome_geometry::fixedHeight(identityButton_, 52);
    identityButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* identityAvatar = new QLabel(QStringLiteral("KP"), identityButton_);
    identityAvatar->setObjectName(QStringLiteral("productIdentityAvatar"));
    identityAvatar->setAlignment(Qt::AlignCenter);
    identityAvatar->setGeometry(7, 11, 29, 29);
    identityAvatar->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* identityChevron = new QLabel(identityButton_);
    identityChevron->setObjectName(QStringLiteral("productIdentityChevron"));
    identityChevron->setProperty("lucideBody", QByteArray(lucide::chevronDown));
    chrome_geometry::fixedSize(identityChevron, 14, 14);
    identityChevron->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* identityRow = new QHBoxLayout(identityButton_);
    identityRow->setSizeConstraint(QLayout::SetNoConstraint);
    chrome_geometry::margins(identityRow, 0, 0, 10, 0);
    identityRow->addStretch();
    identityRow->addWidget(identityChevron);
    // Settings and account entry points live in the user menu instead of the sidebar footer.
    userMenu_ = new QMenu(this);
    userMenu_->setObjectName(QStringLiteral("productUserMenu"));
    auto* settingsAction = userMenu_->addAction(
        tr("Settings") + QLatin1Char('\t') +
        QKeySequence(QKeySequence::Preferences).toString(QKeySequence::NativeText));
    settingsAction->setObjectName(QStringLiteral("productSettingsAction"));
    settingsAction->setIcon(lucideIcon(lucide::settings, QSize(16, 16)));
    connect(settingsAction, &QAction::triggered, this, &ActivityBar::settingsRequested);
    auto* accountAction = userMenu_->addAction(tr("Account & devices"));
    accountAction->setObjectName(QStringLiteral("productAccountAction"));
    accountAction->setIcon(lucideIcon(lucide::unplug, QSize(16, 16)));
    connect(accountAction, &QAction::triggered, this, &ActivityBar::identityRequested);
    auto* updatesAction = userMenu_->addAction(tr("Check for Updates…"));
    updatesAction->setObjectName(QStringLiteral("productCheckForUpdatesAction"));
    updatesAction->setIcon(lucideIcon(lucide::refreshCw, QSize(16, 16)));
    connect(updatesAction, &QAction::triggered, this, &ActivityBar::updatesRequested);
    connect(identityButton_, &QToolButton::clicked, this, [this] {
        const QSize size = userMenu_->sizeHint();
        userMenu_->popup(identityButton_->mapToGlobal(QPoint(4, -size.height() - 4)));
    });
    layout->addWidget(identityButton_);

    resizeHandle_ = new SidebarResizeHandle(this);
    resizeHandle_->raise();

    setMode(Mode::Explorer);
}

void ActivityBar::setPendingUpdate(const QString& version) {
    updateReadyButton_->setText(
        version.isEmpty()
            ? QString()
            : tr("Update available\nKétPlus Pro %1 · Review and install").arg(version));
    updateReadyButton_->setVisible(!version.isEmpty());
}

void ActivityBar::setSidebarWidth(const int width) {
    const int bounded = qBound(kMinWidth, width, kMaxWidth);
    if (bounded == sidebarWidth_)
        return;
    sidebarWidth_ = bounded;
    setFixedWidth(bounded);
    emit sidebarWidthChanged(bounded);
}

void ActivityBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    resizeHandle_->setGeometry(width() - kResizeHandleWidth, 0, kResizeHandleWidth, height());
    resizeHandle_->raise();
}

void ActivityBar::setMode(const Mode mode) {
    mode_ = mode;
    switch (mode) {
    case Mode::Explorer:
    case Mode::Search:
        // Search in Files opens from its shortcut and menu; it belongs to the files workspace.
        explorerButton_->setChecked(true);
        break;
    case Mode::SourceControl:
        sourceControlButton_->setChecked(true);
        break;
    case Mode::AgentWorkspace:
        agentButton_->setChecked(true);
        break;
    case Mode::Atlas:
        group_->setExclusive(false);
        for (auto* button : group_->buttons())
            button->setChecked(false);
        group_->setExclusive(true);
        break;
    case Mode::Remote:
        remoteButton_->setChecked(true);
        break;
    case Mode::Settings:
        // Settings opens from the user menu, so no navigation row stays selected.
        group_->setExclusive(false);
        for (auto* button : group_->buttons())
            button->setChecked(false);
        group_->setExclusive(true);
        break;
    }
    refreshIcons();
}

void ActivityBar::setMachines(const QStringList& ids, const QStringList& names,
                              const QString& selectedId) {
    if (ids.size() != names.size() || ids.isEmpty())
        return;
    const QSignalBlocker blocker(machineSelect_);
    machineSelect_->clear();
    for (int i = 0; i < ids.size(); ++i)
        machineSelect_->addItem(names.at(i), ids.at(i));
    const int selected = machineSelect_->findData(selectedId);
    machineSelect_->setCurrentIndex(selected >= 0 ? selected : 0);
}

void ActivityBar::setRemoteMachineSelected(const bool selected) {
    repositoryAction_->setEnabled(!selected);
    machineNotice_->setVisible(selected);
}

void ActivityBar::setSourceControlAvailable(const bool available) {
    sourceControlButton_->setEnabled(available);
    refreshIcons();
}

void ActivityBar::setSourceControlCount(const int count) {
    sourceControlCount_->setText(QString::number(count));
    sourceControlCount_->setVisible(count > 0);
}

void ActivityBar::setRepositories(const QVector<SidebarRepository>& repositories) {
    repositoryList_->setRepositories(repositories);
    refreshIcons();
}

void ActivityBar::applyPalette(const ThemePalette& palette, const int interfaceFontSizePixels) {
    if (auto* avatar = findChild<QLabel*>(QStringLiteral("productIdentityAvatar"))) {
        const auto px = [interfaceFontSizePixels](int n) {
            return InterfaceZoomMetrics::geometry(n, interfaceFontSizePixels);
        };
        avatar->setGeometry(px(7), px(11), px(29), px(29));
    }
    const QString wordmark = palette.dark ? QStringLiteral(":/brand/ketplus-logo-dark.png")
                                          : QStringLiteral(":/brand/ketplus-logo-light.png");
    const QPixmap source(wordmark);
    if (!source.isNull()) {
        const QSize size = brandWordmark_->size() * brandWordmark_->devicePixelRatioF();
        QPixmap scaled = source.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(brandWordmark_->devicePixelRatioF());
        brandWordmark_->setPixmap(scaled);
    }
    const QString controlBorder = palette.dark ? palette.border : QStringLiteral("#DDDCDE");
    const QString sidebarAccent = palette.accentText;
    const QString navSelectedText =
        palette.dark ? QStringLiteral("#DDE6FF") : QStringLiteral("#102EB3");
    const QString sheet = TypeScale::apply(QStringLiteral(R"(
        QWidget#activityBar { border-right: 1px solid %1; background: %2; }
        QScrollArea#productSidebarContext, QScrollArea#productSidebarContext > QWidget > QWidget,
        QWidget#productSidebarContextBody { border: 0; background: transparent; }
        QScrollBar:vertical { width: 5px; border: 0; background: transparent; }
        QScrollBar::handle:vertical { min-height: 24px; border-radius: 2px; background: %10; }
        QScrollBar[scrolling="false"]::handle:vertical { background: transparent; }
        QScrollBar[scrolling="false"]:hover::handle:vertical { background: %10; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QFrame#productSidebarRule { border: 0; background: %1; }
        QToolButton { text-align: left; }
        QToolButton[kvRole="productSearch"] { min-height: 30px; max-height: 30px; padding: 0 8px; border: 1px solid %11; border-radius: 5px; background: %4; color: %5; font-size: {{caption}}px; }
        QToolButton[kvRole="productSearch"]:hover { border-color: %13; }
        QToolButton[kvRole="productNavButton"] { padding: 0; border: 0; border-radius: 5px; background: transparent; }
        QToolButton[kvRole="productNavButton"]:hover { background: %7; }
        QToolButton[kvRole="productNavButton"]:checked { background: %8; }
        QLabel[kvRole="productNavLabel"] { color: %6; font-size: {{body}}px; font-weight: 400; }
        QLabel[kvRole="productNavLabel"][activeNavigation="true"] { color: %9; }
        QLabel[kvRole="productNavLabel"][disabledNavigation="true"] { color: %10; }
        QLabel[kvRole="productSidebarSection"] { color: %5; font-size: {{caption}}px; font-weight: 500; }
        QToolButton[kvRole="productSectionAction"] { padding: 0; border: 0; border-radius: 4px; background: transparent; }
        QToolButton[kvRole="productSectionAction"]:hover { background: %7; }
        QComboBox#productMachineSelect { min-height:28px; padding:0 18px 0 0; border:0; background:transparent; color:%6; font-size:{{caption}}px; font-weight:600; }
        QComboBox#productMachineSelect:hover { color:%3; }
        QComboBox#productMachineSelect:focus { outline:1px solid %12; }
        QComboBox#productMachineSelect::drop-down { width:17px; border:0; background:transparent; }
        QComboBox#productMachineSelect QAbstractItemView { background:%4; color:%3; border:1px solid %11; selection-background-color:%8; }
        QToolButton[kvRole="productRepositoryGroup"] { padding: 0 0 0 8px; border: 0; background: transparent; color: %5; font-size: {{caption}}px; font-weight: 500; }
        QToolButton[kvRole="productRepositoryGroup"]:hover, QToolButton[kvRole="productRepositoryGroup"][activeRepository="true"] { color: %6; }
        QToolButton[kvRole="productShowMoreConversations"] { padding: 0 0 0 8px; border: 0; background: transparent; color: %5; font-size: {{caption}}px; }
        QToolButton[kvRole="productShowMoreConversations"]:hover { color: %6; }
        QToolButton[kvRole="productConversation"] { padding: 0; border: 0; border-radius: 6px; background: transparent; }
        QToolButton[kvRole="productConversation"]:hover, QToolButton[kvRole="productConversation"]:checked { background: %7; }
        QLabel[kvRole="productConversationTitle"] { color: %3; font-size: {{body}}px; font-weight: 400; }
        QLabel[kvRole="productConversationNumber"] { color: %5; font-size: {{caption}}px; font-weight: 500; }
        QLabel[kvRole="productConversationAgentSender"] { color: %5; font-size: {{caption}}px; font-weight: 400; }
        QMenu#productUserMenu { padding: 4px; border: 1px solid %11; border-radius: 6px; background: %4; }
        QMenu#productUserMenu::item { padding: 5px 18px 5px 8px; border-radius: 4px; color: %3; font-size: {{body}}px; }
        QMenu#productUserMenu::item:selected { background: %7; }
        QToolButton[kvRole="productIdentity"] { padding: 7px 7px 7px 43px; border: 0; border-top: 1px solid %1; border-radius: 0; background: transparent; color: %6; font-size: {{caption}}px; }
        QToolButton[kvRole="productIdentity"]:hover { background: %7; }
        QToolButton[kvRole="productUpdateReady"] { margin: 0 2px 4px 2px; padding: 7px 9px; border: 1px solid %12; border-radius: 6px; background: %8; color: %12; font-size: {{caption}}px; font-weight: 550; }
        QToolButton[kvRole="productUpdateReady"]:hover { background: %7; }
        QLabel#productIdentityAvatar { border: 1px solid %11; border-radius: 14px; background: %4; color: %12; font-size: {{micro}}px; font-weight: 600; }
        QLabel#quickSearchShortcut { color: %5; font-size: {{micro}}px; }
        QLabel#sourceControlCountBadge { border: 1px solid %11; border-radius: 4px; color: %6; font-size: {{micro}}px; }
    )"),
                                           interfaceFontSizePixels, /*biasPixels=*/2);
    setStyleSheet(sheet.arg(
        palette.border, palette.sidebarBackground, palette.textMain, palette.panelBackground,
        palette.textMuted, palette.textSecondary, palette.surfaceHover, palette.accentSubtle,
        navSelectedText, palette.textDisabled, controlBorder, sidebarAccent, palette.borderStrong));
    chrome_geometry::apply(this, interfaceFontSizePixels);
    repositoryList_->applyZoom(interfaceFontSizePixels);
    iconMainColor_ = palette.textMain;
    iconMutedColor_ = palette.textMuted;
    iconDisabledColor_ = palette.textDisabled;
    iconAccentColor_ = sidebarAccent;
    iconPositiveColor_ = palette.positive;
    iconWarningColor_ = palette.warning;
    iconDangerColor_ = palette.danger;
    refreshIcons();
}

void ActivityBar::refreshIcons() {
    for (auto* button : findChildren<QToolButton*>()) {
        const auto body = button->property("lucideBody").toByteArray();
        if (body.isEmpty())
            continue;
        QString color = iconMutedColor_;
        if (button->property("lucideColorRole").toString() == QStringLiteral("main"))
            color = iconMainColor_;
        if (group_->buttons().contains(button) && button->isChecked())
            color = iconAccentColor_;
        button->setIcon(lucideIcon(body, button->iconSize(), QColor(color)));
    }
    for (auto* label : findChildren<QLabel*>()) {
        const auto body = label->property("lucideBody").toByteArray();
        if (label->property("kvRole").toString() == QStringLiteral("productNavLabel")) {
            auto* button = qobject_cast<QToolButton*>(label->parentWidget());
            label->setProperty("activeNavigation", button != nullptr && button->isChecked());
            label->setProperty("disabledNavigation", button != nullptr && !button->isEnabled());
            label->style()->unpolish(label);
            label->style()->polish(label);
        }
        if (body.isEmpty())
            continue;
        const QString role = label->property("lucideColorRole").toString();
        QString color = iconMutedColor_;
        auto* navigationButton = qobject_cast<QToolButton*>(label->parentWidget());
        if (label->objectName() == QStringLiteral("productNavIcon") && navigationButton != nullptr)
            color = navigationButton->isEnabled()
                        ? (navigationButton->isChecked() ? iconAccentColor_ : iconMutedColor_)
                        : iconDisabledColor_;
        else if (role == QStringLiteral("accent"))
            color = iconAccentColor_;
        else if (role == QStringLiteral("main"))
            color = iconMainColor_;
        else if (role == QStringLiteral("positive"))
            color = iconPositiveColor_;
        else if (role == QStringLiteral("warning"))
            color = iconWarningColor_;
        else if (role == QStringLiteral("danger"))
            color = iconDangerColor_;
        label->setPixmap(lucide_icon::pixmap(body, QColor(color), label->size()));
    }
}

ActivityBar::Mode ActivityBar::mode() const noexcept { return mode_; }

QToolButton* ActivityBar::addModeButton(const QByteArray& svgBody, const QString& label,
                                        const Mode mode) {
    auto* button = new QToolButton(this);
    button->setToolTip(label);
    button->setAccessibleName(label);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setCheckable(true);
    button->setProperty("kvRole", QStringLiteral("productNavButton"));
    button->setProperty("activityMode", static_cast<int>(mode));
    chrome_geometry::fixedHeight(button, kRowHeight);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* row = new QHBoxLayout(button);
    chrome_geometry::margins(row, 8, 0, 8, 0);
    chrome_geometry::spacing(row, 8);
    auto* icon = new QLabel(button);
    icon->setObjectName(QStringLiteral("productNavIcon"));
    icon->setProperty("lucideBody", svgBody);
    chrome_geometry::fixedSize(icon, 15, 15);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* text = new QLabel(label, button);
    text->setProperty("kvRole", QStringLiteral("productNavLabel"));
    text->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(icon);
    row->addWidget(text);
    row->addStretch();
    group_->addButton(button);
    connect(button, &QToolButton::clicked, this, [this, mode] {
        mode_ = mode;
        emit modeRequested(mode);
    });
    return button;
}

} // namespace ketplus
