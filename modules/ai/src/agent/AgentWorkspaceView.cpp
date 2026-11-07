#include "agent/AgentWorkspaceView.h"
#include "KetRichRecipes.h"
#include "agent/AgentMessageFormatter.h"
#include "preview/MermaidDiagramDialog.h"
#include "preview/MermaidRenderer.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include "agent/CodexAppServer.h"
#include "agent/FileReveal.h"
#include "agent/TranscriptTrace.h"
#include "app/AppearanceSettings.h"
#include "ui/InterfaceFont.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "ui/TypeScale.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMetaObject>
#include <QMimeData>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickView>
#include <QScopedValueRollback>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QtQml/QQmlExtensionPlugin>

#include <algorithm>
#include <cmath>
#include <utility>

Q_IMPORT_QML_PLUGIN(KetPlugin)

namespace ketplus {
namespace {
QString inlineCodeBackground(const QString& text) {
    QColor wash(text);
    wash.setAlphaF(0.11);
    return wash.name(QColor::HexArgb);
}

QString composerDraftKey(const QString& scope) {
    return QStringLiteral("agent/composerDrafts/%1")
        .arg(QString::fromLatin1(
            QCryptographicHash::hash(scope.toUtf8(), QCryptographicHash::Sha256).toHex()));
}

QString composerAttachmentDraftKey(const QString& scope) {
    return QStringLiteral("agent/composerAttachmentDrafts/%1")
        .arg(QString::fromLatin1(
            QCryptographicHash::hash(scope.toUtf8(), QCryptographicHash::Sha256).toHex()));
}
} // namespace

namespace {

class TranscriptPolish final : public QQuickItem {
  public:
    explicit TranscriptPolish(QQuickItem* root) : QQuickItem(root) {}

  protected:
    void updatePolish() override {
        // Run after the ListView's layout, on the GUI thread and before scene sync.
        // Changing contentY from a delegate's yChanged would reenter that layout.
        if (parentItem())
            QMetaObject::invokeMethod(parentItem(), "settleTranscriptLayout");
    }
};

QString normalizedPath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

QString monoFontFamily() {
    const QString configured = AppearanceSettings::load().editor.fontFamily.trimmed();
    if (!configured.isEmpty())
        return configured;
    return EditorSettings::defaults().fontFamily;
}

void mergeTypeScaleTokens(QVariantMap& tokens, const int basePixels) {
    const QVariantMap scale = TypeScale::tokens(basePixels);
    for (auto it = scale.cbegin(); it != scale.cend(); ++it)
        tokens.insert(it.key(), it.value());
}

} // namespace

// A press lands on the embedded window, not on the container widget, so Qt leaves keyboard
// focus on whichever widget had it (an open file, the sidebar). Selected transcript text
// then looked selected while Cmd+C and Cmd+A went to that widget.
bool AgentWorkspaceView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == view_ && event->type() == QEvent::MouseButtonPress && container_ != nullptr &&
        !container_->hasFocus())
        container_->setFocus(Qt::MouseFocusReason);
    return QWidget::eventFilter(watched, event);
}

