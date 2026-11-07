#include "app/AtlasBuilderTab.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "KetTokens.h"
#include "ui/ChromeGeometry.h"
#include "ui/SymbolIcon.h"

#include "agent/AgentMessageModel.h"
#include "agent/AgentWorkspaceView.h"
#include "atlas/AtlasTab.h"
#include "ui/TypeScale.h"

#include <QApplication>
#include <QDirIterator>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

namespace ketplus {
namespace {
constexpr int placeholderMaximumWidth = 440;

QString builderStyle(const QPalette& palette, const int basePixels) {
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    const auto theme = dark ? ket::Theme::Dark : ket::Theme::Light;
    const auto color = [theme](QStringView name) {
        return ket::Tokens::color(name, theme).name(QColor::HexArgb);
    };
    const QString page = color(u"page-bg");
    const QString panel = color(u"panel-bg");
    const QString raised = color(u"surface-raised");
    const QString text = color(u"text-main");
    const QString muted = color(u"text-muted");
    const QString border = color(u"panel-border");
    const QString strong = color(u"border-strong");
    // Két Design System accent-text and accent-subtle (Két Blue).
    const QString accent = color(u"accent-text");
    const QString accentSubtle = color(u"accent-subtle");
    auto sheet = TypeScale::apply(QStringLiteral(R"QSS(
QWidget#atlasBuilderTab, QWidget#atlasBuilderPlaceholder { background:%1; color:%4; }
QWidget#atlasBuilderToolbar { min-height:44px; max-height:44px; background:%2; border-bottom:1px solid %6; }
QLabel#atlasBuilderToolbarTitle { color:%4; font-size:{{reading}}px; font-weight:600; }
QLabel#atlasBuilderToolbarSubtitle { color:%5; font-size:{{caption}}px; }
QFrame#atlasBuilderChatFrame { background:%2; border:0; border-left:1px solid %6; }
QWidget#atlasBuilderChatHeader { min-height:43px; background:%2; border-bottom:1px solid %6; }
QLabel#atlasBuilderChatTitle { color:%4; font-size:{{body}}px; font-weight:600; }
QLabel#atlasBuilderFlowContext { color:%5; font-size:{{micro}}px; }
QLabel#atlasBuilderPlaceholderIcon { color:%8; font-size:{{glyphLarge}}px; }
QLabel#atlasBuilderPlaceholderEyebrow { color:%8; font-size:{{micro}}px; font-weight:600; letter-spacing:1px; }
QLabel#atlasBuilderPlaceholderTitle { color:%4; font-size:{{title}}px; font-weight:600; }
QLabel#atlasBuilderExplanation { color:%5; font-size:{{body}}px; }
QLabel#atlasBuilderGuidance { color:%5; font-size:{{body}}px; }
QLabel#atlasBuilderSamplePrompt { color:%5; font-size:{{caption}}px; padding:13px; border:1px solid %6; border-radius:7px; background:%3; }
)QSS"),
                                  basePixels);
    const QStringList colors{page,   panel,  raised, text,        muted,
                             border, strong, accent, accentSubtle};
    for (int i = 0; i < colors.size(); ++i)
        sheet.replace(QStringLiteral("%") + QString::number(i + 1), colors[i]);
    return sheet;
}

QPixmap networkPixmap(const QColor& color) {
    return symbol_icon::pixmap("network", color, QSize(35, 35));
}

quint64 bundleFingerprint(const QString& directory) {
    quint64 fingerprint = 1469598103934665603ULL;
    QDirIterator entries(directory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QFileInfo info(entries.next());
        fingerprint ^= qHash(info.absoluteFilePath());
        fingerprint *= 1099511628211ULL;
        fingerprint ^= static_cast<quint64>(info.size());
        fingerprint *= 1099511628211ULL;
        fingerprint ^= static_cast<quint64>(info.lastModified().toMSecsSinceEpoch());
        fingerprint *= 1099511628211ULL;
    }
    return fingerprint;
}
} // namespace

AtlasBuilderTab::AtlasBuilderTab(AtlasIndex* index, const QString& workspaceRoot, QWidget* parent)
    : QWidget(parent), index_(index), atlas_(new AtlasTab(index, this)),
      placeholder_(new QWidget(this)), chatFrame_(new QFrame(this)),
      chat_(new AgentWorkspaceView(chatFrame_)),
      collapse_(
          new ket::IconButton(ket::Icons::icon(ket::Icons::codeForLucide(u"panel-right-close")),
                              tr("Hide AI Chat"), chatFrame_)),
      workspaceRoot_(workspaceRoot) {
    setObjectName(QStringLiteral("atlasBuilderTab"));
    applyPalette(QApplication::palette());
    setMinimumSize(680, 420);
    toolbar_ = new QWidget(this);
    toolbar_->setObjectName(QStringLiteral("atlasBuilderToolbar"));
    auto* toolbarLayout = new QHBoxLayout(toolbar_);
    chrome_geometry::margins(toolbarLayout, 10, 0, 12, 0);
    auto* back = new ket::IconButton(ket::Icons::icon(ket::Icons::codeForLucide(u"arrow-left")),
                                     tr("Back to the conversation"), toolbar_);
    back->setObjectName(QStringLiteral("atlasBackToConversation"));
    toolbarLayout->addWidget(back);
    connect(back, &QAbstractButton::clicked, this,
            [this] { emit conversationRequested(workspaceRoot_, conversationId_); });
    auto* toolbarCopy = new QVBoxLayout;
    chrome_geometry::spacing(toolbarCopy, 1);
    auto* toolbarTitle = new QLabel(tr("Atlas Builder"), toolbar_);
    toolbarTitle->setObjectName(QStringLiteral("atlasBuilderToolbarTitle"));
    toolbarCopy->addWidget(toolbarTitle);
    auto* toolbarSubtitle = new QLabel(tr("Interactive screens and connected flows"), toolbar_);
    toolbarSubtitle->setObjectName(QStringLiteral("atlasBuilderToolbarSubtitle"));
    toolbarCopy->addWidget(toolbarSubtitle);
    connect(this, &AtlasBuilderTab::titleChanged, toolbarTitle, &QLabel::setText);
    connect(this, &AtlasBuilderTab::titleChanged, this, [this, toolbarSubtitle] {
        for (const auto& entry : index_->atlases()) {
            if (entry.path == atlasPath()) {
                toolbarSubtitle->setText(
                    tr("%1 flows · %2 screens").arg(entry.flowCount).arg(entry.screens.size()));
                return;
            }
        }
    });
    toolbarLayout->addLayout(toolbarCopy);
    toolbarLayout->addStretch();
    aiButton_ = new ket::Button(tr("Hide AI Chat"), ket::ButtonVariant::Secondary, toolbar_);
    static_cast<ket::Button*>(aiButton_)->setControlSize(ket::ControlSize::Small);
    aiButton_->setObjectName(QStringLiteral("atlasBuilderAiButton"));
    aiButton_->setText(tr("Hide AI Chat"));
    aiButton_->setCheckable(true);
    aiButton_->setChecked(true);
    toolbarLayout->addWidget(aiButton_);
    connect(aiButton_, &QAbstractButton::clicked, this,
            [this] { setChatCollapsed(!chatCollapsed_); });
    activityPulse_.setInterval(650);
    connect(&activityPulse_, &QTimer::timeout, this, [this] {
        pulseBright_ = !pulseBright_;
        updateAiButton();
    });
    connect(chat_, &AgentWorkspaceView::sessionChanged, this, &AtlasBuilderTab::updateAiButton);
    connect(chat_, &AgentWorkspaceView::messageModelChanged, this,
            &AtlasBuilderTab::observeMessages);
    observeMessages();

    for (const auto& descriptor : index_->atlases())
        initialAtlasPaths_.insert(descriptor.path);

    atlas_->setObjectName(QStringLiteral("atlasBuilderPreview"));
    atlas_->hide();

    placeholder_->setObjectName(QStringLiteral("atlasBuilderPlaceholder"));
    auto* placeholderLayout = new QVBoxLayout(placeholder_);
    chrome_geometry::margins(placeholderLayout, 32, 32, 432, 32);
    placeholderLayout->addStretch();

    placeholderContent_ = new QWidget(placeholder_);
    placeholderContent_->setObjectName(QStringLiteral("atlasBuilderPlaceholderContent"));
    placeholderContent_->setMaximumWidth(placeholderMaximumWidth);
    placeholderContent_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* contentLayout = new QVBoxLayout(placeholderContent_);
    chrome_geometry::margins(contentLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(contentLayout, 14);

    placeholderIcon_ = new QLabel(placeholderContent_);
    placeholderIcon_->setObjectName(QStringLiteral("atlasBuilderPlaceholderIcon"));
    placeholderIcon_->setAlignment(Qt::AlignCenter);
    placeholderIcon_->setPixmap(networkPixmap(palette().color(QPalette::Highlight)));
    contentLayout->addWidget(placeholderIcon_);
    auto* eyebrow = new QLabel(tr("YOUR IDEAS, CONNECTED"), placeholderContent_);
    eyebrow->setObjectName(QStringLiteral("atlasBuilderPlaceholderEyebrow"));
    eyebrow->setAlignment(Qt::AlignCenter);
    contentLayout->addWidget(eyebrow);
    auto* title = new QLabel(tr("Your Atlas will appear here."), placeholderContent_);
    title->setObjectName(QStringLiteral("atlasBuilderPlaceholderTitle"));
    auto titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 7);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);
    contentLayout->addWidget(title);

