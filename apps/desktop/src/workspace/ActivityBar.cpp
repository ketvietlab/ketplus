#include "workspace/ActivityBar.h"
#include "ui/ChromeGeometry.h"
#include "ui/InterfaceZoom.h"
#include "ui/SymbolIcon.h"
#include <QApplication>

#include "KetBanner.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "KetNavItem.h"
#include "KetSelect.h"
#include "KetUpdateCard.h"
#include "ui/Theme.h"
#include "ui/TypeScale.h"
#include "workspace/SymbolIcons.h"

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
            bar_->setSidebarWidth(startWidth_ + qRound((event->globalPosition().x() - startX_) /
                                                       InterfaceZoomMetrics::factor(
                                                           chrome_geometry::zoomPercent(bar_))));
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
QIcon symbolIcon(const QByteArray& body, const QSize& size,
                 const QColor& color = QApplication::palette().color(QPalette::PlaceholderText)) {
    return QIcon(symbol_icon::pixmap(body, color, size));
}

} // namespace

ActivityBar::ActivityBar(QWidget* parent) : QWidget(parent), group_(new QButtonGroup(this)) {
    setObjectName(QStringLiteral("activityBar"));
    setProperty("kvRole", QStringLiteral("productSidebar"));
    setAttribute(Qt::WA_StyledBackground, true);
    sidebarWidth_ = qBound(kMinWidth, QSettings().value(kWidthSetting, kWidth).toInt(), kMaxWidth);
    chrome_geometry::fixedWidth(this, sidebarWidth_);
    shownWidth_ = width();

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

    quickSearchButton_ =
        new ket::NavItem(tr("Quick search"), ket::Icons::codeForLucide(u"search"), this);
    quickSearchButton_->setObjectName(QStringLiteral("quickSearchButton"));
    quickSearchButton_->setCheckable(false);
    quickSearchButton_->setShortcutText(QStringLiteral("⌘ K"));
    connect(quickSearchButton_, &QAbstractButton::clicked, this,
            &ActivityBar::quickSearchRequested);
    layout->addWidget(quickSearchButton_);
    chrome_geometry::addSpacing(layout, 12);

    agentButton_ =
        addModeButton(symbols::messagesSquare, tr("Agent Workspace"), Mode::AgentWorkspace);
    explorerButton_ = addModeButton(symbols::folder, tr("Files & Editor"), Mode::Explorer);
    sourceControlButton_ =
        addModeButton(symbols::gitBranch, tr("Source Control"), Mode::SourceControl);
    remoteButton_ = addModeButton(symbols::unplug, tr("Connections"), Mode::Remote);
    explorerButton_->setObjectName(QStringLiteral("explorerActivityButton"));
    sourceControlButton_->setObjectName(QStringLiteral("sourceControlActivityButton"));
    agentButton_->setObjectName(QStringLiteral("agentWorkspaceActivityButton"));
    remoteButton_->setObjectName(QStringLiteral("remoteActivityButton"));
    const QList<ket::NavItem*> navigationButtons = {agentButton_, explorerButton_,
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
    repositoryAction_->setProperty("symbolName", QByteArray(symbols::plus));
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
    tagFilter_ = new ket::Select(context);
    tagFilter_->setObjectName(QStringLiteral("conversationTagFilter"));
    tagFilter_->setAccessibleName(tr("Filter conversations by tag"));
    tagFilter_->hide();
    connect(tagFilter_, &QComboBox::currentIndexChanged, this,
            [this] { repositoryList_->setTagFilter(tagFilter_->currentData().toString()); });
    contextLayout->addWidget(tagFilter_);
    contextLayout->addWidget(repositoryList_);
    contextLayout->addStretch();
    contextScroll->setWidget(context);
    layout->addWidget(contextScroll, 1);

    storageNotice_ = new QWidget(this);
    storageNotice_->setObjectName(QStringLiteral("sidebarStorageWarning"));
    auto* storageLayout = new QVBoxLayout(storageNotice_);
    storageLayout->setContentsMargins(0, 0, 0, 0);
    storageLayout->setSpacing(4);
    storageBanner_ =
        new ket::Banner(ket::BannerTone::Warning, tr("Journal almost full"), {}, storageNotice_);
    storageLayout->addWidget(storageBanner_);
    auto* storageSettings =
        new ket::Button(tr("Storage settings"), ket::ButtonVariant::Ghost, storageNotice_);
    storageSettings->setObjectName(QStringLiteral("sidebarStorageSettings"));
    connect(storageSettings, &QAbstractButton::clicked, this,
            [this] { emit settingsSectionRequested(QStringLiteral("storage")); });
    storageLayout->addWidget(storageSettings);
    storageNotice_->hide();
    layout->addWidget(storageNotice_);

    updateReadyButton_ = new ket::UpdatePill(tr("Update ready"), {}, this);
    updateReadyButton_->setObjectName(QStringLiteral("productUpdateReadyButton"));
    updateReadyButton_->setProperty("kvRole", QStringLiteral("productUpdateReady"));
    updateReadyButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    updateReadyButton_->hide();
    connect(updateReadyButton_, &QAbstractButton::clicked, this, &ActivityBar::updatesRequested);
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
    identityChevron->setProperty("symbolName", QByteArray(symbols::chevronDown));
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
    settingsAction->setIcon(symbolIcon(symbols::settings, QSize(16, 16)));
    connect(settingsAction, &QAction::triggered, this, &ActivityBar::settingsRequested);
    auto* accountAction = userMenu_->addAction(tr("Account & devices"));
    accountAction->setObjectName(QStringLiteral("productAccountAction"));
    accountAction->setIcon(symbolIcon(symbols::unplug, QSize(16, 16)));
    connect(accountAction, &QAction::triggered, this, &ActivityBar::identityRequested);
    auto* updatesAction = userMenu_->addAction(tr("Check for Updates…"));
    updatesAction->setObjectName(QStringLiteral("productCheckForUpdatesAction"));
    updatesAction->setIcon(symbolIcon(symbols::refreshCw, QSize(16, 16)));
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

void ActivityBar::setStorageStatus(const QVariantMap& result) {
    const auto status = result.value("status").toMap();
    const bool warning = result.value("ok").toBool() && status.value("warning").toBool();
    storageNotice_->setVisible(warning);
    if (!warning)
        return;
    storageBanner_->setTitle(status.value("over_limit").toBool() ? tr("Journal limit reached")
                                                                 : tr("Journal almost full"));
    storageBanner_->setMessage(
        status.value("pressure_enabled").toBool()
            ? tr("Older tool details can be trimmed. Messages stay available.")
            : tr("Review the size limit and detail retention in Settings."));
}

void ActivityBar::setPendingUpdate(const QString& version) {
    updateReadyButton_->setDetail(tr("KétPlus Pro %1 · Review and install").arg(version));
    updateReadyButton_->setVisible(!version.isEmpty());
}

void ActivityBar::setSidebarWidth(const int unscaledWidth) {
    const int bounded = qBound(kMinWidth, unscaledWidth, kMaxWidth);
    if (bounded == sidebarWidth_)
        return;
    sidebarWidth_ = bounded;
    chrome_geometry::fixedWidth(this, bounded);
    syncShownWidth();
}

void ActivityBar::syncShownWidth() {
    if (width() == shownWidth_)
        return;
    shownWidth_ = width();
    emit sidebarWidthChanged(shownWidth_);
}

void ActivityBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    syncShownWidth();
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
    sourceControlButton_->setCount(count > 0 ? std::optional<int>(count) : std::nullopt);
}

void ActivityBar::setTagFilter(const QString& tag) {
    const int index = tagFilter_->findData(tag);
    if (index >= 0)
        tagFilter_->setCurrentIndex(index);
}

void ActivityBar::setRepositories(const QVector<SidebarRepository>& repositories) {
    repositoryList_->setRepositories(repositories);
    QStringList labels;
    for (const auto& repository : repositories)
        for (const auto& conversation : repository.conversations)
            for (const auto& value : conversation.toMap().value("tags").toList()) {
                const QString label =
                    value.toMap().value("label", value.toString()).toString().trimmed();
                if (!label.isEmpty() && !labels.contains(label))
                    labels.append(label);
            }
    labels.sort(Qt::CaseInsensitive);
    const QString selected = tagFilter_->currentData().toString();
    const QSignalBlocker block(tagFilter_);
    tagFilter_->clear();
    tagFilter_->addItem(tr("All tags"), QString());
    for (const auto& label : labels)
        tagFilter_->addItem(label, label);
    tagFilter_->setCurrentIndex(qMax(0, tagFilter_->findData(selected)));
    tagFilter_->setVisible(!labels.isEmpty());
    repositoryList_->setTagFilter(tagFilter_->currentData().toString());
}

ket::NavItem* ActivityBar::addModeButton(const QByteArray& symbol, const QString& label,
                                         const Mode mode) {
    auto* button =
        new ket::NavItem(label, ket::Icons::codeForLucide(QString::fromLatin1(symbol)), this);
    button->setToolTip(label);
    button->setAccessibleName(label);
    button->setProperty("activityMode", static_cast<int>(mode));
    group_->addButton(button);
    connect(button, &QAbstractButton::clicked, this, [this, mode] {
        mode_ = mode;
        emit modeRequested(mode);
    });
    return button;
}

} // namespace ketplus
