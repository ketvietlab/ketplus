#include "KetDialog.h"
#include "agent/CodexAppServer.h"
#include "app/AgentConversationWidth.h"
#include "app/AgentWorktreeCreator.h"
#include "app/AgentWorktreeDialog.h"
#include "app/MainWindow.h"
#include "app/RepositoryRouter.h"
#include "app/SpotlightDialog.h"
#include "atlas/AtlasTab.h"
#include "atlas/ManagedAtlasViewers.h"
#include "workspace/AtlasIndex.h"

#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexRateLimits.h"
#include "agent/ProviderRuntimeStatus.h"
#include "app/ExecutionWorktreeDialog.h"
#include "app/SettingsDialog.h"
#include "editor/EditorWidget.h"
#include "git/GitChangesPanel.h"
#include "git/GitDiffView.h"
#include "git/GitService.h"
#include "preview/MarkdownPreviewPane.h"
#include "terminal/TerminalPanel.h"
#include "ui/FindReplaceBar.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"
#include "workspace/ExplorerPanel.h"

#include <memory>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace ketplus {
namespace {
QString normalizedPath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
}

} // namespace

AgentWorkspaceView* MainWindow::agentWorkspaceContaining(QWidget* widget) {
    for (; widget != nullptr; widget = widget->parentWidget()) {
        if (auto* workspace = qobject_cast<AgentWorkspaceView*>(widget))
            return workspace;
    }
    return nullptr;
}

// While an agent workspace has focus every editing command stays enabled, even with nothing to
// act on. AppKit claims the key equivalent of a disabled menu item without delivering the key,
// so a disabled Copy swallowed Cmd+C meant for selected transcript text. Enabled, the shortcut
// first goes to the focused text, which claims it; a command chosen from the menu is forwarded.
bool MainWindow::enableAgentEditActions() {
    if (agentWorkspaceContaining(QApplication::focusWidget()) == nullptr)
        return false;
    for (auto* action : {undoAction_, redoAction_, cutAction_, copyAction_, pasteAction_,
                         deleteAction_, selectAllAction_})
        action->setEnabled(true);
    return true;
}

bool MainWindow::forwardEditShortcut(const QKeySequence::StandardKey key) {
    auto* workspace = agentWorkspaceContaining(QApplication::focusWidget());
    if (workspace == nullptr)
        return false;
    workspace->sendEditShortcut(key);
    return true;
}