    explanation_ = new QLabel(tr("Describe an experience in AI Chat. Your Agent will map the "
                                 "screens, states, and journeys in this workspace."),
                              placeholderContent_);
    explanation_->setObjectName(QStringLiteral("atlasBuilderExplanation"));
    explanation_->setWordWrap(true);
    explanation_->setAlignment(Qt::AlignCenter);
    explanation_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    contentLayout->addWidget(explanation_);

    guidance_ = new QLabel(tr("Screens   · · ·   States   · · ·   Journeys"), placeholderContent_);
    guidance_->setObjectName(QStringLiteral("atlasBuilderGuidance"));
    guidance_->setWordWrap(true);
    guidance_->setAlignment(Qt::AlignCenter);
    guidance_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    contentLayout->addWidget(guidance_);

    samplePrompt_ = new QLabel(tr("<b>Try:</b> “Design onboarding for KetPlus Desktop using Két "
                                  "Design System.”"),
                               placeholderContent_);
    samplePrompt_->setObjectName(QStringLiteral("atlasBuilderSamplePrompt"));
    samplePrompt_->setTextFormat(Qt::RichText);
    samplePrompt_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    samplePrompt_->setWordWrap(true);
    samplePrompt_->setAlignment(Qt::AlignCenter);
    samplePrompt_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    contentLayout->addWidget(samplePrompt_);

