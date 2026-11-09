#pragma once

#include "agent/AgentMessageModel.h"
#include "agent/TranscriptFileLinks.h"
#include "git/GitTypes.h"
#include "ui/Theme.h"

#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QKeySequence>
#include <QMap>
#include <QSet>
#include <QSize>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

#include <algorithm>
#include <functional>

class QQuickView;
class QQuickItem;
class QProcess;
class QTimer;

namespace ketplus {

struct ThemePalette;
class MermaidRenderer;

class AgentWorkspaceView final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QString executionBranch MEMBER executionBranch_ NOTIFY executionBranchChanged)
    Q_PROPERTY(
        ketplus::AgentMessageModel* messageModel READ messageModel NOTIFY messageModelChanged)
    Q_PROPERTY(QVariantMap historyState READ historyState NOTIFY historyStateChanged)
    Q_PROPERTY(bool findActionsProvided READ findActionsProvided NOTIFY findActionsProvidedChanged)
    Q_PROPERTY(bool layoutTraceEnabled READ layoutTraceEnabled CONSTANT)
    Q_PROPERTY(int mermaidRevision READ mermaidRevision NOTIFY mermaidChanged)
    Q_PROPERTY(int pathStateRevision READ pathStateRevision NOTIFY pathStatesChanged)
    Q_PROPERTY(bool mermaidInstalling READ mermaidInstalling NOTIFY mermaidChanged)
    Q_PROPERTY(QString mermaidInstallError READ mermaidInstallError NOTIFY mermaidChanged)
    Q_PROPERTY(QVariantList worktrees READ worktrees NOTIFY worktreesChanged)
    Q_PROPERTY(QVariantList conversations READ conversations NOTIFY conversationsChanged)
    Q_PROPERTY(QString activePath READ activePath NOTIFY activePathChanged)
    Q_PROPERTY(int workingChangesRevision READ workingChangesRevision NOTIFY workingChangesChanged)
    Q_PROPERTY(QString repositoryName READ repositoryName WRITE setRepositoryName NOTIFY
                   repositoryNameChanged)
    Q_PROPERTY(QUrl messageBaseUrl READ messageBaseUrl NOTIFY activePathChanged)
    Q_PROPERTY(QString monoFontFamily READ monoFontFamily NOTIFY monoFontFamilyChanged)
    Q_PROPERTY(QString sessionTitle READ sessionTitle NOTIFY sessionChanged)
    Q_PROPERTY(QString sessionStatus READ sessionStatus NOTIFY sessionChanged)
    Q_PROPERTY(bool sessionRunning READ sessionRunning NOTIFY sessionChanged)
    Q_PROPERTY(int atlasRunningCount READ atlasRunningCount NOTIFY atlasRunningCountChanged)
    Q_PROPERTY(bool agentAvailable READ agentAvailable NOTIFY agentAvailableChanged)
    Q_PROPERTY(bool restoring READ restoring NOTIFY restoringChanged)
    Q_PROPERTY(bool motionEnabled READ motionEnabled CONSTANT)
    Q_PROPERTY(bool workingMotionEnabled READ workingMotionEnabled CONSTANT)
    Q_PROPERTY(QVariantMap presentationState READ presentationState NOTIFY presentationStateChanged)
    Q_PROPERTY(
        QString conversationModel READ conversationModel NOTIFY conversationConfigurationChanged)
    Q_PROPERTY(
        QString conversationEffort READ conversationEffort NOTIFY conversationConfigurationChanged)
    Q_PROPERTY(QString conversationPermission READ conversationPermission NOTIFY
                   conversationConfigurationChanged)
    // The provider an already started conversation is bound to; empty for a new one.
    Q_PROPERTY(QString conversationProvider READ conversationProvider NOTIFY
                   conversationConfigurationChanged)

    Q_PROPERTY(qint64 conversationConfigurationRevision READ conversationConfigurationRevision
                   NOTIFY conversationConfigurationChanged)
    Q_PROPERTY(QVariant conversationCompactWindow READ conversationCompactWindow NOTIFY
                   conversationConfigurationChanged)
    Q_PROPERTY(bool compactWindowSupported READ compactWindowSupported NOTIFY
                   conversationConfigurationChanged)
    Q_PROPERTY(bool configurationWritable READ configurationWritable NOTIFY
                   conversationConfigurationChanged)
    Q_PROPERTY(bool modelConfigurationApplying READ modelConfigurationApplying NOTIFY
                   modelConfigurationStateChanged)
    Q_PROPERTY(QString modelConfigurationError READ modelConfigurationError NOTIFY
                   modelConfigurationStateChanged)

    Q_PROPERTY(QString composerScope READ composerScope NOTIFY composerScopeChanged)
    Q_PROPERTY(bool steeringPending READ steeringPending NOTIFY steeringPendingChanged)
    Q_PROPERTY(QVariantList permissions READ permissions CONSTANT)
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)
    Q_PROPERTY(QVariantMap weeklyLimit READ weeklyLimit NOTIFY weeklyLimitChanged)
    Q_PROPERTY(QVariantMap providerRuntime READ providerRuntime NOTIFY providerRuntimeChanged)
    // `provider` scopes the snapshot; `context` is conversation-scoped and `limits`
    // contains account-scoped windows.
    Q_PROPERTY(
        QVariantMap claudeAccountUsage READ claudeAccountUsage NOTIFY claudeAccountUsageChanged)
    Q_PROPERTY(QVariantMap usageSnapshot READ usageSnapshot NOTIFY usageSnapshotChanged)
    Q_PROPERTY(QVariantList backgroundTasks READ backgroundTasks NOTIFY backgroundTasksChanged)
    // The open output pane: {taskId, loading, output, totalBytes, truncated, error}.
    Q_PROPERTY(QVariantMap backgroundTaskOutput READ backgroundTaskOutput NOTIFY
                   backgroundTaskOutputChanged)
    Q_PROPERTY(QString statusProvider READ statusProvider NOTIFY conversationsChanged)
    Q_PROPERTY(QVariantList skills READ skills NOTIFY skillsChanged)
    Q_PROPERTY(int fileMentionRevision READ fileMentionRevision NOTIFY fileMentionsChanged)
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY attachmentsChanged)
    Q_PROPERTY(bool compactMode READ compactMode WRITE setCompactMode NOTIFY compactModeChanged)
    Q_PROPERTY(
        bool compactWindow READ compactWindow WRITE setCompactWindow NOTIFY compactWindowChanged)
    Q_PROPERTY(bool productSidebarVisible READ productSidebarVisible WRITE setProductSidebarVisible
                   NOTIFY productSidebarVisibleChanged)
    // Unscaled pixels; 0 lets the conversation fill the pane.
    Q_PROPERTY(int conversationWidth READ conversationWidth WRITE setConversationWidth NOTIFY
                   conversationWidthChanged)
  public:
    static constexpr int defaultConversationWidth = 760;

    AgentMessageModel* messageModel() const { return activeMessageModel_; }
    QVariantMap historyState() const { return historyState_; }
    void setHistoryState(const QVariantMap& state) {
        historyState_ = state;
        emit historyStateChanged();
        emit presentationStateChanged();
    }
    void prependMessages(const QVariantList& messages);
    void replaceMessageWindow(const QVariantList& messages);
    // Geometry hints (0 = unknown) size continuation pages to about three viewports.
    // requestToken is echoed back as historyState.requestToken when the request completes.
    Q_INVOKABLE void loadOlderMessages(double viewportHeight = 0, double averageRowHeight = 0,
                                       double requestToken = 0) {
        emit olderMessagesRequested(viewportHeight, averageRowHeight, qint64(requestToken));
    }
    Q_INVOKABLE void loadNewerMessages(double viewportHeight = 0, double averageRowHeight = 0,
                                       double requestToken = 0) {
        emit newerMessagesRequested(viewportHeight, averageRowHeight, qint64(requestToken));
    }
    Q_INVOKABLE void loadLatestMessages() { emit latestMessagesRequested(); }
    Q_INVOKABLE void reportTranscriptExtent(double renderedHeight, double viewportHeight) {
        emit transcriptExtentReported(renderedHeight, viewportHeight);
    }
    Q_INVOKABLE void setTranscriptInputActive(bool active) {
        emit transcriptInputActiveChanged(active);
    }
    Q_INVOKABLE void setTranscriptScrolling(bool scrolling, const QString& readingKey = {}) {
        emit transcriptScrollingChanged(scrolling, readingKey);
    }
    Q_INVOKABLE void setTranscriptReadingKey(const QString& readingKey) {
        emit transcriptReadingKeyChanged(readingKey);
    }
    Q_INVOKABLE void scheduleTranscriptRestore();
    Q_INVOKABLE void copyConversation(const QString& loadedText) {
        if (historyState_.isEmpty())
            copyText(loadedText);
        else
            emit conversationCopyRequested();
    }
    Q_INVOKABLE void cancelConversationCopy() { emit conversationCopyCanceled(); }
    int mermaidRevision() const { return mermaidRevision_; }
    int pathStateRevision() const { return pathStateRevision_; }
    // Assistant Markdown as the transcript shows it: raw HTML escaped. A Mermaid fence stays an
    // ordinary source block until the reader switches that block to its diagram
    // (setMermaidMode); only then is it rendered and shown as an image.
    Q_INVOKABLE QString renderChatMarkdown(const QString& source, const QString& messageKey = {});
    Q_INVOKABLE void setMermaidMode(const QString& messageKey, int ordinal, bool rendered);
    Q_INVOKABLE bool mermaidRendered(const QString& messageKey, int ordinal) const;
    // Where each Mermaid block of a rendered message sits, source or diagram, so the transcript
    // can frame it and offer the switch; and which source fence each code frame belongs to.
    Q_INVOKABLE QVariantMap mermaidLayout(QObject* document, const QString& messageKey) const;
    // The image viewer's item for a rendered diagram, as the mock opens one: { source, caption },
    // or empty for any other image. The viewer scales its image, so the source is a raster large
    // enough to stay sharp at twice the screen; it is the dark rendering once there is one.
    Q_INVOKABLE QVariantMap mermaidViewerImage(const QUrl& url);
    Q_INVOKABLE void installMermaid();
    bool mermaidInstalling() const;
    QString mermaidInstallError() const { return mermaidInstallError_; }
    Q_INVOKABLE QVariantList mermaidPlaceholderPositions(QObject* document) const;
    bool layoutTraceEnabled() const;
    Q_INVOKABLE void traceLayout(const QString& event, const QVariantMap& values);
    QString composerScope() const { return composerScope_; }
    bool steeringPending() const { return steeringPending_; }
    Q_INVOKABLE void steerPrompt(const QString& prompt);
    Q_INVOKABLE void updateConversationConfiguration(const QString& model, const QString& effort,
                                                     const QString& permission,
                                                     const QString& provider = {});
    Q_INVOKABLE void applyModelConfiguration(const QString& model, const QString& effort,
                                             const QString& provider, const QVariantMap& changes);
    void finishModelConfiguration(const QString& error);
    qint64 conversationConfigurationRevision() const { return conversationConfigurationRevision_; }
    QVariant conversationCompactWindow() const { return conversationCompactWindow_; }
    bool compactWindowSupported() const { return compactWindowSupported_; }
    bool configurationWritable() const { return configurationWritable_; }
    bool modelConfigurationApplying() const { return modelConfigurationApplying_; }
    QString modelConfigurationError() const { return modelConfigurationError_; }
    void finishSteering(const QString& path, const QString& conversationId, const QString& prompt,
                        bool accepted);
    QVariantList permissions() const;
    QVariantList models() const { return models_; }
    QVariantList skills() const { return skills_; }
    QVariantList attachments() const { return attachments_; }
    void setModels(const QVariantList& models) {
        models_ = models;
        emit modelsChanged();
    }
    void setExecutionBranch(const QString& branch, const QString& path = {}, qint64 revision = 1) {
        executionBranch_ = branch;
        executionPath_ = path;
        if (!path.isEmpty())
            executionContextSwitched_ = revision > 1;
        emit executionBranchChanged();
        emit presentationStateChanged();
    }
    QString executionPath() const {
        return executionPath_.isEmpty() ? activePath() : executionPath_;
    }
    Q_INVOKABLE void requestExecutionWorktree() { emit executionWorktreeRequested(); }
    Q_INVOKABLE void requestConversationWorktree(const QString& id) {
        if (!id.isEmpty())
            emit conversationWorktreeRequested(id);
    }
    Q_INVOKABLE void requestModels() { emit modelsRequested(); }
    QVariantMap weeklyLimit() const { return weeklyLimit_; }
    using UsageReply = std::function<void(const QJsonObject&, const QString&)>;
    using UsageRequester = std::function<void(UsageReply)>;
    void setClaudeUsageRequester(UsageRequester requester) {
        claudeUsageRequester_ = std::move(requester);
    }
    QVariantMap claudeAccountUsage() const { return claudeAccountUsage_; }
    Q_INVOKABLE void requestClaudeAccountUsage();
    QVariantMap usageSnapshot() const { return usageSnapshot_; }
    QString statusProvider() const;
    void setWeeklyLimit(const QVariantMap& value) {
        weeklyLimit_ = value;
        emit weeklyLimitChanged();
    }
    QVariantMap providerRuntime() const { return providerRuntime_; }
    void setProviderRuntime(const QVariantMap& value) {
        providerRuntime_ = value;
        emit providerRuntimeChanged();
    }
    void setUsageSnapshot(const QVariantMap& value) {
        usageSnapshot_ = value;
        emit usageSnapshotChanged();
    }
    QVariantList backgroundTasks() const { return backgroundTasks_; }
    void setBackgroundTasks(const QVariantList& value) {
        if (backgroundTasks_ == value)
            return;
        backgroundTasks_ = value;
        emit backgroundTasksChanged();
        const QString open = backgroundTaskOutput_.value(QStringLiteral("taskId")).toString();
        if (!open.isEmpty() &&
            std::none_of(value.cbegin(), value.cend(), [&open](const QVariant& task) {
                return task.toMap().value(QStringLiteral("task_id")).toString() == open;
            }))
            closeBackgroundTaskOutput();
    }
    QVariantMap backgroundTaskOutput() const { return backgroundTaskOutput_; }
    Q_PROPERTY(QVariantMap heldMessageStates READ heldMessageStates NOTIFY heldMessageStatesChanged)
    QVariantMap heldMessageStates() const { return heldMessageStates_; }
    using AtlasResultInspector = std::function<QVariantMap(const QString&, const QString&)>;
    void setAtlasResultInspector(AtlasResultInspector inspector) {
        atlasResultInspector_ = std::move(inspector);
    }
    Q_INVOKABLE QVariantMap inspectAtlasResult(const QString& link) const;
    Q_INVOKABLE void openAtlasResult(const QString& link, bool inBrowser);
    Q_INVOKABLE void requestHeldMessageState(const QString& messageId);
    Q_INVOKABLE void releaseHeldMessage(const QString& messageId);
    void acceptHeldMessageState(const QString& messageId, const QVariantMap& value);
    // ADR 0067: {available, pending, error, reviewer: {number,title,provider,model}}.
    Q_PROPERTY(QVariantMap reviewerState READ reviewerState NOTIFY reviewerStateChanged)
    QVariantMap reviewerState() const { return reviewerState_; }
    // Claude conversations that can review the active one, newest first.
    Q_INVOKABLE QVariantList reviewerCandidates() const;
    // Assigns conversation #number, starts a new reviewer with newProvider, or clears it.
    Q_INVOKABLE void chooseReviewer(const QString& number, const QString& newProvider);
    void acceptReviewerState(const QVariantMap& value);
    Q_INVOKABLE void stopBackgroundTask(const QString& taskId) {
        emit backgroundTaskStopRequested(taskId);
    }
    Q_INVOKABLE void requestBackgroundTaskOutput(const QString& taskId) {
        // A refresh of the open task keeps the tail it already shows until the new one arrives.
        if (backgroundTaskOutput_.value(QStringLiteral("taskId")).toString() != taskId)
            backgroundTaskOutput_ = {{QStringLiteral("taskId"), taskId}};
        backgroundTaskOutput_.insert(QStringLiteral("loading"), true);
        emit backgroundTaskOutputChanged();
        emit backgroundTaskOutputRequested(taskId);
    }
    Q_INVOKABLE void closeBackgroundTaskOutput() {
        backgroundTaskOutput_.clear();
        emit backgroundTaskOutputChanged();
    }
    void acceptBackgroundTaskResult(const QString& taskId, const QString& action,
                                    const QVariantMap& result);
    Q_INVOKABLE void requestWeeklyLimit() { emit weeklyLimitRequested(); }
    Q_INVOKABLE void refreshSkills();
    Q_INVOKABLE QVariantList skillSuggestions(const QString& query) const;
    Q_INVOKABLE void toggleSkill(const QString& path, bool enabled);
    // Files and folders of the conversation worktree matching an `@` mention query. Never
    // blocks: the worktree is listed off the GUI thread, and fileMentionRevision changes when
    // a listing lands, so a binding that reads it asks again.
    Q_INVOKABLE QVariantList fileMentionSuggestions(const QString& query);
    // ADR 0027: `#` completes conversation numbers from the directory below.
    Q_INVOKABLE QVariantList conversationMentionSuggestions(const QString& query) const;
    Q_INVOKABLE void refreshFileMentions();
    int fileMentionRevision() const { return fileMentionRevision_; }
    Q_INVOKABLE void chooseAttachments();
    Q_INVOKABLE bool pasteClipboardImage();
    Q_INVOKABLE void addAttachments(const QStringList& paths);
    Q_INVOKABLE void removeAttachment(int index);
    Q_INVOKABLE void submitConfiguredPrompt(const QString& prompt, const QVariantMap& options);
    explicit AgentWorkspaceView(QWidget* parent = nullptr);
    ~AgentWorkspaceView() override;

    void setWorktrees(const QVector<GitWorktree>& worktrees, const QString& activePath,
                      const QSet<QString>& liveSessionPaths = {},
                      const QSet<QString>& conversationPaths = {});
    void setActivePath(const QString& path);
    // Source Control's uncommitted files in `root`, relative to it, for the changed-files
    // card's "Not committed" note.
    void setWorkingChanges(const QString& root, const QStringList& relativePaths);
    int workingChangesRevision() const { return workingChangesRevision_; }
    // Whether any of `paths` (relative to the conversation's checkout, or absolute) has
    // uncommitted changes there; false while Source Control shows another checkout.
    Q_INVOKABLE bool hasUncommittedChange(const QStringList& paths) const;
    QString repositoryName() const { return repositoryName_; }
    void setRepositoryName(const QString& name) {
        if (repositoryName_ == name)
            return;
        repositoryName_ = name;
        emit repositoryNameChanged();
    }
    // Refresh renderer colours and editor mono preferences. Interface zoom is owned
    // by ket::Appearance; the legacy size argument is retained for shell callers.
    void applyTheme(const ThemePalette& palette, int interfaceFontSizePixels = 14);
    void focusPrompt();
    void openModelPicker();
    void setAgentAvailable(bool available);
    void setRestoring(bool restoring);
    void setCompactMode(bool compact);
    void setProductSidebarVisible(bool visible);
    void setCompactWindow(bool compact);
    [[nodiscard]] bool compactWindow() const { return compactWindow_; }
    Q_INVOKABLE void requestCompactWindow() { emit compactWindowRequested(); }
    void setConversationWidth(int pixels);
    void setConversations(const QVariantList& conversations);
    void setSessionState(const QString& title, const QString& status, bool running);
    void setConversationConfiguration(const QVariantMap& configuration);
    void resetMessages(const QVariantList& messages, const QString& conversationScope = {});
    void appendMessage(const QVariantMap& message);
    void updateMessage(int index, const QVariantMap& message);

    [[nodiscard]] QVariantList worktrees() const;
    [[nodiscard]] QVariantList conversations() const;
    [[nodiscard]] QString activePath() const;
    [[nodiscard]] QString monoFontFamily() const;
    [[nodiscard]] QString sessionTitle() const;
    [[nodiscard]] QString sessionStatus() const;
    [[nodiscard]] bool sessionRunning() const noexcept;
    [[nodiscard]] int atlasRunningCount() const noexcept { return atlasRunningCount_; }
    void setAtlasRunningCount(int count) {
        count = std::max(0, count);
        if (atlasRunningCount_ == count)
            return;
        atlasRunningCount_ = count;
        emit atlasRunningCountChanged();
    }
    [[nodiscard]] bool agentAvailable() const noexcept;
    [[nodiscard]] bool restoring() const noexcept { return restoring_; }
    [[nodiscard]] bool motionEnabled() const noexcept { return motionEnabled_; }
    [[nodiscard]] bool workingMotionEnabled() const noexcept { return workingMotionEnabled_; }
    [[nodiscard]] QVariantMap presentationState() const;
    [[nodiscard]] bool compactMode() const noexcept { return compactMode_; }
    [[nodiscard]] bool productSidebarVisible() const noexcept { return productSidebarVisible_; }
    [[nodiscard]] int conversationWidth() const noexcept { return conversationWidth_; }
    [[nodiscard]] QString conversationModel() const;
    [[nodiscard]] QString conversationEffort() const;
    [[nodiscard]] QString conversationProvider() const;
    [[nodiscard]] QString conversationPermission() const;
    [[nodiscard]] QQuickView* quickView() const;

    void prepareBenchmarkMessages(int count);
    void setBenchmarkScrollProgress(double progress);
    [[nodiscard]] int benchmarkMessageCount() const;
    [[nodiscard]] int benchmarkWarmTranscriptCount() const;
    [[nodiscard]] bool benchmarkUsableViewport() const;
    [[nodiscard]] bool benchmarkMaterializing() const {
        return activeMessageModel_->materializing();
    }

    Q_INVOKABLE void selectWorktree(const QString& path);
    Q_INVOKABLE void openRelativeFile(const QString& path);
    Q_INVOKABLE void requestTerminal();
    Q_INVOKABLE void requestSettings() { emit settingsRequested(); }
    Q_INVOKABLE void requestTagFilter(const QString& tag) { emit tagFilterRequested(tag); }
    Q_INVOKABLE void requestProductNavigation(const QString& destination) {
        if (!destination.isEmpty())
            emit productNavigationRequested(destination);
    }
    Q_INVOKABLE void requestAddWorktree();
    Q_INVOKABLE void requestWorktreeContextMenu(const QString& path) {
        emit worktreeContextMenuRequested(path);
    }
    Q_INVOKABLE void requestNewSession();
    Q_INVOKABLE void requestClearSession();
    Q_INVOKABLE void requestCurrentFileDiff();
    Q_INVOKABLE void requestFileDiff(const QString& path);
    Q_INVOKABLE QString absoluteFilePath(const QString& path) const;
    Q_INVOKABLE QString displayFilePath(const QString& path) const;
    Q_INVOKABLE QString revealFileActionText() const;
    Q_INVOKABLE QString revealableFilePath(const QString& path) const;
    Q_INVOKABLE void revealFile(const QString& path) const;
    // True once a background check found no file or directory at `path`. Never blocks: an
    // unchecked or stale path is queued for a batched off-thread check and reads as present
    // until that check lands; pathStateRevision changes only when an answer flips.
    Q_INVOKABLE bool pathMissing(const QString& path);
    Q_INVOKABLE void copyText(const QString& text);
    // The image viewer's host tools (AgentWorkspaceImages.cpp). `source` is a viewer item's
    // URL and `rotation` the viewer's turn in degrees.
    Q_INVOKABLE QString imageCaption(const QUrl& source) const;
    Q_INVOKABLE bool copyImage(const QUrl& source, int rotation);
    // The saved path, or empty when the image could not be written.
    Q_INVOKABLE QString saveImageToDownloads(const QUrl& source);
    Q_INVOKABLE bool printImage(const QUrl& source, int rotation, const QString& title);
    // The viewer's Fullscreen tool turns the top-level window, not the embedded Quick one.
    Q_INVOKABLE void toggleWindowFullScreen();
    Q_INVOKABLE void copySelectedText(QString text);
    Q_INVOKABLE QString composerDraft(const QString& scope) const;
    Q_INVOKABLE void saveComposerDraft(const QString& scope, const QString& text) const;
    // The reserved Mermaid box, so the overlay that draws it and the document
    // that reserves it cannot disagree about its size.
    Q_INVOKABLE qreal mermaidFrameHeight(qreal availableWidth) const;
    Q_INVOKABLE void formatComposerDocument(QObject* document, qreal fontPixelSize);
    Q_INVOKABLE void formatMessageDocument(QObject* document, qreal fontPixelSize,
                                           qreal availableWidth);
    Q_INVOKABLE QVariantList messageCodeBlocks(QObject* document) const;
    // Rects for the rounded chips behind inline code, one per wrapped line.
    Q_INVOKABLE QVariantList messageInlineCode(QObject* document) const;
    QUrl messageBaseUrl() const;
    Q_INVOKABLE QUrl messageImageAt(QObject* document, int position) const;
    Q_INVOKABLE QString messageLinkAt(QObject* document, int position) const;
    // Oriented pixel size from the image header (no decode); invalid if unknown.
    Q_INVOKABLE QSize messageImageSize(const QUrl& source) const;
    Q_INVOKABLE void selectConversation(const QString& conversationId);
    Q_INVOKABLE void requestRenameConversation(const QString& conversationId);
    Q_INVOKABLE void requestArchiveConversation(const QString& conversationId) {
        if (!conversationId.isEmpty())
            emit conversationArchiveRequested(conversationId);
    }
    Q_INVOKABLE void requestDeleteConversation(const QString& conversationId) {
        if (!conversationId.isEmpty())
            emit conversationDeleteRequested(conversationId);
    }
    Q_INVOKABLE void requestForkConversation(const QString& conversationId) {
        if (!conversationId.isEmpty())
            emit conversationForkRequested(conversationId);
    }
    Q_INVOKABLE void stopAgent();
    Q_INVOKABLE void submitPrompt(const QString& prompt);
    Q_INVOKABLE void answerQuestion(const QString& messageKey, const QVariantMap& answers) {
        emit questionAnswered(messageKey, answers);
    }
    Q_INVOKABLE QString resolveTranscriptPayload(const QString& reference) {
        return activeMessageModel_ ? activeMessageModel_->resolvePayload(reference) : QString{};
    }
    Q_INVOKABLE QString transcriptCopyText() const;
    Q_INVOKABLE QString entryCopyText(const QVariantMap& entry) const;
    Q_INVOKABLE QStringList codeBlockSources(const QString& markdown) const;
    // Edit menu routing: delivers the shortcut directly to the focused Quick item.
    void sendEditShortcut(QKeySequence::StandardKey key);
    // Edit > Find routing: open the transcript find bar, or step to the next/previous match.
    void openTranscriptFind();
    void findInTranscript(bool backwards);
    // Set by hosts whose own Find/Find Next/Find Previous actions route to the transcript; the
    // workspace then registers no competing shortcuts for those sequences.
    [[nodiscard]] bool findActionsProvided() const noexcept { return findActionsProvided_; }
    void setFindActionsProvided(bool provided);
    // Whether keyboard focus is inside this workspace, its Quick scene included.
    [[nodiscard]] bool hasFocusWithin() const;
    using TranscriptPayloadSource =
        std::function<QVariant(const QString&, const QString&, const QString&)>;
    void setTranscriptPayloadSource(TranscriptPayloadSource source);
    // Numbered conversations on this computer ({number, title, provider, running, path}).
    using ConversationDirectory = std::function<QVariantList()>;
    void setConversationDirectory(ConversationDirectory directory) {
        conversationDirectory_ = std::move(directory);
    }

  signals:
    void tagFilterRequested(const QString& tag);
    void activityGroupRequested(const QString& scope, const QString& key, bool expanded);
    void historyStateChanged();
    void findActionsProvidedChanged();
    void transcriptInputActiveChanged(bool active);
    void transcriptScrollingChanged(bool scrolling, const QString& readingKey);
    void transcriptReadingKeyChanged(const QString& readingKey);
    void transcriptExtentReported(double renderedHeight, double viewportHeight);
    void olderMessagesRequested(double viewportHeight, double averageRowHeight,
                                qint64 requestToken);
    void newerMessagesRequested(double viewportHeight, double averageRowHeight,
                                qint64 requestToken);
    void latestMessagesRequested();
    void conversationCopyRequested();
    void conversationCopyCanceled();
    void mermaidChanged();
    void pathStatesChanged();
    void composerScopeChanged();
    void steeringPendingChanged();
    void steeringPromptSubmitted(const QString& prompt, const QVariantList& attachments,
                                 const QString& displayText);
    void composerErrorOccurred(const QString& message);
    void fileMentionsChanged();
    // The worktree file list that inline-code links resolve against arrived or changed.
    void fileLinksChanged();
    void steeringAccepted(const QString& scope, const QString& prompt);
    void modelsChanged();
    void skillsChanged();
    void executionWorktreeRequested();
    void conversationWorktreeRequested(const QString& conversationId);
    void executionBranchChanged();
    void modelsRequested();
    void weeklyLimitChanged();
    void providerRuntimeChanged();
    void usageSnapshotChanged();
    void claudeAccountUsageChanged();
    void backgroundTasksChanged();
    void backgroundTaskOutputChanged();
    void atlasResultsChanged();
    void atlasResultOpenRequested(const QString& path, const QString& root, bool inBrowser);
    void heldMessageStatesChanged();
    void heldMessageActionRequested(const QString& messageId, bool release);
    void reviewerStateChanged();
    // Both empty reads the reviewer; otherwise as chooseReviewer.
    void reviewerRequested();
    void reviewerChangeRequested(const QString& number, const QString& newProvider);
    void backgroundTaskStopRequested(const QString& taskId);
    void backgroundTaskOutputRequested(const QString& taskId);
    void backgroundTaskFailed(const QString& message);
    void weeklyLimitRequested();
    void attachmentsChanged();
    void configuredPromptSubmitted(const QString& prompt, const QVariantMap& options,
                                   const QVariantList& attachments);
    void worktreesChanged();
    void conversationsChanged();
    void activePathChanged();
    void workingChangesChanged();
    void repositoryNameChanged();
    void repositoryPickerRequested();
    void monoFontFamilyChanged();
    void documentStyleChanged();
    void sessionChanged();
    void agentAvailableChanged();
    void restoringChanged();
    void presentationStateChanged();
    void conversationConfigurationChanged();
    void modelConfigurationStateChanged();
    void modelConfigurationFinished(const QString& error);
    void messageModelChanged();
    void compactModeChanged();
    void productSidebarVisibleChanged();
    void compactWindowChanged();
    void compactWindowRequested();
    void conversationWidthChanged();
    void conversationConfigurationRequested(const QString& model, const QString& effort,
                                            const QString& permission, const QString& provider,
                                            const QVariantMap& changes);
    void worktreeActivated(const QString& path);
    void fileOpenRequested(const QString& relativePath);
    void fileDiffRequested(const QString& relativePath);
    void terminalRequested();
    void settingsRequested();
    void productNavigationRequested(const QString& destination);
    void atlasRunningCountChanged();
    void addWorktreeRequested();
    void worktreeContextMenuRequested(const QString& path);
    void newSessionRequested();
    void clearSessionRequested();
    void currentFileDiffRequested();
    void conversationActivated(const QString& conversationId);
    void conversationRenameRequested(const QString& conversationId);
    void conversationArchiveRequested(const QString& conversationId);
    void conversationDeleteRequested(const QString& conversationId);
    void conversationForkRequested(const QString& conversationId);
    void stopRequested();
    void promptSubmitted(const QString& prompt);
    void questionAnswered(const QString& messageKey, const QVariantMap& answers);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QString executionBranch_;
    QString executionPath_;
    bool executionContextSwitched_{false};
    void flushStreamingMessages();
    QTimer* streamUpdateTimer_{nullptr};
    QMap<int, QVariantMap> pendingStreamingMessages_;
    AgentMessageModel* activeMessageModel_{nullptr};
    QHash<QString, AgentMessageModel*> messageModels_;
    TranscriptPayloadSource transcriptPayloadSource_;
    ConversationDirectory conversationDirectory_;
    QVariantMap historyState_;
    QQuickItem* transcriptPolish_{nullptr};
    MermaidRenderer* mermaidRenderer_{nullptr};
    ThemePalette mermaidPalette_;
    QSet<QString> mermaidImages_;
    QString workingChangesRoot_;
    QSet<QString> workingChanges_;
    int workingChangesRevision_{0};
    // The diagram source behind each rendered image, for the viewer's dark rendering.
    QHash<QString, QString> mermaidDiagrams_;
    // "messageKey#ordinal" of the Mermaid fences the reader switched to their diagram.
    QSet<QString> mermaidRendered_;
    // What each fence of a message turned into the last time it was rendered: kept as source
    // (a code frame) or replaced by one image. Frames and images are matched back to fences by it.
    struct FenceOutcome {
        bool mermaid{false};
        int ordinal{-1};
        bool complete{true};
        bool image{false};
    };
    QHash<QString, QVector<FenceOutcome>> fenceOutcomes_;
    mutable QHash<QString, QSize> messageImageSizes_;
    int mermaidRevision_{0};
    struct PathState {
        bool exists{true};
        qint64 checkedAt{0};
    };
    void checkPendingPaths();
    void applyPathStates(const QHash<QString, bool>& results, qint64 checkedAt);
    QHash<QString, PathState> pathStates_;
    QHash<QString, QString> pendingPathChecks_;
    bool pathCheckRunning_{false};
    int pathStateRevision_{0};
    QProcess* mermaidInstaller_{nullptr};
    bool mermaidInstallOffered_{false};
    QString mermaidInstallOutput_;
    QString mermaidInstallError_;
    void rebuildWorktreeModel();
    void rebuildSkillModel();
    void rebuildSkillModelLater();
    void settleSkillModel();
    void loadEnabledSkills();
    void saveEnabledSkills() const;
    void saveAttachmentDraft(const QString& scope) const;
    void saveAttachmentDraft(const QString& scope, const QVariantList& attachments) const;
    [[nodiscard]] QVariantList loadAttachmentDraft(const QString& scope) const;
    [[nodiscard]] QVariantList skillsForPrompt(const QString& prompt) const;
    // False when a mention points outside the conversation worktree; the error is emitted.
    [[nodiscard]] bool referencedPathsForPrompt(const QString& prompt, QVariantList& paths);
    void setComposerScope(const QString& scope);
    void setReviewerScope(const QString& scope);

    QString composerScope_;
    bool steeringPending_{false};
    QString steeringScope_;
    QString steeringPrompt_;
    QString steeringSubmittedPrompt_;
    QVariantList steeringAttachments_;
    QHash<QString, QVariantList> attachmentDrafts_;
    QVariantList models_;
    QVariantMap weeklyLimit_;
    QVariantMap providerRuntime_;
    QVariantMap usageSnapshot_;
    QVariantMap claudeAccountUsage_;
    UsageRequester claudeUsageRequester_;
    QVariantList backgroundTasks_;
    AtlasResultInspector atlasResultInspector_;
    QVariantMap heldMessageStates_;
    QString heldMessageScope_;
    QVariantMap reviewerState_;
    QString reviewerScope_;
    QVariantMap backgroundTaskOutput_;
    QVariantList skills_;
    QSet<QString> enabledSkillPaths_;
    quint64 skillScanGeneration_{0};
    bool skillScanPending_{false};
    QString fileMentionRoot_;
    QStringList fileMentionEntries_;
    QStringList fileMentionFoldedEntries_;
    QElapsedTimer fileMentionAge_;
    QString fileMentionLoadingRoot_;
    int fileMentionRevision_{0};
    QVariantList attachments_;
    QQuickView* view_{nullptr};
    bool sendingEditShortcut_{false};
    QWidget* container_{nullptr};
    QTimer* renderCacheCleanupTimer_{nullptr};
    // Agents delete and recreate worktrees mid-turn; nothing else re-reads the folder, so
    // the missing-worktree notice and status follow the disk on this timer.
    QTimer* folderPresenceTimer_{nullptr};
    TranscriptFileLinks fileLinks_;
    bool folderPresent_{true};
    QString activeRenderScope_;
    QVector<GitWorktree> sourceWorktrees_;
    QSet<QString> liveSessionPaths_;
    QSet<QString> conversationPaths_;
    QVariantList worktrees_;
    QVariantList conversations_;
    QString monoFontFamily_;
    // Bootstrap the HTML/Mermaid renderer even when the first palette is unchanged.
    bool themeApplied_{false};
    QString activePath_;
    QString repositoryName_;
    QString sessionTitle_{QStringLiteral("New conversation")};
    QString sessionStatus_{QStringLiteral("Ready")};
    bool sessionRunning_{false};
    int atlasRunningCount_{0};
    bool findActionsProvided_{false};
    bool agentAvailable_{true};
    bool restoring_{false};
    // Keep status indicators static to avoid a continuous Qt Quick render loop.
    bool motionEnabled_{false};
    bool workingMotionEnabled_{!qEnvironmentVariableIsSet("KETPLUS_REDUCED_MOTION")};
    bool compactMode_{false};
    bool productSidebarVisible_{true};
    bool compactWindow_{false};
    int conversationWidth_{defaultConversationWidth};
    qint64 conversationConfigurationRevision_{0};
    QVariant conversationCompactWindow_;
    bool compactWindowSupported_{false};
    bool configurationWritable_{true};
    bool modelConfigurationApplying_{false};
    QString modelConfigurationError_;
    QString conversationModel_;
    QString conversationEffort_;
    QString conversationProvider_;
    QString conversationPermission_{QStringLiteral("workspace-write")};

    void touchRenderTranscript(const QString& scope);
    void releaseRenderTranscript(const QString& scope);
    static void pruneRenderTranscripts();
    static void removeRenderTranscripts(AgentWorkspaceView* owner);
};

} // namespace ketplus