void MainWindow::ensureAgentWorkspace() {
    if (agentWorkspace_ != nullptr) {
        return;
    }

    agentWorkspace_ = new AgentWorkspaceView(workspaceSplit_);
    connectAtlasResults(agentWorkspace_);
    auto* atlasViewers = ManagedAtlasViewers::instance();
    agentWorkspace_->setAtlasRunningCount(atlasViewers->runningCount());
    connect(atlasViewers, &ManagedAtlasViewers::changed, agentWorkspace_, [this, atlasViewers] {
        agentWorkspace_->setAtlasRunningCount(atlasViewers->runningCount());
    });
    agentWorkspace_->setProductSidebarVisible(false);
    agentWorkspace_->setFindActionsProvided(true);
    // Only this workspace follows the preference; Atlas's embedded chat keeps
    // the default column.
    agentWorkspace_->setConversationWidth(AgentConversationWidth::load());
    connect(AgentConversationWidth::notifier(), &AgentConversationWidth::changed, agentWorkspace_,
            &AgentWorkspaceView::setConversationWidth);
    connect(agentWorkspace_, &AgentWorkspaceView::repositoryPickerRequested, this,
            &MainWindow::chooseFolder);
    connect(agentWorkspace_, &AgentWorkspaceView::teamRequested, this, &MainWindow::openTeam);
    connect(agentWorkspace_, &AgentWorkspaceView::teamTaskRequested, this,
            &MainWindow::openTeamTask);
    auto updateRepositoryName = [this, lastPath = QString()]() mutable {
        const auto path = agentWorkspace_->executionPath();
        if (lastPath == path)
            return;
        lastPath = path;
        agentWorkspace_->setRepositoryName(
            QFileInfo(RepositoryRouter::cachedRepositoryRoot(path)).fileName());
        auto* lookup = new QFutureWatcher<QString>(agentWorkspace_);
        connect(lookup, &QFutureWatcher<QString>::finished, agentWorkspace_, [this, lookup, path] {
            if (agentWorkspace_->executionPath() == path)
                agentWorkspace_->setRepositoryName(QFileInfo(lookup->result()).fileName());
            lookup->deleteLater();
        });
        lookup->setFuture(QtConcurrent::run(
            &repositoryLookupPool_, [path] { return RepositoryRouter::repositoryRoot(path); }));
    };
    connect(agentWorkspace_, &AgentWorkspaceView::activePathChanged, this, updateRepositoryName);
    updateRepositoryName();
    agentWorktreeCreator_ = new AgentWorktreeCreator(this);
    agentManager_ = new AgentSessionManager(this);
    setupSidebarStorage();
    setupSidebarAccount();
    connect(
        agentManager_, &AgentSessionManager::executionContextChanged, this,
        [this](const QString& path, const QString& conversation, const QString& branch,
               const QString& executionPath, qint64 revision) {
            if (agentWorkspace_->activePath() == path &&
                agentManager_->activeConversationId(path) == conversation) {
                const auto previousRepository =
                    sidebarRepositoryForPath(agentWorkspace_->executionPath());
                agentWorkspace_->setExecutionBranch(branch, executionPath, revision);
                const auto repository = sidebarRepositoryForPath(executionPath);
                agentWorkspace_->setRepositoryName(QFileInfo(repository).fileName());
                if (repository != previousRepository)
                    promotedSidebarRepository_ = repository;
                if (normalizedPath(executionPath) !=
                    normalizedPath(fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_))
                    setFileContextWorktree(executionPath);
                scheduleSidebarRefresh(true);
                agentWorkspace_->setSessionState(agentManager_->title(path),
                                                 QFileInfo(executionPath).isDir()
                                                     ? agentManager_->status(path)
                                                     : QStringLiteral("Worktree missing"),
                                                 agentManager_->isRunning(path));
                agentWorkspace_->setWorkingDetail(agentManager_->workingDetail(path));
            }
        });
    connect(agentWorkspace_, &AgentWorkspaceView::compactWindowRequested, this,
            [this] { setCompactConversationWindow(!compactConversationWindow_); });
    connect(agentWorkspace_, &AgentWorkspaceView::executionWorktreeRequested, this, [this] {
        const auto path = agentWorkspace_->activePath();
        const auto conversation = agentManager_->activeConversationId(path);
        QStringList roots = RepositoryRouter::cachedRecentRepositories();
        const auto currentRepository = sidebarRepositoryForPath(agentWorkspace_->executionPath());
        if (!roots.contains(currentRepository))
            roots.prepend(currentRepository);
        showExecutionWorktreeDialog(
            this,
            [this, path, conversation](const QString& type, const QJsonObject& payload,
                                       auto callback) {
                if (agentWorkspace_->activePath() != path ||
                    agentManager_->activeConversationId(path) != conversation) {
                    callback({}, "The conversation changed. Reopen the picker.");
                    return;
                }
                agentManager_->executionWorktreeCommand(path, type, payload, std::move(callback));
            },
            [this, path, conversation](const QString& selectedPath) {
                if (agentWorkspace_->activePath() != path ||
                    agentManager_->activeConversationId(path) != conversation)
                    return;
                setFileContextWorktree(selectedPath);
                promotedSidebarRepository_ = sidebarRepositoryForPath(selectedPath);
                scheduleSidebarRefresh(true);
            },
            agentWorkspace_->executionPath(), roots, currentRepository,
            // Worktrees are made here, for the conversation; there is no Worktrees screen.
            [this](const QString& repository, qint64 revision) {
                createConversationWorktree(repository, revision);
            });
    });
    connect(agentWorkspace_, &AgentWorkspaceView::activePathChanged, this,
            [this] { agentWorkspace_->setExecutionBranch({}); });

    connect(agentWorkspace_, &AgentWorkspaceView::activityGroupRequested, this,
            [this](const QString& scope, const QString& key, bool expanded) {
                agentManager_->setActivityGroupOpen(agentWorkspace_->activePath(), scope, key,
                                                    expanded);
            });
    agentWorkspace_->setTranscriptPayloadSource([this](const QString& scope, const QString& key,
                                                       const QString& kind) {
        return agentManager_->transcriptPayload(agentWorkspace_->activePath(), scope, key, kind);
    });
    workspaceSplit_->insertWidget(0, agentWorkspace_);
    agentWorkspace_->applyTheme(theme_.palette(), appearanceSettings_.interfaceZoomPercent);
    agentWorkspace_->setAgentAvailable(agentManager_->isAvailable());
    // ADR 0027: `#` completion lists every numbered conversation on this computer.
    agentWorkspace_->setConversationDirectory(
        [this] { return agentManager_->numberedConversations(); });
    connect(agentManager_, &AgentSessionManager::restoreStateChanged, agentWorkspace_,
            &AgentWorkspaceView::setRestoring);
    agentWorkspace_->setRestoring(agentManager_->isRestoring());
    connect(agentManager_->rateLimits(), &CodexRateLimits::weeklyLimitChanged, agentWorkspace_,
            &AgentWorkspaceView::setWeeklyLimit);
    connect(agentWorkspace_, &AgentWorkspaceView::weeklyLimitRequested, agentManager_->rateLimits(),
            &CodexRateLimits::refresh);
    agentManager_->rateLimits()->refresh();
    connect(agentManager_->providerStatus(), &ProviderRuntimeStatus::changed, agentWorkspace_,
            &AgentWorkspaceView::setProviderRuntime);
    agentWorkspace_->setProviderRuntime(agentManager_->providerStatus()->state());
    agentManager_->providerStatus()->refresh();
    // ADR 0110: the provider setup guide in the conversation.
    agentWorkspace_->setTurnGuide(
        [status = agentManager_->providerStatus()](const QString& provider, const QString& code,
                                                   const QString& message, const QString& model) {
            return status->turnGuide(provider, code, message, model);
        });
    connect(agentWorkspace_, &AgentWorkspaceView::providerRecheckRequested,
            agentManager_->providerStatus(),
            [status = agentManager_->providerStatus()] { status->refresh(true); });
    connect(agentWorkspace_, &AgentWorkspaceView::typeInTerminalRequested, this,
            &MainWindow::typeInTerminal);
    agentWorkspace_->setClaudeUsageRequester([this](AgentWorkspaceView::UsageReply reply) {
        agentManager_->appServer()->call("provider/claudeCredentials/analytics.refresh", {},
                                         std::move(reply));
    });
    connect(agentManager_, &AgentSessionManager::usageSnapshotChanged, this,
            [this](const QString& path, const QVariantMap& snapshot) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath()))
                    agentWorkspace_->setUsageSnapshot(snapshot);
            });
    connect(agentManager_, &AgentSessionManager::backgroundTasksChanged, this,
            [this](const QString& path, const QVariantList& tasks) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath()))
                    agentWorkspace_->setBackgroundTasks(tasks);
            });
    connect(agentManager_, &AgentSessionManager::backgroundTaskResult, this,
            [this](const QString& path, const QString& taskId, const QString& action,
                   const QVariantMap& result) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath()))
                    agentWorkspace_->acceptBackgroundTaskResult(taskId, action, result);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::backgroundTaskFailed, this,
            [this](const QString& message) {
                statusBar()->showMessage(tr("Unable to stop the background task: %1").arg(message),
                                         7000);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::heldMessageActionRequested, this,
            [this](const QString& messageId, bool release) {
                agentManager_->heldMessageAction(agentWorkspace_->activePath(), messageId, release);
            });
    connect(agentManager_, &AgentSessionManager::heldMessageResult, this,
            [this](const QString& path, const QString&, const QString& messageId,
                   const QVariantMap& result) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath()))
                    agentWorkspace_->acceptHeldMessageState(messageId, result);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::reviewerRequested, this,
            [this] { agentManager_->readReviewer(agentWorkspace_->activePath()); });
    connect(agentWorkspace_, &AgentWorkspaceView::reviewerChangeRequested, this,
            [this](const QString& number, const QString& newProvider) {
                agentManager_->setReviewer(agentWorkspace_->activePath(), number, newProvider);
            });
    connect(agentManager_, &AgentSessionManager::reviewerResult, this,
            [this](const QString& path, const QString&, const QVariantMap& result) {
                if (normalizedPath(path) != normalizedPath(agentWorkspace_->activePath()))
                    return;
                agentWorkspace_->acceptReviewerState(result);
                if (const QString error = result.value("error").toString();
                    !error.isEmpty() && result.value("change").toBool())
                    statusBar()->showMessage(tr("Unable to change the reviewer: %1").arg(error),
                                             7000);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::backgroundTaskStopRequested, this,
            [this](const QString& taskId) {
                agentManager_->stopBackgroundTask(agentWorkspace_->activePath(), taskId);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::backgroundTaskOutputRequested, this,
            [this](const QString& taskId) {
                agentManager_->readBackgroundTaskOutput(agentWorkspace_->activePath(), taskId);
            });
    connect(
        agentWorkspace_, &AgentWorkspaceView::worktreeActivated, this, [this](const QString& path) {
            if (!QFileInfo(path).isDir()) {
                agentWorkspace_->setActivePath(path);
                agentManager_->activate(path);
                statusBar()->showMessage(QStringLiteral(
                    "Worktree missing. The agent continues from the repository's main worktree."));
                return;
            }
            if (normalizedPath(path) == normalizedPath(workspaceRoot_)) {
                agentWorkspace_->setActivePath(path);
                agentManager_->activate(path);
                refreshAgentWorktrees();
            } else {
                switchToWorktree(path);
            }
        });
    connect(agentWorkspace_, &AgentWorkspaceView::fileOpenRequested, this,
            &MainWindow::openTranscriptLink);
    connect(agentWorkspace_, &AgentWorkspaceView::fileDiffRequested, this,
            [this](const QString& relativePath) {
                const QString path = agentWorkspace_->absoluteFilePath(relativePath);
                if (!QFileInfo(path).isFile()) {
                    statusBar()->showMessage(QStringLiteral("File not found: %1").arg(relativePath),
                                             3000);
                    return;
                }
                openTranscriptFile(path, true);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::modelsRequested, agentManager_,
            [this] { agentManager_->loadModels(); });
    connect(agentManager_, &AgentSessionManager::modelsLoaded, agentWorkspace_,
            &AgentWorkspaceView::setModels);
    agentManager_->loadModels();
    connect(agentWorkspace_, &AgentWorkspaceView::conversationConfigurationRequested, this,
            [this](const QString& model, const QString& effort, const QString& permission,
                   const QString& provider, const QVariantMap& changes) {
                agentManager_->setConversationConfiguration(agentWorkspace_->activePath(), model,
                                                            effort, permission, provider, changes);
            });
    connect(
        agentWorkspace_, &AgentWorkspaceView::configuredPromptSubmitted, this,
        [this](const QString& prompt, const QVariantMap& options, const QVariantList& attachments) {
            agentManager_->submitPrompt(agentWorkspace_->activePath(), prompt, options,
                                        attachments);
        });
    connect(agentWorkspace_, &AgentWorkspaceView::promptSubmitted, this,
            [this](const QString& prompt) {
                agentManager_->submitPrompt(agentWorkspace_->activePath(), prompt);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::composerErrorOccurred, this,
            [this](const QString& message) { statusBar()->showMessage(message, 7000); });
    connect(
        agentWorkspace_, &AgentWorkspaceView::steeringPromptSubmitted, this,
        [this](const QString& prompt, const QVariantList& attachments, const QString& displayText) {
            agentManager_->steerPrompt(agentWorkspace_->activePath(), prompt, attachments,
                                       displayText);
        });
    connect(agentWorkspace_, &AgentWorkspaceView::questionAnswered, this,
            [this](const QString& messageKey, const QVariantMap& answers) {
                agentManager_->answerQuestion(agentWorkspace_->activePath(), messageKey, answers);
            });
    connect(agentManager_, &AgentSessionManager::steeringFinished, agentWorkspace_,
            &AgentWorkspaceView::finishSteering);
    connect(agentWorkspace_, &AgentWorkspaceView::terminalRequested, this, [this] {
        if (!QFileInfo(agentWorkspace_->activePath()).isDir()) {
            statusBar()->showMessage(
                QStringLiteral("Worktree missing. Restore the folder to open a terminal."), 7000);
            return;
        }
        openAgentTerminalTab();
    });
    connectAgentWorkReport();
    connect(agentWorkspace_, &AgentWorkspaceView::tagFilterRequested, activityBar_,
            &ActivityBar::setTagFilter);
    connect(agentWorkspace_, &AgentWorkspaceView::settingsRequested, this,
            &MainWindow::showSettingsSection);
    connect(agentWorkspace_, &AgentWorkspaceView::productNavigationRequested, this,
            [this](const QString& destination) {
                if (destination == QStringLiteral("search"))
                    showSpotlight();
                else if (destination == QStringLiteral("workspace"))
                    setAgentWorkspaceVisible(true);
                else if (destination == QStringLiteral("explorer"))
                    setExplorerVisible(true);
                else if (destination == QStringLiteral("source-control"))
                    setSourceControlVisible(true);
                else if (destination == QStringLiteral("atlas"))
                    showConversationAtlases();
                else if (destination == QStringLiteral("connections"))
                    setRemoteWorkspaceVisible(true);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::addWorktreeRequested, this,
            &MainWindow::createAgentWorktree);
    connect(agentWorkspace_, &AgentWorkspaceView::worktreeContextMenuRequested, this,
            &MainWindow::showAgentWorktreeContextMenu);
    connect(agentWorktreeCreator_, &AgentWorktreeCreator::removed, this,
            [this](const QString& path) {
                agentManager_->archiveConversationsForWorktree(path);
                git_->scheduleRefresh(0);
                if (!pendingDeleteWorktreePaths_.isEmpty() ||
                    !pendingDeleteWorktreeSkipped_.isEmpty()) {
                    removeNextPendingLinkedWorktree(); // Also reports skipped worktrees.
                    return;
                }
                clearPendingWorktreeDeletion();
                statusBar()->showMessage(tr("Worktree deleted. Its branch was kept."), 5000);
            });
    connect(agentWorktreeCreator_, &AgentWorktreeCreator::created, this,
            [this](const QString& path, const QString& branchName) {
                statusBar()->showMessage(QStringLiteral("Created worktree for %1").arg(branchName),
                                         3000);
                promotedWorktreePath_ = normalizedPath(path);
                if (!pickerWorktreeAgentPath_.isEmpty()) {
                    moveConversationToCreatedWorktree(path, branchName);
                    git_->scheduleRefresh(0);
                    return;
                }
                const bool forConversation = pendingConversationWorktree_;
                pendingConversationWorktree_ = false;
                switchToWorktree(path);
                if (forConversation && agentManager_ != nullptr)
                    agentManager_->markActiveConversationOwnsWorktree(path);
                // "Start a conversation in it": open the new worktree's conversation.
                if (std::exchange(openCreatedWorktree_, false) && !forConversation)
                    setAgentWorkspaceVisible(true);
                git_->scheduleRefresh(0);
            });
    connect(agentWorktreeCreator_, &AgentWorktreeCreator::pruned, this,
            [this] { git_->scheduleRefresh(0); });
    connect(agentWorktreeCreator_, &AgentWorktreeCreator::failed, this,
            [this](const QString& error) {
                pendingConversationWorktree_ = false;
                openCreatedWorktree_ = false;
                pickerWorktreeAgentPath_.clear();
                pickerWorktreeConversation_.clear();
                if (!pendingDeleteConversationPath_.isEmpty() ||
                    !pendingDeleteWorktreePaths_.isEmpty()) {
                    clearPendingWorktreeDeletion();
                    auto* notice = new QMessageBox(
                        QMessageBox::Warning, tr("Unable to delete worktree"),
                        tr("The conversation was deleted, but a linked worktree could not be "
                           "removed:\n%1\n\nEnable force delete if Git refused a dirty, untracked, "
                           "or locked worktree.")
                            .arg(error),
                        QMessageBox::Ok, this);
                    notice->setObjectName(QStringLiteral("worktreeDeletionError"));
                    notice->setAttribute(Qt::WA_DeleteOnClose);
                    notice->open();
                    return;
                }
                QMessageBox::warning(this, QStringLiteral("Worktree operation failed"), error);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::newSessionRequested, this,
            [this] { startNewAgentConversation(); });
    connect(agentWorkspace_, &AgentWorkspaceView::clearSessionRequested, this,
            [this] { agentManager_->newSession(agentWorkspace_->activePath()); });
    connect(agentWorkspace_, &AgentWorkspaceView::currentFileDiffRequested, this, [this] {
        if (!QFileInfo(agentWorkspace_->activePath()).isDir()) {
            statusBar()->showMessage(
                QStringLiteral("Worktree missing. Restore the folder to view file changes."), 7000);
            return;
        }
        requestCurrentFileDiff();
    });
    connect(agentWorkspace_, &AgentWorkspaceView::conversationActivated, this,
            [this](const QString& conversationId) {
                agentManager_->selectConversation(agentWorkspace_->activePath(), conversationId);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::stopRequested, this,
            [this] { agentManager_->cancel(agentWorkspace_->activePath()); });
    connect(agentWorkspace_, &AgentWorkspaceView::conversationWorktreeRequested, this,
            [this](const QString& id) {
                changeAgentConversationWorktree(agentWorkspace_->activePath(), id);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::conversationRenameRequested, this,
            [this](const QString& conversationId) {
                QTimer::singleShot(0, this, [this, conversationId] {
                    renameAgentConversation(agentWorkspace_->activePath(), conversationId);
                });
            });
    connect(agentWorkspace_, &AgentWorkspaceView::conversationTagRequested, this,
            [this](const QString& conversationId, const QString& label, const bool tagged) {
                agentManager_->setConversationTag(agentWorkspace_->activePath(), conversationId,
                                                  label, tagged);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::conversationArchiveRequested, this,
            [this](const QString& conversationId) {
                archiveAgentConversation(agentWorkspace_->activePath(), conversationId);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::conversationDeleteRequested, this,
            [this](const QString& conversationId) {
                deleteAgentConversation(agentWorkspace_->activePath(), conversationId);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::conversationForkRequested, this,
            [this](const QString& conversationId) {
                forkAgentConversation(agentWorkspace_->activePath(), conversationId);
            });
    connect(agentManager_, &AgentSessionManager::messagesReset, this,
            [this](const QString& path, const QVariantList& messages) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath())) {
                    agentWorkspace_->setExecutionBranch({});
                    agentManager_->executionWorktreeCommand(
                        path, "session.worktrees", {{"limit", 1}},
                        [](const QJsonObject&, const QString&) {});
                    agentWorkspace_->resetMessages(messages,
                                                   normalizedPath(path) + ":" +
                                                       agentManager_->activeConversationId(path));
                }
            });
    connect(agentWorkspace_, &AgentWorkspaceView::transcriptInputActiveChanged, agentManager_,
            &AgentSessionManager::setTranscriptInputActive);
    connect(agentWorkspace_, &AgentWorkspaceView::transcriptScrollingChanged, agentManager_,
            &AgentSessionManager::setTranscriptScrolling);
    connect(agentWorkspace_, &AgentWorkspaceView::transcriptReadingKeyChanged, agentManager_,
            &AgentSessionManager::setTranscriptReadingKey);
    connect(agentWorkspace_, &AgentWorkspaceView::transcriptExtentReported, agentManager_,
            &AgentSessionManager::setTranscriptExtent);
    connect(agentWorkspace_, &AgentWorkspaceView::olderMessagesRequested, this,
            [this](const double viewportHeight, const double averageRowHeight,
                   const qint64 requestToken) {
                agentManager_->loadOlderMessages(agentWorkspace_->activePath(), viewportHeight,
                                                 averageRowHeight, requestToken);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::newerMessagesRequested, this,
            [this](const double viewportHeight, const double averageRowHeight,
                   const qint64 requestToken) {
                agentManager_->loadNewerMessages(agentWorkspace_->activePath(), viewportHeight,
                                                 averageRowHeight, requestToken);
            });
    connect(agentWorkspace_, &AgentWorkspaceView::latestMessagesRequested, this,
            [this] { agentManager_->loadLatestMessages(agentWorkspace_->activePath()); });
    connect(agentWorkspace_, &AgentWorkspaceView::conversationCopyRequested, this,
            [this] { agentManager_->copyConversation(agentWorkspace_->activePath()); });
    connect(agentWorkspace_, &AgentWorkspaceView::conversationCopyCanceled, this,
            [this] { agentManager_->cancelConversationCopy(agentWorkspace_->activePath()); });
    connect(agentManager_, &AgentSessionManager::messagesPrepended, this,
            [this](const QString& path, const QVariantList& messages) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath()))
                    agentWorkspace_->prependMessages(messages);
            });
    connect(agentManager_, &AgentSessionManager::messagesWindowReset, this,
            [this](const QString& path, const QVariantList& messages) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath()))
                    agentWorkspace_->replaceMessageWindow(messages);
            });
    connect(agentManager_, &AgentSessionManager::historyStateChanged, this,
            [this](const QString& path, const QVariantMap& state) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath()))
                    agentWorkspace_->setHistoryState(state);
            });
    connect(agentManager_, &AgentSessionManager::conversationCopyReady, this,
            [this](const QString& path, const QString& id, const QString& text) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath()) &&
                    agentManager_->activeConversationId(path) == id)
                    agentWorkspace_->copyText(text);
            });
    connect(agentManager_, &AgentSessionManager::messageAppended, this,
            [this](const QString& path, const int, const QVariantMap& message) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath())) {
                    agentWorkspace_->appendMessage(message);
                }
            });
    connect(agentManager_, &AgentSessionManager::messageUpdated, this,
            [this](const QString& path, const int index, const QVariantMap& message) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath())) {
                    agentWorkspace_->updateMessage(index, message);
                }
                const QString kind = message.value(QStringLiteral("kind")).toString();
                if (kind == QStringLiteral("tool") || kind == QStringLiteral("file")) {
                    git_->scheduleRefresh(350);
                }
            });
    connect(agentManager_, &AgentSessionManager::workingDetailChanged, this,
            [this](const QString& path, const QString& detail) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath()))
                    agentWorkspace_->setWorkingDetail(detail);
            });
    connect(agentManager_, &AgentSessionManager::sessionStateChanged, this,
            [this](const QString& path, const QString& title, const QString& status,
                   const bool running) {
                agentWorkspace_->setAgentAvailable(agentManager_->isAvailable());
                scheduleSidebarRefresh();
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath())) {
                    agentWorkspace_->setSessionState(
                        title,
                        QFileInfo(agentWorkspace_->executionPath()).isDir()
                            ? status
                            : QStringLiteral("Worktree missing"),
                        running);
                    agentWorkspace_->setWorkingDetail(agentManager_->workingDetail(path));
                }
            });
    connect(agentManager_, &AgentSessionManager::conversationsChanged, this,
            [this](const QString& path, const QVariantList& conversations) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath()))
                    agentWorkspace_->setConversations(conversations);
                // The sidebar lists every repository's conversations, not only this worktree's.
                scheduleSidebarRefresh();
            });
    connect(agentManager_, &AgentSessionManager::conversationConfigurationApplyFinished, this,
            [this](const QString& path, const QString& conversationId, const QString& error) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath()) &&
                    agentManager_->activeConversationId(path) == conversationId)
                    agentWorkspace_->finishModelConfiguration(error);
            });
    connect(agentManager_, &AgentSessionManager::conversationConfigurationChanged, this,
            [this](const QString& path, const QVariantMap& configuration) {
                if (normalizedPath(path) == normalizedPath(agentWorkspace_->activePath())) {
                    agentWorkspace_->setConversationConfiguration(configuration);
                }
            });
    connect(agentManager_, &AgentSessionManager::activityChanged, this, [this] {
        refreshAgentWorktrees();
        // A completed agent turn may have created or removed linked worktrees.
        git_->scheduleRefresh(200);
    });
    connect(agentManager_, &AgentSessionManager::errorOccurred, this,
            [this](const QString& message) { statusBar()->showMessage(message, 7000); });
    connect(
        agentManager_, &AgentSessionManager::conversationForkFinished, this,
        [this](const QString& path, const QString&, const QString& forkId, const QString& error) {
            if (forkId.isEmpty()) {
                statusBar()->showMessage(tr("Unable to fork the conversation: %1").arg(error),
                                         7000);
                return;
            }
            openSidebarConversation(path, forkId);
            statusBar()->showMessage(tr("Forked the conversation."), 5000);
        });
    connect(agentManager_, &AgentSessionManager::conversationDeletionFinished, this,
            [this](const QString& path, const QString&, const QString& error) {
                if (!error.isEmpty()) {
                    clearPendingWorktreeDeletion();
                    auto* notice =
                        new QMessageBox(QMessageBox::Warning, tr("Unable to delete conversation"),
                                        error, QMessageBox::Ok, this);
                    notice->setObjectName(QStringLiteral("conversationDeletionError"));
                    notice->setAttribute(Qt::WA_DeleteOnClose);
                    notice->open();
                    return;
                }
                if (pendingDeleteConversationPath_.isEmpty() ||
                    normalizedPath(pendingDeleteConversationPath_) != normalizedPath(path))
                    return;
                pendingDeleteConversationPath_.clear();
                removeNextPendingLinkedWorktree();
            });
}