    placeholderLayout->addWidget(placeholderContent_, 0, Qt::AlignCenter);
    placeholderLayout->addStretch();

    chatFrame_->setObjectName(QStringLiteral("atlasBuilderChatFrame"));
    chatFrame_->setFrameShape(QFrame::StyledPanel);
    chatFrame_->setAutoFillBackground(true);
    chatFrame_->setAttribute(Qt::WA_NativeWindow);

    auto* chatLayout = new QVBoxLayout(chatFrame_);
    chrome_geometry::margins(chatLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(chatLayout, 0);
    auto* chatHeader = new QWidget(chatFrame_);
    chatHeader->setObjectName(QStringLiteral("atlasBuilderChatHeader"));
    auto* headerLayout = new QHBoxLayout(chatHeader);
    chrome_geometry::margins(headerLayout, 12, 6, 8, 6);
    auto* chatTitle = new QLabel(tr("AI Chat"), chatHeader);
    chatTitle->setObjectName(QStringLiteral("atlasBuilderChatTitle"));
    auto chatTitleFont = chatTitle->font();
    chatTitleFont.setBold(true);
    chatTitle->setFont(chatTitleFont);
    headerLayout->addWidget(chatTitle);
    flowContext_ = new QLabel(tr("@Flow · Entire Atlas"), chatHeader);
    flowContext_->setObjectName(QStringLiteral("atlasBuilderFlowContext"));
    flowContext_->setToolTip(
        tr("Preferred context only. Shared screens and styles may affect other flows."));
    flowContext_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    headerLayout->addWidget(flowContext_, 1, Qt::AlignRight);
    collapse_->setObjectName(QStringLiteral("atlasBuilderChatCollapse"));
    collapse_->setToolTip(tr("Hide chat — AI keeps running"));
    collapse_->setAccessibleName(tr("Hide Atlas Builder chat"));
    headerLayout->addWidget(collapse_);
    chatLayout->addWidget(chatHeader);
    chat_->setCompactMode(true);
    // The main window's Edit > Find actions route to this chat while it has focus.
    chat_->setFindActionsProvided(true);
    chrome_geometry::minimumWidth(chat_, 0);
    chatLayout->addWidget(chat_, 1);

    connect(collapse_, &QAbstractButton::clicked, this,
            [this] { setChatCollapsed(!chatCollapsed_); });
    connect(index_, &AtlasIndex::changed, this, &AtlasBuilderTab::detectCreatedAtlas);
    connect(atlas_, &AtlasTab::flowContextChanged, this,
            [this](const QString&, const QString& flowId, const QString& title) {
                flowId_ = flowId;
                flowTitle_ = title;
                flowContext_->setText(flowId.isEmpty()
                                          ? tr("@Flow · Entire Atlas")
                                          : tr("@Flow · %1").arg(title.isEmpty() ? flowId : title));
            });
    connect(atlas_, &AtlasTab::atlasRequested, this, [this](const AtlasDescriptor& descriptor) {
        flowId_.clear();
        flowTitle_.clear();
        atlas_->openAtlas(descriptor);
        watchBundle();
        atlas_->setActive(active_);
        updateGeometry();
        chatFrame_->raise();
        emit titleChanged(descriptor.title);
        emit atlasBuilt(descriptor);
    });
    previewRefresh_.setObjectName(QStringLiteral("atlasBuilderRefreshTimer"));
    previewRefresh_.setParent(this);
    previewRefresh_.setSingleShot(true);
    previewRefresh_.setInterval(750);
    bundleVerification_.setParent(this);
    bundleVerification_.setInterval(250);
    const auto changed = [this] { previewRefresh_.start(); };
    connect(&bundleWatcher_, &QFileSystemWatcher::fileChanged, this, changed);
    connect(&bundleWatcher_, &QFileSystemWatcher::directoryChanged, this, changed);
    connect(&previewRefresh_, &QTimer::timeout, this, &AtlasBuilderTab::refreshPreview);
    connect(&bundleVerification_, &QTimer::timeout, this, [this] {
        if (!built_ || atlasPath().isEmpty())
            return;
        const auto fingerprint = bundleFingerprint(QFileInfo(atlasPath()).absolutePath());
        if (fingerprint == bundleFingerprint_)
            return;
        bundleFingerprint_ = fingerprint;
        previewRefresh_.start(0);
    });
}

QString AtlasBuilderTab::atlasPath() const { return built_ ? atlas_->atlasPath() : QString{}; }

QVariantMap AtlasBuilderTab::promptContext() const {
    QVariantMap context{{QStringLiteral("atlasConversationId"), conversationId_}};
    if (built_) {
        context.insert(QStringLiteral("atlasId"), atlas_->atlasId());
        context.insert(QStringLiteral("atlasPath"), atlasPath());
    }
    if (!flowId_.isEmpty()) {
        context.insert(QStringLiteral("atlasFlowId"), flowId_);
        context.insert(QStringLiteral("atlasFlowTitle"), flowTitle_);
    }
    return context;
}

void AtlasBuilderTab::setConversationId(const QString& conversationId) {
    conversationId_ = conversationId;
}

void AtlasBuilderTab::openExistingAtlas(const AtlasDescriptor& descriptor) {
    if (descriptor.path.isEmpty() || QFileInfo(descriptor.workspaceRoot).canonicalFilePath() !=
                                         QFileInfo(workspaceRoot_).canonicalFilePath())
        return;
    built_ = true;
    placeholder_->hide();
    updateGeometry();
    atlas_->show();
    atlas_->openAtlas(descriptor);
    watchBundle();
    atlas_->setActive(active_);
    updateGeometry();
    chatFrame_->raise();
    emit titleChanged(descriptor.title);
}

void AtlasBuilderTab::applyPalette(const QPalette& palette, const int interfaceFontSizePixels) {
    if (applyingStyle_)
        return;
    applyingStyle_ = true;
    interfaceFontSizePixels_ = interfaceFontSizePixels;
    setStyleSheet(builderStyle(palette, interfaceFontSizePixels));
    chrome_geometry::apply(this, interfaceFontSizePixels);
    atlas_->applyPalette(palette, interfaceFontSizePixels);
    if (placeholderIcon_)
        placeholderIcon_->setPixmap(networkPixmap(palette.color(QPalette::Highlight)));
    applyingStyle_ = false;
    if (toolbar_)
        updateGeometry();
}

void AtlasBuilderTab::setActive(const bool active) {
    active_ = active;
    atlas_->setActive(active && built_);
    if (active && built_)
        bundleVerification_.start();
    else
        bundleVerification_.stop();
}

void AtlasBuilderTab::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateGeometry();
}