AgentWorkspaceView::AgentWorkspaceView(QWidget* parent)
    : QWidget(parent), view_(new QQuickView),
      container_(QWidget::createWindowContainer(view_, this)) {
    activeMessageModel_ = new AgentMessageModel(this);
    messageModels_.insert(QString{}, activeMessageModel_);
    // Transcript links resolve `Foo.cpp` against the same background listing as @ mentions.
    fileLinks_.setFileRequester([this] { refreshFileMentions(); });
    streamUpdateTimer_ = new QTimer(this);
    streamUpdateTimer_->setSingleShot(true);
    connect(streamUpdateTimer_, &QTimer::timeout, this,
            &AgentWorkspaceView::flushStreamingMessages);
    mermaidRenderer_ = new MermaidRenderer(this);
    const auto refreshMermaid = [this] {
        ++mermaidRevision_;
        emit mermaidChanged();
    };
    // A cache/start failure can finish inside request(). Let the caller apply its Pending
    // document before publishing the terminal state, otherwise that document overwrites it.
    connect(mermaidRenderer_, &MermaidRenderer::diagramReady, this, refreshMermaid,
            Qt::QueuedConnection);
    connect(mermaidRenderer_, &MermaidRenderer::diagramFailed, this, refreshMermaid,
            Qt::QueuedConnection);
    setObjectName(QStringLiteral("agentWorkspaceView"));
    setMinimumWidth(420);
    setFocusPolicy(Qt::StrongFocus);
    container_->setFocusPolicy(Qt::StrongFocus);
    setFocusProxy(container_);
    view_->installEventFilter(this);

    view_->setResizeMode(QQuickView::SizeRootObjectToView);
    view_->setPersistentGraphics(true);
    view_->setPersistentSceneGraph(true);
    // Bootstrap from the same semantic palette as Widgets; no second hard-coded theme.
    applyTheme(currentThemePalette(), AppearanceSettings::load().interfaceFontSizePixels);
    view_->rootContext()->setContextProperty(QStringLiteral("agentHost"), this);
    view_->setSource(QUrl(QStringLiteral("qrc:/agent/AgentWorkspace.qml")));
    if (view_->rootObject())
        transcriptPolish_ = new TranscriptPolish(view_->rootObject());
    renderCacheCleanupTimer_ = new QTimer(this);
    renderCacheCleanupTimer_->setInterval(60'000);
    renderCacheCleanupTimer_->setTimerType(Qt::VeryCoarseTimer);
    connect(renderCacheCleanupTimer_, &QTimer::timeout, this,
            &AgentWorkspaceView::pruneRenderTranscripts);
    renderCacheCleanupTimer_->start();
    folderPresenceTimer_ = new QTimer(this);
    folderPresenceTimer_->setInterval(2'000);
    folderPresenceTimer_->setTimerType(Qt::CoarseTimer);
    connect(folderPresenceTimer_, &QTimer::timeout, this, [this] {
        const bool present = activePath_.isEmpty() || QFileInfo(executionPath()).isDir();
        if (present == folderPresent_)
            return;
        folderPresent_ = present;
        rebuildWorktreeModel();
        emit presentationStateChanged();
    });
    folderPresenceTimer_->start();
    if (layoutTraceEnabled()) {
        connect(
            view_, &QQuickWindow::frameSwapped, this,
            [this] {
                if (auto* root = view_->rootObject())
                    QMetaObject::invokeMethod(root, "traceTranscriptFrame");
            },
            Qt::QueuedConnection);
    }
    loadEnabledSkills();
    rebuildSkillModel();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(container_);
}

bool AgentWorkspaceView::layoutTraceEnabled() const { return transcript_trace::enabled(); }

void AgentWorkspaceView::traceLayout(const QString& event, const QVariantMap& values) {
    if (!layoutTraceEnabled())
        return;
    auto record = values;
    record.insert(QStringLiteral("view"), QString::number(quintptr(this), 16));
    transcript_trace::write(event, std::move(record));
}

AgentWorkspaceView::~AgentWorkspaceView() {
    removeRenderTranscripts(this);
    // Unload while the context object is still alive; otherwise bindings may run
    // once more against a destroyed agentHost during native-window teardown.
    transcriptPolish_ = nullptr;
    view_->setSource(QUrl());
    if (mermaidInstaller_ && mermaidInstaller_->state() != QProcess::NotRunning) {
        disconnect(mermaidInstaller_, nullptr, this, nullptr);
        mermaidInstaller_->terminate();
        if (!mermaidInstaller_->waitForFinished(500)) {
            mermaidInstaller_->kill();
            mermaidInstaller_->waitForFinished(1000);
        }
    }
}

void AgentWorkspaceView::setWorktrees(const QVector<GitWorktree>& worktrees,
                                      const QString& activePath,
                                      const QSet<QString>& liveSessionPaths,
                                      const QSet<QString>& conversationPaths) {
    sourceWorktrees_ = worktrees;
    const QString nextPath = normalizedPath(activePath);
    const bool pathChanged = activePath_ != nextPath;
    activePath_ = nextPath;
    liveSessionPaths_.clear();
    for (const QString& path : liveSessionPaths) {
        liveSessionPaths_.insert(normalizedPath(path));
    }
    conversationPaths_.clear();
    for (const QString& path : conversationPaths) {
        conversationPaths_.insert(normalizedPath(path));
    }
    rebuildWorktreeModel();
    // Git status refreshes must not recursively rescan skill files on the GUI
    // thread. Explicit refreshSkills() still discovers edits in the same path.
    if (pathChanged) {
        loadEnabledSkills();
        rebuildSkillModelLater();
        emit activePathChanged();
    }
}

void AgentWorkspaceView::setActivePath(const QString& path) {
    const QString normalized = normalizedPath(path);
    if (activePath_ == normalized) {
        return;
    }
    activePath_ = normalized;
    // The next conversation's report replaces these; another worktree's tasks never show here.
    setBackgroundTasks({});
    setComposerScope(activePath_);
    loadEnabledSkills();
    rebuildWorktreeModel();
    rebuildSkillModelLater();
    emit activePathChanged();
    emit presentationStateChanged();
}

void AgentWorkspaceView::applyTheme(const ThemePalette& palette,
                                    const int interfaceFontSizePixels) {
    const int basePixels = InterfaceZoomMetrics::snap(interfaceFontSizePixels);
    QVariantMap tokens{
        {QStringLiteral("dark"), palette.dark},
        {QStringLiteral("appBackground"), palette.appBackground},
        {QStringLiteral("pageBackground"), palette.pageBackground},
        {QStringLiteral("sidebarBackground"), palette.sidebarBackground},
        {QStringLiteral("panelBackground"), palette.panelBackground},
        {QStringLiteral("panelSubtle"), palette.panelSubtle},
        {QStringLiteral("surfaceRaised"), palette.surfaceRaised},
        {QStringLiteral("surfaceHover"), palette.surfaceHover},
        {QStringLiteral("textMain"), palette.textMain},
        {QStringLiteral("textSecondary"), palette.textSecondary},
        {QStringLiteral("textMuted"), palette.textMuted},
        {QStringLiteral("textDisabled"), palette.textDisabled},
        {QStringLiteral("border"), palette.border},
        {QStringLiteral("borderStrong"), palette.borderStrong},
        {QStringLiteral("interactiveBackground"), palette.interactiveBackground},
        {QStringLiteral("interactiveHover"), palette.interactiveHover},
        {QStringLiteral("interactiveActive"), palette.interactiveActive},
        {QStringLiteral("accent"), palette.accent},
        {QStringLiteral("accentHover"), palette.accentHover},
        {QStringLiteral("accentActive"), palette.accentActive},
        {QStringLiteral("accentSubtle"), palette.accentSubtle},
        {QStringLiteral("accentMuted"), palette.accentMuted},
        {QStringLiteral("focus"), palette.focus},
        {QStringLiteral("tabActiveText"), palette.tabActiveText},
        {QStringLiteral("accentText"), palette.accentText},
        {QStringLiteral("actionPrimary"), palette.actionPrimary},
        {QStringLiteral("actionPrimaryHover"), palette.actionPrimaryHover},
        {QStringLiteral("actionPrimaryActive"), palette.actionPrimaryActive},
        // Inline code chips are a translucent wash of the text colour, so they read
        // on the page and on any surface a message sits on, as Codex draws them.
        {QStringLiteral("inlineCodeBackground"), inlineCodeBackground(palette.textMain)},
        {QStringLiteral("codeBlockBackground"),
         ket::codeBlockRecipe(palette.dark ? ket::Theme::Dark : ket::Theme::Light)
             .background.name(QColor::HexArgb)},
        {QStringLiteral("codeBlockBorder"),
         ket::codeBlockRecipe(palette.dark ? ket::Theme::Dark : ket::Theme::Light)
             .border.name(QColor::HexArgb)},
        {QStringLiteral("positive"), palette.positive},
        {QStringLiteral("warning"), palette.warning},
        {QStringLiteral("danger"), palette.danger},
        {QStringLiteral("uiFont"), InterfaceUiFont::resolvedFamily()},
        {QStringLiteral("monoFont"), monoFontFamily()},
    };
    mergeTypeScaleTokens(tokens, basePixels);
    // One Settings save re-themes every surface several times. Rebinding every
    // token and redrawing each diagram is costly, so an unchanged theme is a no-op.
    if (themeApplied_ && tokens == tokens_)
        return;
    themeApplied_ = true;
    tokens_ = std::move(tokens);
    mermaidPalette_ = palette;
    ++mermaidRevision_;
    emit mermaidChanged();
    view_->setColor(QColor(palette.pageBackground));
    emit tokensChanged();
}

void AgentWorkspaceView::focusPrompt() {
    if (view_->rootObject() != nullptr) {
        QMetaObject::invokeMethod(view_->rootObject(), "focusPrompt");
    }
}

void AgentWorkspaceView::openModelPicker() {
    if (view_->rootObject() != nullptr)
        QMetaObject::invokeMethod(view_->rootObject(), "openModelPicker");
}

void AgentWorkspaceView::setAgentAvailable(const bool available) {
    if (agentAvailable_ == available) {
        return;
    }
    agentAvailable_ = available;
    emit agentAvailableChanged();
    emit presentationStateChanged();
}

void AgentWorkspaceView::setRestoring(const bool restoring) {
    if (restoring_ == restoring) {
        return;
    }
    restoring_ = restoring;
    emit restoringChanged();
    emit presentationStateChanged();
}

void AgentWorkspaceView::setCompactMode(const bool compact) {
    if (compactMode_ == compact) {
        return;
    }
    compactMode_ = compact;
    emit compactModeChanged();
}

void AgentWorkspaceView::setProductSidebarVisible(const bool visible) {
    if (productSidebarVisible_ == visible) {
        return;
    }
    productSidebarVisible_ = visible;
    emit productSidebarVisibleChanged();
}

void AgentWorkspaceView::setConversationWidth(const int pixels) {
    const int width = qMax(0, pixels);
    if (conversationWidth_ == width)
        return;
    conversationWidth_ = width;
    emit conversationWidthChanged();
}

void AgentWorkspaceView::setConversations(const QVariantList& conversations) {
    QVariantList ordered;
    QSet<QString> retained;
    QSet<QString> previouslyRunning;
    QHash<QString, QVariant> byId;
    for (const QVariant& entry : conversations)
        byId.insert(entry.toMap().value("id").toString(), entry);
    for (const QVariant& previous : conversations_) {
        const QString id = previous.toMap().value("id").toString();
        if (previous.toMap().value("running").toBool())
            previouslyRunning.insert(id);
        if (byId.contains(id)) {
            ordered.append(byId.value(id));
            retained.insert(id);
        }
    }
    for (const QVariant& entry : conversations) {
        if (!retained.contains(entry.toMap().value("id").toString()))
            ordered.append(entry);
    }
    for (const QVariant& entry : conversations) {
        const auto item = entry.toMap();
        if (item.value("running").toBool() &&
            !previouslyRunning.contains(item.value("id").toString())) {
            ordered.removeOne(entry);
            ordered.prepend(entry);
        }
    }
    conversations_ = ordered;
    QString scope = activePath_;
    for (qsizetype index = 0; index < conversations_.size(); ++index) {
        const auto conversation = conversations_.at(index).toMap();
        if (conversation.value("active").toBool()) {
            scope += ":" + conversation.value("id").toString();
            break;
        }
    }
    setComposerScope(scope);
    emit conversationsChanged();
}

QString AgentWorkspaceView::statusProvider() const {
    if (conversations_.isEmpty())
        return {};
    auto selected = conversations_.first().toMap();
    for (const auto& entry : conversations_) {
        if (entry.toMap().value("active").toBool()) {
            selected = entry.toMap();
            break;
        }
    }
    // Conversations saved before providers were introduced all use Codex.
    return selected.value("provider", QStringLiteral("codex")).toString().toLower();
}

void AgentWorkspaceView::setSessionState(const QString& title, const QString& status,
                                         const bool running) {
    if (!running)
        flushStreamingMessages();
    sessionTitle_ = title.isEmpty() ? QStringLiteral("New conversation") : title;
    sessionStatus_ = status.isEmpty() ? QStringLiteral("Ready") : status;
    sessionRunning_ = running;
    emit sessionChanged();
    emit presentationStateChanged();
}

QVariantMap AgentWorkspaceView::presentationState() const {
    QString history = QStringLiteral("ready");
    if (restoring_ || historyState_.value(QStringLiteral("loading")).toBool())
        history = QStringLiteral("loading");
    else if (!historyState_.value(QStringLiteral("error")).toString().isEmpty())
        history = QStringLiteral("error");
    else if (historyState_.value(QStringLiteral("hasOlder")).toBool())
        history = QStringLiteral("partial");

    QString turn = QStringLiteral("idle");
    if (steeringPending_)
        turn = QStringLiteral("sending_guidance");
    else if (sessionRunning_)
        turn = QStringLiteral("working");

    return {{QStringLiteral("connection"),
             agentAvailable_ ? QStringLiteral("connected") : QStringLiteral("unavailable")},
            {QStringLiteral("folder"), activePath_.isEmpty() || QFileInfo(executionPath()).isDir()
                                           ? QStringLiteral("available")
                                           : QStringLiteral("missing")},
            {QStringLiteral("history"), history},
            {QStringLiteral("turn"), turn}};
}

void AgentWorkspaceView::setConversationConfiguration(const QVariantMap& configuration) {
    const QString model = configuration.value(QStringLiteral("model")).toString();
    const QString effort = configuration.value(QStringLiteral("effort")).toString();
    const QString permission =
        configuration.value(QStringLiteral("permission"), QStringLiteral("workspace-write"))
            .toString();
    const QString provider = configuration.value(QStringLiteral("provider")).toString();
    const auto revision = configuration.value("configurationRevision").toLongLong();
    const QVariant compactWindow = configuration.value("compactWindow");
    const bool compactSupported = configuration.value("compactWindowSupported").toBool();
    const bool writable = configuration.value("configurationWritable", true).toBool();
    if (conversationConfigurationRevision_ == revision &&
        conversationCompactWindow_ == compactWindow &&
        compactWindowSupported_ == compactSupported && configurationWritable_ == writable &&
        conversationModel_ == model && conversationEffort_ == effort &&
        conversationPermission_ == permission && conversationProvider_ == provider) {
        return;
    }
    conversationConfigurationRevision_ = revision;
    conversationCompactWindow_ = compactWindow;
    compactWindowSupported_ = compactSupported;
    configurationWritable_ = writable;
    conversationProvider_ = provider;
    conversationModel_ = model;
    conversationEffort_ = effort;
    conversationPermission_ = permission.isEmpty() ? QStringLiteral("workspace-write") : permission;
    emit conversationConfigurationChanged();
}

void AgentWorkspaceView::resetMessages(const QVariantList& messages,
                                       const QString& conversationScope) {
    // Commit to the old model before changing scopes; no queued row index may
    // leak into another conversation (including a warm A -> B -> A switch).
    flushStreamingMessages();
    const QString scope = conversationScope;
    if (view_->rootObject())
        QMetaObject::invokeMethod(view_->rootObject(), "captureSessionTranscriptPosition");
    AgentMessageModel* model = messageModels_.value(scope);
    if (!model) {
        model = new AgentMessageModel(this);
        // Resolve payloads against the model's own scope: a warm, inactive
        // model must not fetch from whichever conversation is active now.
        model->setPayloadSource([this, scope](const QString& key, const QString& kind) {
            return transcriptPayloadSource_ ? transcriptPayloadSource_(scope, key, kind)
                                            : QVariant{};
        });
        connect(model, &AgentMessageModel::remoteGroupRequested, this,
                [this, scope](const QString& key, bool expanded) {
                    emit activityGroupRequested(scope, key, expanded);
                });
        messageModels_.insert(scope, model);
    }
    // Re-opening a warm scope whose session history was pruned while inactive
    // delivers an empty, not-yet-reloaded list. Keep the warm rows until the
    // reload lands (a later resetMessages for the now-active scope) instead of
    // flashing an empty transcript. An empty list for the already-active scope
    // is authoritative and still clears.
    const bool keepWarmRows =
        messages.isEmpty() && model->sourceCount() > 0 && scope != activeRenderScope_;
    const bool changed = !keepWarmRows && model->resetIfChanged(messages);
    if (activeMessageModel_ != model) {
        activeMessageModel_ = model;
        emit messageModelChanged();
    }
    touchRenderTranscript(scope);
    if (view_->rootObject()) {
        QMetaObject::invokeMethod(
            view_->rootObject(), "setSessionMessageModel",
            Q_ARG(QVariant, QVariant::fromValue(static_cast<QObject*>(activeMessageModel_))),
            Q_ARG(QVariant, scope), Q_ARG(QVariant, changed));
    }
}

void AgentWorkspaceView::setTranscriptPayloadSource(TranscriptPayloadSource source) {
    transcriptPayloadSource_ = std::move(source);
    for (auto it = messageModels_.cbegin(); it != messageModels_.cend(); ++it) {
        const QString scope = it.key();
        it.value()->setPayloadSource([this, scope](const QString& key, const QString& kind) {
            return transcriptPayloadSource_ ? transcriptPayloadSource_(scope, key, kind)
                                            : QVariant{};
        });
    }
}

void AgentWorkspaceView::appendMessage(const QVariantMap& message) {
    flushStreamingMessages();
    if (activeMessageModel_ && view_->rootObject() != nullptr) {
        QMetaObject::invokeMethod(view_->rootObject(), "appendSessionMessage",
                                  Q_ARG(QVariant, message));
    }
}

void AgentWorkspaceView::prependMessages(const QVariantList& messages) {
    flushStreamingMessages();
    traceLayout(QStringLiteral("history-prepend"), {{QStringLiteral("rows"), messages.size()}});
    if (view_->rootObject())
        QMetaObject::invokeMethod(view_->rootObject(), "prependSessionMessages",
                                  Q_ARG(QVariant, messages));
}

void AgentWorkspaceView::replaceMessageWindow(const QVariantList& messages) {
    flushStreamingMessages();
    if (!view_->rootObject() || !activeMessageModel_)
        return;
    traceLayout(QStringLiteral("history-window"),
                {{QStringLiteral("rows"), messages.size()},
                 {QStringLiteral("before"), activeMessageModel_->sourceCount()}});
    // History page windows keep kinetic scrolling alive (see beginTranscriptPageUpdate).
    QMetaObject::invokeMethod(view_->rootObject(), "beginTranscriptPageUpdate");
    activeMessageModel_->replaceWindow(messages);
    QMetaObject::invokeMethod(view_->rootObject(), "endTranscriptPageUpdate");
}

void AgentWorkspaceView::scheduleTranscriptRestore() {
    if (transcriptPolish_)
        transcriptPolish_->polish();
}

QVariantList AgentWorkspaceView::worktrees() const { return worktrees_; }

QVariantList AgentWorkspaceView::conversations() const { return conversations_; }

QString AgentWorkspaceView::activePath() const { return activePath_; }

QVariantMap AgentWorkspaceView::tokens() const { return tokens_; }

QString AgentWorkspaceView::sessionTitle() const { return sessionTitle_; }

QString AgentWorkspaceView::sessionStatus() const { return sessionStatus_; }

bool AgentWorkspaceView::sessionRunning() const noexcept { return sessionRunning_; }

bool AgentWorkspaceView::agentAvailable() const noexcept { return agentAvailable_; }

QString AgentWorkspaceView::conversationModel() const { return conversationModel_; }

QString AgentWorkspaceView::conversationEffort() const { return conversationEffort_; }

QString AgentWorkspaceView::conversationProvider() const { return conversationProvider_; }

QString AgentWorkspaceView::conversationPermission() const { return conversationPermission_; }

QQuickView* AgentWorkspaceView::quickView() const { return view_; }

void AgentWorkspaceView::prepareBenchmarkMessages(const int count) {
    if (view_->rootObject() != nullptr) {
        QMetaObject::invokeMethod(view_->rootObject(), "prepareBenchmarkMessages",
                                  Q_ARG(QVariant, count));
    }
}

void AgentWorkspaceView::setBenchmarkScrollProgress(const double progress) {
    if (view_->rootObject() != nullptr) {
        QMetaObject::invokeMethod(view_->rootObject(), "setBenchmarkScrollProgress",
                                  Q_ARG(QVariant, progress));
    }
}

int AgentWorkspaceView::benchmarkMessageCount() const {
    if (view_->rootObject() == nullptr) {
        return 0;
    }
    return view_->rootObject()->property("messageCount").toInt();
}

void AgentWorkspaceView::selectWorktree(const QString& path) {
    setActivePath(path);
    emit worktreeActivated(activePath_);
}

QString AgentWorkspaceView::transcriptCopyText() const {
    QVariantList entries;
    if (activeMessageModel_)
        for (int index = 0; index < activeMessageModel_->sourceCount(); ++index)
            entries.append(activeMessageModel_->source(index));
    return agent_message::transcriptText(entries);
}

QString AgentWorkspaceView::entryCopyText(const QVariantMap& entry) const {
    return agent_message::entryCopyText(entry);
}

void AgentWorkspaceView::sendEditShortcut(const QKeySequence::StandardKey key) {
    const QKeySequence sequence(key);
    if (view_ == nullptr || sequence.isEmpty() || sendingEditShortcut_)
        return;
    QPointer<QQuickItem> focusedItem = view_->activeFocusItem();
    if (focusedItem == nullptr)
        return;
    const QScopedValueRollback<bool> sendingGuard(sendingEditShortcut_, true);
    const QKeyCombination combination = sequence[0];
    QKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers());
    // Sending this event to QQuickView enters Qt's shortcut map and reactivates the
    // same Edit action recursively. The focused item handles the edit key itself.
    QCoreApplication::sendEvent(focusedItem, &press);
    if (focusedItem == nullptr)
        return;
    QKeyEvent release(QEvent::KeyRelease, combination.key(), combination.keyboardModifiers());
    QCoreApplication::sendEvent(focusedItem, &release);
}

void AgentWorkspaceView::openTranscriptFind() {
    if (container_ != nullptr)
        container_->setFocus(Qt::ShortcutFocusReason);
    if (auto* root = view_ ? view_->rootObject() : nullptr)
        QMetaObject::invokeMethod(root, "openTranscriptFind");
}

void AgentWorkspaceView::setFindActionsProvided(const bool provided) {
    if (findActionsProvided_ == provided)
        return;
    findActionsProvided_ = provided;
    emit findActionsProvidedChanged();
}

bool AgentWorkspaceView::hasFocusWithin() const {
    if (const QWidget* focus = QApplication::focusWidget();
        focus != nullptr && (focus == this || isAncestorOf(focus)))
        return true;
    return view_ != nullptr && QGuiApplication::focusWindow() == view_;
}

void AgentWorkspaceView::findInTranscript(const bool backwards) {
    if (auto* root = view_ ? view_->rootObject() : nullptr)
        QMetaObject::invokeMethod(root, "transcriptFindStep", Q_ARG(QVariant, backwards));
}

QStringList AgentWorkspaceView::codeBlockSources(const QString& markdown) const {
    return agent_message::fencedCodeBlocks(markdown);
}

void AgentWorkspaceView::openRelativeFile(const QString& path) {
    const QUrl url(path);
    if ((url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http")) &&
        url.isValid()) {
        QDesktopServices::openUrl(url);
        return;
    }
    if (executionContextSwitched_ && QDir::isRelativePath(path) && !url.isLocalFile()) {
        emit composerErrorOccurred(
            tr("This link has no verified worktree source. Open the file from Files instead."));
        return;
    }
    emit fileOpenRequested(path);
}

void AgentWorkspaceView::requestTerminal() { emit terminalRequested(); }

void AgentWorkspaceView::requestAddWorktree() { emit addWorktreeRequested(); }

void AgentWorkspaceView::requestNewSession() { emit newSessionRequested(); }

void AgentWorkspaceView::requestClearSession() { emit clearSessionRequested(); }

void AgentWorkspaceView::requestCurrentFileDiff() { emit currentFileDiffRequested(); }

void AgentWorkspaceView::requestFileDiff(const QString& path) {
    if (executionContextSwitched_ && QDir::isRelativePath(path) && !QUrl(path).isLocalFile()) {
        emit composerErrorOccurred(
            tr("This link has no verified worktree source. Open the file from Files instead."));
        return;
    }
    if (!path.isEmpty()) {
        emit fileDiffRequested(path);
    }
}

QString AgentWorkspaceView::absoluteFilePath(const QString& path) const {
    const QUrl url(path);
    if (url.isLocalFile()) {
        return normalizedPath(url.toLocalFile());
    }
    if (QDir::isAbsolutePath(path)) {
        return normalizedPath(path);
    }
    return normalizedPath(QDir(activePath_).filePath(path));
}

QString AgentWorkspaceView::displayFilePath(const QString& path) const {
    const QString absolutePath = absoluteFilePath(path);
    if (absolutePath.isEmpty()) {
        return path;
    }

    QString worktreeRoot = normalizedPath(activePath_);
    const auto isInside = [&absolutePath](const QString& root) {
        const QString relative = QDir(root).relativeFilePath(absolutePath);
        return relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../")) &&
               !QDir::isAbsolutePath(relative);
    };
    if (worktreeRoot.isEmpty() || !isInside(worktreeRoot)) {
        QDir candidate = QFileInfo(absolutePath).absoluteDir();
        while (!candidate.isRoot() &&
               !QFileInfo(candidate.filePath(QStringLiteral(".git"))).exists()) {
            if (!candidate.cdUp()) {
                break;
            }
        }
        if (QFileInfo(candidate.filePath(QStringLiteral(".git"))).exists()) {
            worktreeRoot = normalizedPath(candidate.absolutePath());
        }
    }
    if (worktreeRoot.isEmpty() || !isInside(worktreeRoot)) {
        return path;
    }

    const QString relative = QDir(worktreeRoot).relativeFilePath(absolutePath);
    return QFileInfo(worktreeRoot).fileName() + QLatin1Char('/') + relative;
}

QString AgentWorkspaceView::revealFileActionText() const {
#if defined(Q_OS_MACOS)
    return tr("Reveal in Finder");
#elif defined(Q_OS_WIN)
    return tr("Reveal in Explorer");
#else
    return tr("Reveal in File Manager");
#endif
}

QString AgentWorkspaceView::revealableFilePath(const QString& path) const {
    if (path.isEmpty())
        return {};
    const QUrl url(path);
    if ((!url.scheme().isEmpty() && !url.isLocalFile() && !QDir::isAbsolutePath(path)) ||
        (executionContextSwitched_ && QDir::isRelativePath(path) && !url.isLocalFile()))
        return {};
    QString absolutePath = absoluteFilePath(path);
    if (!QFileInfo::exists(absolutePath))
        absolutePath.remove(QRegularExpression(QStringLiteral(R"(:\d+(?::\d+)?$)")));
    return QFileInfo::exists(absolutePath) ? QFileInfo(absolutePath).canonicalFilePath()
                                           : QString{};
}

void AgentWorkspaceView::revealFile(const QString& path) const {
    const QString absolutePath = revealableFilePath(path);
    if (!absolutePath.isEmpty())
        file_reveal::reveal(absolutePath);
}

namespace {
// A missing-path answer is re-checked after this long, so a file restored on disk turns
// clickable again without a reload.
constexpr qint64 kPathStateLifetimeMs = 10000;
constexpr int kPathStateLimit = 4096;
} // namespace

bool AgentWorkspaceView::pathMissing(const QString& path) {
    if (path.isEmpty())
        return false;
    const QUrl url(path);
    if (!url.scheme().isEmpty() && !url.isLocalFile() && !QDir::isAbsolutePath(path))
        return false;
    // Keyed by the worktree too: a relative path means a different file in another worktree.
    const QString key = activePath_ + QChar(u'\n') + path;
    const auto it = pathStates_.constFind(key);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (it == pathStates_.constEnd() || now - it->checkedAt > kPathStateLifetimeMs) {
        QString absolute = url.isLocalFile()            ? url.toLocalFile()
                           : QDir::isAbsolutePath(path) ? path
                                                        : QDir(activePath_).filePath(path);
        pendingPathChecks_.insert(key, absolute);
        if (!pathCheckRunning_) {
            pathCheckRunning_ = true;
            QMetaObject::invokeMethod(this, &AgentWorkspaceView::checkPendingPaths,
                                      Qt::QueuedConnection);
        }
    }
    return it != pathStates_.constEnd() && !it->exists;
}

// One worker at a time drains every path queued since the last run, so a page of rows
// realized in one frame costs one pool task and at most one repaint.
void AgentWorkspaceView::checkPendingPaths() {
    if (pendingPathChecks_.isEmpty()) {
        pathCheckRunning_ = false;
        return;
    }
    QHash<QString, QString> batch;
    batch.swap(pendingPathChecks_);
    QThreadPool::globalInstance()->start([self = QPointer<AgentWorkspaceView>(this), batch] {
        QHash<QString, bool> results;
        results.reserve(batch.size());
        for (auto it = batch.constBegin(); it != batch.constEnd(); ++it)
            results.insert(it.key(), QFileInfo::exists(it.value()));
        const qint64 checkedAt = QDateTime::currentMSecsSinceEpoch();
        QMetaObject::invokeMethod(
            qApp,
            [self, results, checkedAt] {
                if (self)
                    self->applyPathStates(results, checkedAt);
            },
            Qt::QueuedConnection);
    });
}

void AgentWorkspaceView::applyPathStates(const QHash<QString, bool>& results, qint64 checkedAt) {
    // Forget present paths first: a forgotten one reads as present anyway, while forgetting a
    // missing one would briefly turn its row clickable again.
    if (pathStates_.size() + results.size() > kPathStateLimit) {
        pathStates_.removeIf([](const auto& entry) { return entry.value().exists; });
        if (pathStates_.size() + results.size() > kPathStateLimit)
            pathStates_.clear();
    }
    bool changed = false;
    for (auto it = results.constBegin(); it != results.constEnd(); ++it) {
        PathState& state = pathStates_[it.key()];
        // A path first seen reads as present, so only a missing answer changes what shows.
        const bool wasExisting = state.checkedAt == 0 || state.exists;
        changed = changed || wasExisting != it.value();
        state = {it.value(), checkedAt};
    }
    if (changed) {
        ++pathStateRevision_;
        emit pathStatesChanged();
    }
    // Paths queued while the worker ran go out as the next batch; otherwise go idle.
    checkPendingPaths();
}

void AgentWorkspaceView::copyText(const QString& text) {
    if (!text.isEmpty() && QGuiApplication::clipboard() != nullptr) {
        QGuiApplication::clipboard()->setText(text);
    }
}

void AgentWorkspaceView::copySelectedText(QString text) {
    // TextEdit.selectedText uses document separators, not clipboard line endings.
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    text.replace(QChar::LineSeparator, QLatin1Char('\n'));
    copyText(text);
}

void AgentWorkspaceView::selectConversation(const QString& conversationId) {
    if (!conversationId.isEmpty()) {
        emit conversationActivated(conversationId);
    }
}

void AgentWorkspaceView::stopAgent() { emit stopRequested(); }

void AgentWorkspaceView::requestRenameConversation(const QString& conversationId) {
    if (!conversationId.isEmpty()) {
        emit conversationRenameRequested(conversationId);
    }
}

void AgentWorkspaceView::submitPrompt(const QString& prompt) {
    if (!prompt.trimmed().isEmpty()) {
        emit promptSubmitted(prompt.trimmed());
    }
}

QString AgentWorkspaceView::composerDraft(const QString& scope) const {
    if (scope.isEmpty())
        return {};
    const QVariantMap stored = QSettings().value(composerDraftKey(scope)).toMap();
    if (stored.value(QStringLiteral("scope")).toString() != scope)
        return {};
    return stored.value(QStringLiteral("text")).toString().left(256 * 1024);
}

void AgentWorkspaceView::saveComposerDraft(const QString& scope, const QString& text) const {
    if (scope.isEmpty())
        return;
    QSettings settings;
    const QString key = composerDraftKey(scope);
    if (text.isEmpty()) {
        settings.remove(key);
        return;
    }
    settings.setValue(key, QVariantMap{{QStringLiteral("scope"), scope},
                                       {QStringLiteral("text"), text.left(256 * 1024)}});
}

void AgentWorkspaceView::updateConversationConfiguration(const QString& model,
                                                         const QString& effort,
                                                         const QString& permission,
                                                         const QString& provider) {
    emit conversationConfigurationRequested(model.trimmed(), effort.trimmed(), permission.trimmed(),
                                            provider.trimmed().toLower(), {});
}

void AgentWorkspaceView::applyModelConfiguration(const QString& model, const QString& effort,
                                                 const QString& provider,
                                                 const QVariantMap& changes) {
    if (modelConfigurationApplying_)
        return;
    modelConfigurationError_.clear();
    modelConfigurationApplying_ = true;
    emit modelConfigurationStateChanged();
    emit conversationConfigurationRequested(model.trimmed(), effort.trimmed(),
                                            conversationPermission_, provider.trimmed().toLower(),
                                            changes);
}

void AgentWorkspaceView::finishModelConfiguration(const QString& error) {
    if (!modelConfigurationApplying_)
        return;
    modelConfigurationApplying_ = false;
    modelConfigurationError_ = error;
    emit modelConfigurationStateChanged();
    emit modelConfigurationFinished(error);
}

void AgentWorkspaceView::setComposerScope(const QString& scope) {
    if (scope == composerScope_)
        return;
    modelConfigurationApplying_ = false;
    modelConfigurationError_.clear();
    emit modelConfigurationStateChanged();
    attachmentDrafts_.insert(composerScope_, attachments_);
    saveAttachmentDraft(composerScope_);
    composerScope_ = scope;
    attachments_ = attachmentDrafts_.contains(scope) ? attachmentDrafts_.value(scope)
                                                     : loadAttachmentDraft(scope);
    attachmentDrafts_.insert(scope, attachments_);
    emit attachmentsChanged();
    emit composerScopeChanged();
}

void AgentWorkspaceView::saveAttachmentDraft(const QString& scope) const {
    saveAttachmentDraft(scope, attachments_);
}

void AgentWorkspaceView::saveAttachmentDraft(const QString& scope,
                                             const QVariantList& attachments) const {
    if (scope.isEmpty())
        return;
    QSettings settings;
    const QString key = composerAttachmentDraftKey(scope);
    if (attachments.isEmpty()) {
        settings.remove(key);
        return;
    }
    settings.setValue(key, attachments.mid(0, 16));
}

QVariantList AgentWorkspaceView::loadAttachmentDraft(const QString& scope) const {
    if (scope.isEmpty())
        return {};
    QVariantList result;
    const QVariantList stored = QSettings().value(composerAttachmentDraftKey(scope)).toList();
    for (const QVariant& value : stored) {
        const QVariantMap item = value.toMap();
        if (QFileInfo::exists(item.value(QStringLiteral("path")).toString()))
            result.append(item);
        if (result.size() == 16)
            break;
    }
    return result;
}

QVariantList AgentWorkspaceView::permissions() const {
    return CodexAppServer::permissionProfiles();
}

void AgentWorkspaceView::acceptBackgroundTaskResult(const QString& taskId, const QString& action,
                                                    const QVariantMap& result) {
    const QString error = result.value(QStringLiteral("error")).toString();
    if (action == QStringLiteral("stop")) {
        // A stop that worked shows up in the next task report.
        if (!error.isEmpty())
            emit backgroundTaskFailed(error);
        return;
    }
    // A pane closed or switched to another task while this read was in flight keeps its state.
    if (backgroundTaskOutput_.value(QStringLiteral("taskId")).toString() != taskId)
        return;
    backgroundTaskOutput_ = {
        {QStringLiteral("taskId"), taskId},
        {QStringLiteral("loading"), false},
        {QStringLiteral("output"), result.value(QStringLiteral("output"))},
        {QStringLiteral("totalBytes"), result.value(QStringLiteral("total_bytes"))},
        {QStringLiteral("truncated"), result.value(QStringLiteral("truncated"))},
        {QStringLiteral("error"), error}};
    emit backgroundTaskOutputChanged();
}

} // namespace ketplus