void MainWindow::createAgentWorktree() {
    if (workspaceRoot_.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Unable to create worktree"),
                             QStringLiteral("Open a Git repository first."));
        return;
    }

    // A shortcut can ask again while the card is open.
    if (agentDialogOpen(this, QStringLiteral("agentWorktreeDialogCard")))
        return;
    const QString repositoryPath = workspaceRoot_;
    auto* form = new AgentWorktreeDialog(repositoryPath);
    connect(form->present(this), &ket::Dialog::accepted, this, [this, form, repositoryPath] {
        // The folder can change while the card is open.
        if (workspaceRoot_ != repositoryPath)
            return;
        openCreatedWorktree_ = form->startConversation();
        agentWorktreeCreator_->create(repositoryPath, primaryWorktreePath(), form->branchName(),
                                      form->baseBranch());
    });
}

QString MainWindow::primaryWorktreePath() const {
    QString primary = workspaceRoot_;
    for (const GitWorktree& worktree : worktrees_) {
        if (!worktree.bare) {
            return worktree.path;
        }
    }
    return primary;
}

void MainWindow::startNewAgentConversation() {
    if (agentManager_ == nullptr || agentWorkspace_ == nullptr)
        return;
    const QString activePath = agentWorkspace_->activePath();
    if (workspaceRoot_.isEmpty() ||
        !QFileInfo(QDir(workspaceRoot_).filePath(QStringLiteral(".git"))).exists()) {
        agentManager_->newSession(activePath);
        return;
    }
    // A shortcut can ask again while the card is open.
    if (agentDialogOpen(this, QStringLiteral("agentConversationDialogCard")))
        return;
    const QString repositoryPath = workspaceRoot_;
    // The sidebar selects by the conversation on screen, which can sit in another
    // worktree of this repository than the open folder; the dialog opens on the
    // same one, so it never starts somewhere the reader is not.
    const QString anchorPath =
        !activePath.isEmpty() && QFileInfo(activePath).isDir() ? activePath : repositoryPath;
    QVector<GitWorktree> choices;
    for (const GitWorktree& worktree : std::as_const(worktrees_)) {
        if (worktree.bare || worktree.prunable || worktree.path.isEmpty() ||
            !QFileInfo(worktree.path).isDir())
            continue;
        choices.append(worktree);
    }
    auto* form =
        new AgentWorktreeDialog(anchorPath, nullptr, AgentWorktreeDialog::Purpose::Conversation,
                                primaryWorktreePath(), choices);
    // Worktrees made outside KetPlus (another agent, a terminal) change nothing the
    // repository watcher sees, so the cached list can miss them: list them again and
    // update the form in place rather than delay it.
    connect(git_, &GitService::worktreesChanged, form, &AgentWorktreeDialog::setWorktrees);
    connect(form->present(this), &ket::Dialog::accepted, this, [this, form, repositoryPath] {
        // The worktree list belongs to the repository the card opened on.
        if (workspaceRoot_ != repositoryPath)
            return;
        if (!form->createWorktree()) {
            const QString selected = form->selectedWorktreePath();
            if (!selected.isEmpty())
                switchToWorktree(selected);
            agentManager_->newSession(selected.isEmpty() ? agentWorkspace_->activePath()
                                                         : selected);
            return;
        }
        pendingConversationWorktree_ = true;
        // A new worktree is cut from a branch, not from another worktree: Git resolves
        // the base ref the same way in any checkout of the repository.
        agentWorktreeCreator_->create(primaryWorktreePath(), primaryWorktreePath(),
                                      form->branchName(), form->baseBranch());
    });
    git_->refresh();
}