void AtlasBuilderTab::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    updateAiButton();
}

void AtlasBuilderTab::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    activityPulse_.stop();
}

void AtlasBuilderTab::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange ||
        event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::FontChange) {
        const int basePixels = QApplication::font().pixelSize() > 0
                                   ? QApplication::font().pixelSize()
                                   : InterfaceZoomMetrics::mediumPixels;
        applyPalette(QApplication::palette(), basePixels);
    }
    QWidget::changeEvent(event);
}

void AtlasBuilderTab::detectCreatedAtlas() {
    if (built_)
        return;
    for (const auto& descriptor : index_->atlases()) {
        if (QFileInfo(descriptor.workspaceRoot).canonicalFilePath() !=
                QFileInfo(workspaceRoot_).canonicalFilePath() ||
            initialAtlasPaths_.contains(descriptor.path))
            continue;
        built_ = true;
        placeholder_->hide();
        // Size the native WebView before showing it; it must never cover chat.
        updateGeometry();
        atlas_->show();
        atlas_->openAtlas(descriptor);
        watchBundle();
        atlas_->setActive(active_);
        updateGeometry();
        chatFrame_->raise();
        emit titleChanged(descriptor.title);
        emit atlasBuilt(descriptor);
        return;
    }
}

void AtlasBuilderTab::updateGeometry() {
    const int width = this->width();
    const int toolbarHeight = qMax(40, toolbar_->sizeHint().height());
    toolbar_->setGeometry(0, 0, width, toolbarHeight);
    const int height = qMax(0, this->height() - toolbarHeight);
    const auto px = [this](int value) { return qRound(value * interfaceFontSizePixels_ / 14.0); };
    const int chatWidth = chatCollapsed_ ? 0 : qMin(px(410), qMax(px(300), width / 3));
    const int inset = 0;
    if (!chatCollapsed_)
        chatFrame_->setGeometry(qMax(0, width - chatWidth - inset), toolbarHeight + inset,
                                chatWidth, qMax(0, height - inset * 2));
    if (!built_) {
        if (auto* layout = qobject_cast<QVBoxLayout*>(placeholder_->layout()))
            layout->setContentsMargins(32, 32, chatCollapsed_ ? 32 : chatWidth + inset * 2 + 32,
                                       32);
        placeholder_->setGeometry(0, toolbarHeight, width, height);
        updatePlaceholderTextHeights();
        if (!chatCollapsed_)
            chatFrame_->raise();
        return;
    }
    atlas_->setGeometry(0, toolbarHeight, qMax(0, width - chatWidth), height);
    if (!chatCollapsed_)
        chatFrame_->raise();
}

void AtlasBuilderTab::watchBundle() {
    const auto directory = QFileInfo(atlasPath()).absolutePath();
    QSet<QString> desired{directory};
    QDirIterator entries(directory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks,
                         QDirIterator::Subdirectories);
    while (entries.hasNext())
        desired.insert(entries.next());
    const auto paths = bundleWatcher_.directories() + bundleWatcher_.files();
    const QSet<QString> current(paths.begin(), paths.end());
    const auto removed = current - desired;
    const auto added = desired - current;
    if (!removed.isEmpty())
        bundleWatcher_.removePaths(removed.values());
    if (!added.isEmpty())
        bundleWatcher_.addPaths(added.values());
    if (!bundleVerification_.isActive()) {
        bundleFingerprint_ = bundleFingerprint(directory);
        if (active_)
            bundleVerification_.start();
    }
}

void AtlasBuilderTab::refreshPreview() {
    watchBundle(); // Also re-register files replaced by atomic saves/new directories.
    QFile manifest(atlasPath());
    if (!manifest.open(QIODevice::ReadOnly))
        return;
    const auto config = QJsonDocument::fromJson(manifest.readAll()).object();
    // Retain the last preview while the agent is midway through writing JSON.
    if (config.value("version").toInt() != 1 || !config.value("title").isString() ||
        !config.value("screens").isArray() || !config.value("flows").isArray())
        return;
    emit titleChanged(config.value("title").toString());
    atlas_->refreshPreview();
}