void MainWindow::setAgentWorkspaceVisible(const bool visible) {
    if (!visible && compactConversationWindow_)
        setCompactConversationWindow(false);
    // Sidebar navigation selects its target after showing the workspace. When
    // it already shows this workspace, activating the previous session resets
    // and hydrates a transcript that will immediately be replaced. A retained
    // missing-worktree conversation still needs its view path restored.
    const QString activePath = workspaceRoot_.isEmpty() ? QDir::homePath() : workspaceRoot_;
    if (visible && agentWorkspaceMode_ && agentWorkspace_ != nullptr &&
        agentWorkspace_->activePath() == activePath)
        return;
    if (!visible) {
        if (!agentWorkspaceMode_) {
            return;
        }
        if (agentWorkspace_ != nullptr) {
            agentWorkspace_->hide();
        }
        if (agentDetailPane_ != nullptr) {
            agentDetailPane_->hide();
        }
        editorSplit_->show();
        terminalAction_->setEnabled(true);
        agentWorkspaceMode_ = false;
        const QSignalBlocker blocker(agentWorkspaceAction_);
        agentWorkspaceAction_->setChecked(false);
        updateAgentDetailTabs();
        return;
    }

    hideUtilityWorkspaces();
    setRemoteWorkspaceVisible(false);
    setAtlasWorkspaceVisible(false);
    ensureAgentWorkspace();
    if (explorer_ != nullptr) {
        explorer_->hide();
    }
    if (gitChanges_ != nullptr) {
        gitChanges_->hide();
    }
    hideSearchPanel();
    if (terminal_ != nullptr) {
        terminal_->hide();
    }
    {
        const QSignalBlocker blocker(terminalAction_);
        terminalAction_->setChecked(false);
    }
    agentWorkspace_->show();
    editorSplit_->hide();
    agentWorkspaceMode_ = true;
    // The desktop shell owns the one shared product sidebar for every mode.
    // AgentWorkspace renders only the conversation surface when embedded here.
    setShellMode(ActivityBar::Mode::AgentWorkspace);
    terminalAction_->setEnabled(true);

    const QSignalBlocker explorerBlocker(explorerAction_);
    const QSignalBlocker sourceControlBlocker(sourceControlAction_);
    const QSignalBlocker agentBlocker(agentWorkspaceAction_);
    explorerAction_->setChecked(false);
    sourceControlAction_->setChecked(false);
    agentWorkspaceAction_->setChecked(true);

    agentWorkspace_->setActivePath(activePath);
    agentManager_->activate(activePath);
    refreshAgentWorktrees();
    if (agentDetailTabs_ != nullptr && agentDetailTabs_->count() > 0) {
        updateAgentDetailTabs();
    } else {
        QList<int> sizes(workspaceSplit_->count(), 0);
        sizes[workspaceSplit_->indexOf(agentWorkspace_)] = qMax(800, workspaceSplit_->width());
        workspaceSplit_->setSizes(sizes);
        agentWorkspace_->focusPrompt();
    }
}

void MainWindow::syncAgentWorkspaceContext(const bool loadTranscript) {
    if (agentWorkspace_ == nullptr || agentManager_ == nullptr || workspaceRoot_.isEmpty()) {
        return;
    }
    agentWorkspace_->setActivePath(workspaceRoot_);
    agentManager_->activate(workspaceRoot_, loadTranscript, navigatingConversationId_);
    refreshAgentWorktrees();
}

void MainWindow::clearPendingWorktreeDeletion() {
    pendingDeleteConversationPath_.clear();
    pendingDeleteWorktreePaths_.clear();
    pendingDeleteWorktreeRepository_.clear();
    pendingDeleteWorktreeForce_ = false;
    pendingDeleteWorktreeSkipped_.clear();
}

void MainWindow::removeNextPendingLinkedWorktree() {
    const QString primary = primaryWorktreePath();
    const QString repository =
        pendingDeleteWorktreeRepository_.isEmpty() ? primary : pendingDeleteWorktreeRepository_;
    while (!pendingDeleteWorktreePaths_.isEmpty()) {
        const QString worktree = pendingDeleteWorktreePaths_.takeFirst();
        if (normalizedPath(worktree) == normalizedPath(primary))
            continue;
        if (!QFileInfo(QDir(worktree).filePath(QStringLiteral(".git"))).isFile())
            continue;
        // The same checks as the sidebar's "Delete worktree…", except that the current
        // worktree is switched away from below instead of refusing.
        const QString blocker = worktreeRemovalBlocker(QFileInfo(worktree).canonicalFilePath());
        if (!blocker.isEmpty()) {
            pendingDeleteWorktreeSkipped_.append(
                tr("%1: %2").arg(QDir::toNativeSeparators(worktree), blocker));
            continue;
        }
        if (normalizedPath(worktree) == normalizedPath(workspaceRoot_) &&
            normalizedPath(primary) != normalizedPath(worktree))
            switchToWorktree(primary);
        agentWorktreeCreator_->remove(repository, worktree, pendingDeleteWorktreeForce_);
        return;
    }
    if (!pendingDeleteWorktreeSkipped_.isEmpty()) {
        auto* notice =
            new QMessageBox(QMessageBox::Warning, tr("Some worktrees were kept"),
                            tr("These worktrees were not deleted:\n%1")
                                .arg(pendingDeleteWorktreeSkipped_.join(QLatin1Char('\n'))),
                            QMessageBox::Ok, this);
        notice->setObjectName(QStringLiteral("worktreeDeletionSkipped"));
        notice->setAttribute(Qt::WA_DeleteOnClose);
        notice->open();
    }
    clearPendingWorktreeDeletion();
    git_->scheduleRefresh(0);
}