void AtlasBuilderTab::updatePlaceholderTextHeights() {
    auto* placeholderLayout = placeholder_->layout();
    const int horizontalMargins =
        placeholderLayout->contentsMargins().left() + placeholderLayout->contentsMargins().right();
    const int contentWidth =
        qMin(placeholderMaximumWidth, qMax(1, placeholder_->width() - horizontalMargins));
    placeholderContent_->setFixedWidth(contentWidth);
    const auto fitTextHeight = [contentWidth](QLabel* label) {
        chrome_geometry::minimumHeight(label, 0);
        label->setMaximumHeight(QWIDGETSIZE_MAX);
        label->setFixedHeight(
            qMax(label->fontMetrics().height(), label->heightForWidth(contentWidth)) + 2);
    };
    fitTextHeight(explanation_);
    fitTextHeight(guidance_);
    fitTextHeight(samplePrompt_);
    chrome_geometry::minimumHeight(placeholderContent_, 0);
    placeholderContent_->setMaximumHeight(QWIDGETSIZE_MAX);
    auto* contentLayout = placeholderContent_->layout();
    contentLayout->invalidate();
    contentLayout->activate();
    placeholderContent_->setFixedHeight(contentLayout->sizeHint().height());
    placeholderLayout->invalidate();
    placeholderLayout->activate();
}

void AtlasBuilderTab::setChatCollapsed(const bool collapsed) {
    chatCollapsed_ = collapsed;
    chatFrame_->setVisible(!collapsed);
    aiButton_->setChecked(!collapsed);
    updateAiButton();
    updateGeometry();
}

void AtlasBuilderTab::observeMessages() {
    disconnect(messageChanged_);
    disconnect(messageCountChanged_);
    auto* model = chat_->messageModel();
    messageChanged_ =
        connect(model, &QAbstractItemModel::dataChanged, this, &AtlasBuilderTab::updateAiButton);
    messageCountChanged_ =
        connect(model, &AgentMessageModel::countChanged, this, &AtlasBuilderTab::updateAiButton);
    updateAiButton();
}

void AtlasBuilderTab::updateAiButton() {
    bool needsAnswer = false;
    auto* model = chat_->messageModel();
    for (int row = 0; row < model->count(); ++row) {
        const auto message = model->get(row);
        if (message.value("subtype") == "question" && message.value("running").toBool()) {
            needsAnswer = true;
            break;
        }
    }
    const bool running = chat_->sessionRunning();
    if (running && !needsAnswer && isVisible()) {
        if (!activityPulse_.isActive())
            activityPulse_.start();
    } else {
        activityPulse_.stop();
    }
    aiButton_->setText(needsAnswer      ? tr("AI Chat · Needs answer")
                       : chatCollapsed_ ? tr("Show AI Chat")
                                        : tr("Hide AI Chat"));
    const QString action =
        chatCollapsed_ ? tr("Show Atlas Builder chat") : tr("Hide chat — AI keeps running");
    const QString state = needsAnswer ? tr("Waiting for your answer")
                          : running   ? tr("AI is working")
                                      : tr("AI is idle");
    aiButton_->setToolTip(action + "\n" + state);
    aiButton_->setAccessibleName(action + ". " + state);
    QPixmap indicator(12, 12);
    indicator.fill(Qt::transparent);
    QPainter painter(&indicator);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor color = palette().color(needsAnswer || running ? QPalette::Highlight : QPalette::Mid);
    if (running && !needsAnswer && !pulseBright_)
        color.setAlpha(90);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QRectF(2, 2, 8, 8));
    painter.end();
    aiButton_->setIcon(QIcon(indicator));
}

} // namespace ketplus