void MainWindow::refreshAgentWorktrees() {
    updateAtlasActivity();
    if (agentWorkspace_ == nullptr) {
        return;
    }
    const QStringList archivedValues =
        QSettings().value(QStringLiteral("agent/archivedWorktrees")).toStringList();
    const QSet<QString> archived(archivedValues.cbegin(), archivedValues.cend());
    QVector<GitWorktree> visibleWorktrees;
    for (const GitWorktree& worktree : std::as_const(worktrees_)) {
        if (worktree.bare || worktree.prunable || !QFileInfo(worktree.path).isDir() ||
            !QFileInfo(QDir(worktree.path).filePath(QStringLiteral(".git"))).exists())
            continue;
        if (!archived.contains(QFileInfo(worktree.path).canonicalFilePath()) ||
            normalizedPath(worktree.path) == normalizedPath(agentWorkspace_->activePath())) {
            visibleWorktrees.append(worktree);
        }
    }
    prioritizeWorktree(visibleWorktrees);
    const QSet<QString> runningPaths =
        agentManager_ == nullptr ? QSet<QString>{} : agentManager_->runningPaths();
    scheduleSidebarRefresh();
    agentWorkspace_->setWorktrees(
        visibleWorktrees,
        agentWorkspace_->activePath().isEmpty()
            ? (workspaceRoot_.isEmpty() ? QDir::homePath() : workspaceRoot_)
            : agentWorkspace_->activePath(),
        runningPaths,
        agentManager_ == nullptr ? QSet<QString>{} : agentManager_->conversationPaths());
    if (agentManager_ != nullptr) {
        const QString path = agentWorkspace_->activePath();
        agentWorkspace_->setSessionState(agentManager_->title(path),
                                         QFileInfo(agentWorkspace_->executionPath()).isDir()
                                             ? agentManager_->status(path)
                                             : QStringLiteral("Worktree missing"),
                                         agentManager_->isRunning(path));
        agentWorkspace_->setWorkingDetail(agentManager_->workingDetail(path));
    }
}

} // namespace ketplus
