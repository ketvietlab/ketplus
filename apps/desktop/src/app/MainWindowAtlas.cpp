#include "KetButton.h"
#include "KetIcons.h"
#include "agent/CodexAppServer.h"
#include "app/AtlasBuilderTab.h"
#include "app/AtlasConversationPanel.h"
#include "app/MainWindow.h"
#include "app/RepositoryRouter.h"
#include "app/SpotlightDialog.h"
#include "atlas/AtlasTab.h"
#include "workspace/AtlasIndex.h"
#include "workspace/SearchPanel.h"

#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexRateLimits.h"
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

#include "ui/SymbolIcon.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
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
#include <QPainter>
#include <QResizeEvent>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStyleOptionToolButton>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrentRun>

#include <algorithm>

namespace {
QString normalizedSpotlightPath(const QString& path) {
    const QFileInfo file(path);
    const auto canonical = file.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
}

class AtlasTabWidget final : public QTabWidget {
  public:
    explicit AtlasTabWidget(QWidget* parent = nullptr) : QTabWidget(parent) {
        addButton_ = new ket::IconButton(ket::Icons::icon(ket::Icons::codeForLucide(u"plus")),
                                         tr("New Atlas tab"), this);
        addButton_->setObjectName(QStringLiteral("atlasNewTabButton"));
        setCornerWidget(addButton_, Qt::TopRightCorner);
    }

    QAbstractButton* addButton() const { return addButton_; }

    void configureCloseButton(QWidget* tab) {
        auto* close = new ket::IconButton(ket::Icons::icon(ket::Icons::codeForLucide(u"x")),
                                          tr("Close Atlas tab"), this);
        close->setObjectName(QStringLiteral("atlasCloseTabButton"));
        tabBar()->setTabButton(indexOf(tab), QTabBar::RightSide, close);
        connect(close, &QAbstractButton::clicked, this, [this, tab] {
            const int index = indexOf(tab);
            if (index >= 0)
                emit tabCloseRequested(index);
        });
    }

  private:
    QAbstractButton* addButton_{nullptr};
};
} // namespace

namespace ketplus {
void MainWindow::showSpotlight() {
    if (auto* existing = findChild<SpotlightDialog*>("spotlightDialog")) {
        existing->close();
        return;
    }
    auto* dialog = new SpotlightDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    atlasIndex_->scheduleRefresh(0);
    refreshWorkspaceFileIndex(false);
    populateSpotlight();
    connect(dialog, &SpotlightDialog::entryActivated, this, [this](const QString& id) {
        const int separator = id.indexOf(':');
        const auto type = id.left(separator);
        const auto path = id.mid(separator + 1);
        if (type == "atlas") {
            for (const auto& atlas : atlasIndex_->atlases())
                if (atlas.path == path) {
                    openAtlas(atlas);
                    break;
                }
        } else if (type == "file") {
            if (auto* editor = currentEditor(); editor != nullptr && !editor->isHibernated())
                pushNavigationLocation(locationOf(editor));
            openFile(path);
        }
    });
    dialog->show();
    dialog->activateWindow();
}

void MainWindow::refreshAtlasIndex() { atlasIndex_->setRoot(workspaceRoot_); }

void MainWindow::populateSpotlight() {
    const auto fileRoot = fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_;
    auto* dialog = findChild<SpotlightDialog*>("spotlightDialog");
    if (!dialog)
        return;
    QVector<SpotlightEntry> entries;
    QVector<SpotlightEntry> fileEntries;
    QSet<QString> indexedFiles;
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (editor == nullptr || editor->document().isUntitled())
            continue;
        const auto path = normalizedSpotlightPath(editor->document().filePath());
        if (path.isEmpty() || indexedFiles.contains(path))
            continue;
        indexedFiles.insert(path);
        const auto relative = QDir(fileRoot).relativeFilePath(path);
        fileEntries.append({"file:" + path, QFileInfo(path).fileName(),
                            relative.startsWith(QStringLiteral("..")) ? path : relative,
                            QStringLiteral("Files"), true, QStringLiteral("file")});
    }
    for (const auto& relative :
         {QStringLiteral("docs/design-system.md"), QStringLiteral("README.md")}) {
        const auto path = normalizedSpotlightPath(QDir(fileRoot).filePath(relative));
        if (!QFileInfo::exists(path) || indexedFiles.contains(path))
            continue;
        indexedFiles.insert(path);
        fileEntries.append({"file:" + path, QFileInfo(path).fileName(), QFileInfo(relative).path(),
                            QStringLiteral("Files"), true, QStringLiteral("file")});
    }
    // Spotlight finds files and Atlases only; repositories and worktrees are reached
    // from the sidebar's conversations.
    for (const auto& atlas : atlasIndex_->atlases()) {
        bool opened = false;
        for (auto* tab : findChildren<AtlasTab*>())
            if (tab->atlasPath() == atlas.path)
                opened = true;
        entries.append({"atlas:" + atlas.path, atlas.title,
                        (opened ? tr("Open - ") : QString{}) + atlas.relativePath, "Atlases", true,
                        "atlas", atlas.title + ' ' + atlas.path});
    }
    // Workspace files come from the path-only index; contents are never indexed.
    // Paths are joined with cleanPath only: the index skips symlinks and the root is
    // already canonical, so no per-file filesystem lookups run on the UI thread.
    if (!fileRoot.isEmpty() && workspaceFilesRoot_ == fileRoot) {
        const QDir root(fileRoot);
        for (const auto& relativePath : workspaceFiles_) {
            const auto path = QDir::cleanPath(root.absoluteFilePath(relativePath));
            if (indexedFiles.contains(path))
                continue;
            indexedFiles.insert(path);
            fileEntries.append({"file:" + path, QFileInfo(relativePath).fileName(), relativePath,
                                QStringLiteral("Files"), true, QStringLiteral("file")});
        }
    }
    dialog->setEntries(fileEntries + entries);
}

AtlasTab* MainWindow::createAtlasTab() {
    auto* host = atlasTabs_;
    auto* tab = new AtlasTab(atlasIndex_, host);
    host->addTab(tab, tr("Atlases"));
    static_cast<AtlasTabWidget*>(host)->configureCloseButton(tab);
    connect(tab, &AtlasTab::atlasRequested, this,
            [this, tab](const AtlasDescriptor& atlas) { openAtlas(atlas, tab); });
    connect(tab, &AtlasTab::creationRequested, this, [this, tab] { openAtlasBuilder(tab); });
    connect(tab, &AtlasTab::titleChanged, this, [host, tab](const QString& title) {
        const int index = host->indexOf(tab);
        if (index >= 0) {
            host->setTabText(index, title);
            host->setTabToolTip(index, tab->atlasPath());
        }
    });
    host->setCurrentWidget(tab);
    updateAtlasActivity();
    return tab;
}

void MainWindow::openAtlasBuilder(AtlasTab* picker) {
    if (!picker || !atlasTabs_ || workspaceRoot_.isEmpty()) {
        statusBar()->showMessage(tr("Open a workspace before creating an Atlas."), 5000);
        return;
    }
    const int index = atlasTabs_->indexOf(picker);
    if (index < 0)
        return;

    const QString root = QFileInfo(workspaceRoot_).canonicalFilePath();
    for (int tabIndex = 0; tabIndex < atlasTabs_->count(); ++tabIndex) {
        auto* existing = qobject_cast<AtlasBuilderTab*>(atlasTabs_->widget(tabIndex));
        if (!existing || existing->hasBuiltAtlas() ||
            QFileInfo(existing->workspaceRoot()).canonicalFilePath() != root)
            continue;
        atlasTabs_->removeTab(index);
        picker->deleteLater();
        atlasTabs_->setCurrentWidget(existing);
        existing->chatView()->focusPrompt();
        updateAtlasActivity();
        return;
    }

    ensureAgentWorkspace();
    agentWorkspace_->hide();
    auto* builder = new AtlasBuilderTab(atlasIndex_, workspaceRoot_, atlasTabs_);
    builder->chatView()->applyTheme(theme_.palette(), appearanceSettings_.interfaceZoomPercent);
    connectAtlasBuilder(builder);

    atlasTabs_->removeTab(index);
    picker->deleteLater();
    atlasTabs_->insertTab(index, builder, tr("Create Atlas"));
    static_cast<AtlasTabWidget*>(atlasTabs_)->configureCloseButton(builder);
    atlasTabs_->setCurrentWidget(builder);

    agentManager_->newSession(workspaceRoot_);
    builder->setConversationId(agentManager_->activeConversationId(workspaceRoot_));
    auto* chat = builder->chatView();
    chat->setWorktrees(worktrees_, workspaceRoot_, agentManager_->runningPaths());
    chat->setConversations(agentManager_->conversations(workspaceRoot_));
    chat->setConversationConfiguration(agentManager_->conversationConfiguration(workspaceRoot_));
    chat->resetMessages(agentManager_->messages(workspaceRoot_),
                        workspaceRoot_ + ":" + builder->conversationId());
    chat->setHistoryState(agentManager_->historyState(workspaceRoot_));
    chat->setSessionState(agentManager_->title(workspaceRoot_),
                          agentManager_->status(workspaceRoot_),
                          agentManager_->isRunning(workspaceRoot_));
    chat->setAgentAvailable(agentManager_->isAvailable());
    chat->focusPrompt();
    updateAtlasActivity();
}

void MainWindow::connectAtlasBuilder(AtlasBuilderTab* builder) {
    auto* chat = builder->chatView();
    connectAtlasResults(chat);
    connect(chat, &AgentWorkspaceView::activityGroupRequested, builder,
            [this, builder](const QString& scope, const QString& key, bool expanded) {
                agentManager_->setActivityGroupOpen(builder->workspaceRoot(), scope, key, expanded);
            });
    chat->setTranscriptPayloadSource(
        [this, builder](const QString& scope, const QString& key, const QString& kind) {
            return agentManager_->transcriptPayload(builder->workspaceRoot(), scope, key, kind);
        });
    const auto matches = [this, builder](const QString& path) {
        return QFileInfo(path).canonicalFilePath() ==
                   QFileInfo(builder->workspaceRoot()).canonicalFilePath() &&
               agentManager_->activeConversationId(path) == builder->conversationId();
    };

    connect(builder, &AtlasBuilderTab::titleChanged, this, [this, builder](const QString& title) {
        const int index = atlasTabs_ ? atlasTabs_->indexOf(builder) : -1;
        if (index >= 0) {
            atlasTabs_->setTabText(index, title);
            atlasTabs_->setTabToolTip(index, builder->atlasPath());
        }
    });
    connect(builder, &AtlasBuilderTab::conversationRequested, this,
            &MainWindow::openSidebarConversation);
    connect(builder, &AtlasBuilderTab::atlasBuilt, this,
            [this](const AtlasDescriptor&) { populateSpotlight(); });

    connect(chat, &AgentWorkspaceView::modelsRequested, agentManager_,
            [this] { agentManager_->loadModels(); });
    connect(agentManager_, &AgentSessionManager::modelsLoaded, chat,
            &AgentWorkspaceView::setModels);
    chat->setModels(agentManager_->models());
    connect(chat, &AgentWorkspaceView::conversationConfigurationRequested, builder,
            [this, builder](const QString& model, const QString& effort, const QString& permission,
                            const QString& provider, const QVariantMap& changes) {
                agentManager_->selectConversation(builder->workspaceRoot(),
                                                  builder->conversationId());
                agentManager_->setConversationConfiguration(builder->workspaceRoot(), model, effort,
                                                            permission, provider, changes);
            });
    connect(chat, &AgentWorkspaceView::configuredPromptSubmitted, builder,
            [this, builder](const QString& prompt, QVariantMap options,
                            const QVariantList& attachments) {
                agentManager_->selectConversation(builder->workspaceRoot(),
                                                  builder->conversationId());
                options.insert(QStringLiteral("mode"), QStringLiteral("atlas-builder"));
                const auto context = builder->promptContext();
                for (auto it = context.cbegin(); it != context.cend(); ++it)
                    options.insert(it.key(), it.value());
                agentManager_->submitPrompt(builder->workspaceRoot(), prompt, options, attachments);
            });
    connect(chat, &AgentWorkspaceView::promptSubmitted, builder,
            [this, builder](const QString& prompt) {
                agentManager_->selectConversation(builder->workspaceRoot(),
                                                  builder->conversationId());
                auto options = builder->promptContext();
                options.insert(QStringLiteral("mode"), QStringLiteral("atlas-builder"));
                agentManager_->submitPrompt(builder->workspaceRoot(), prompt, options);
            });
    connect(chat, &AgentWorkspaceView::steeringPromptSubmitted, builder,
            [this, builder](const QString& prompt, const QVariantList& attachments,
                            const QString& displayText) {
                agentManager_->steerPrompt(builder->workspaceRoot(), prompt, attachments,
                                           displayText);
            });
    connect(chat, &AgentWorkspaceView::questionAnswered, builder,
            [this, builder](const QString& key, const QVariantMap& answers) {
                agentManager_->answerQuestion(builder->workspaceRoot(), key, answers);
            });
    connect(chat, &AgentWorkspaceView::stopRequested, builder,
            [this, builder] { agentManager_->cancel(builder->workspaceRoot()); });
    connect(chat, &AgentWorkspaceView::olderMessagesRequested, builder,
            [this, builder](const double viewportHeight, const double averageRowHeight,
                            const qint64 requestToken) {
                agentManager_->loadOlderMessages(builder->workspaceRoot(), viewportHeight,
                                                 averageRowHeight, requestToken);
            });
    connect(chat, &AgentWorkspaceView::newerMessagesRequested, builder,
            [this, builder](const double viewportHeight, const double averageRowHeight,
                            const qint64 requestToken) {
                agentManager_->loadNewerMessages(builder->workspaceRoot(), viewportHeight,
                                                 averageRowHeight, requestToken);
            });
    connect(chat, &AgentWorkspaceView::latestMessagesRequested, builder,
            [this, builder] { agentManager_->loadLatestMessages(builder->workspaceRoot()); });
    connect(chat, &AgentWorkspaceView::conversationCopyRequested, builder,
            [this, builder] { agentManager_->copyConversation(builder->workspaceRoot()); });
    connect(chat, &AgentWorkspaceView::conversationCopyCanceled, builder,
            [this, builder] { agentManager_->cancelConversationCopy(builder->workspaceRoot()); });

    connect(agentManager_, &AgentSessionManager::messagesReset, builder,
            [builder, chat, matches](const QString& path, const QVariantList& messages) {
                if (matches(path))
                    chat->resetMessages(messages, path + ":" + builder->conversationId());
            });
    connect(agentManager_, &AgentSessionManager::messagesPrepended, builder,
            [chat, matches](const QString& path, const QVariantList& messages) {
                if (matches(path))
                    chat->prependMessages(messages);
            });
    connect(agentManager_, &AgentSessionManager::messagesWindowReset, builder,
            [chat, matches](const QString& path, const QVariantList& messages) {
                if (matches(path))
                    chat->replaceMessageWindow(messages);
            });
    connect(agentManager_, &AgentSessionManager::historyStateChanged, builder,
            [chat, matches](const QString& path, const QVariantMap& state) {
                if (matches(path))
                    chat->setHistoryState(state);
            });
    connect(agentManager_, &AgentSessionManager::conversationCopyReady, builder,
            [chat, matches](const QString& path, const QString&, const QString& text) {
                if (matches(path))
                    chat->copyText(text);
            });
    connect(agentManager_, &AgentSessionManager::messageAppended, builder,
            [chat, matches](const QString& path, int, const QVariantMap& message) {
                if (matches(path))
                    chat->appendMessage(message);
            });
    connect(agentManager_, &AgentSessionManager::messageUpdated, builder,
            [this, chat, matches](const QString& path, int index, const QVariantMap& message) {
                if (matches(path))
                    chat->updateMessage(index, message);
                const QString kind = message.value(QStringLiteral("kind")).toString();
                if (matches(path) &&
                    (kind == QStringLiteral("tool") || kind == QStringLiteral("file")))
                    atlasIndex_->scheduleRefresh();
            });
    connect(agentManager_, &AgentSessionManager::sessionStateChanged, builder,
            [this, chat, matches](const QString& path, const QString& title, const QString& status,
                                  bool running) {
                if (!matches(path))
                    return;
                chat->setSessionState(title, status, running);
                if (!running)
                    atlasIndex_->scheduleRefresh(0);
            });
    connect(agentManager_, &AgentSessionManager::conversationsChanged, builder,
            [chat, matches](const QString& path, const QVariantList& conversations) {
                if (matches(path))
                    chat->setConversations(conversations);
            });
    connect(
        agentManager_, &AgentSessionManager::conversationConfigurationApplyFinished, builder,
        [builder, chat](const QString& path, const QString& conversationId, const QString& error) {
            if (path == builder->workspaceRoot() && conversationId == builder->conversationId())
                chat->finishModelConfiguration(error);
        });
    connect(agentManager_, &AgentSessionManager::conversationConfigurationChanged, builder,
            [chat, matches](const QString& path, const QVariantMap& configuration) {
                if (matches(path))
                    chat->setConversationConfiguration(configuration);
            });
    chat->setClaudeUsageRequester([this](AgentWorkspaceView::UsageReply reply) {
        agentManager_->appServer()->call("provider/claudeCredentials/analytics.refresh", {},
                                         std::move(reply));
    });
    connect(agentManager_, &AgentSessionManager::usageSnapshotChanged, builder,
            [chat, matches](const QString& path, const QVariantMap& snapshot) {
                if (matches(path))
                    chat->setUsageSnapshot(snapshot);
            });
    connect(
        agentManager_, &AgentSessionManager::steeringFinished, builder,
        [chat](const QString& path, const QString& conversationId, const QString& prompt,
               bool accepted) { chat->finishSteering(path, conversationId, prompt, accepted); });
}

void MainWindow::openAtlasPicker() {
    refreshAtlasIndex();
    atlasIndex_->scheduleRefresh(0);
    const bool hasTabs = atlasTabs_ && atlasTabs_->count() > 0;
    setAtlasWorkspaceVisible(true);
    if (hasTabs)
        createAtlasTab();
}

void MainWindow::showConversationAtlases() {
    refreshAtlasIndex();
    atlasIndex_->scheduleRefresh(0);
    auto* tabs = ensureAgentDetailTabs();
    if (conversationAtlasPanel_.isNull()) {
        auto* panel = new AtlasConversationPanel(atlasIndex_, tabs);
        conversationAtlasPanel_ = panel;
        connect(panel, &AtlasConversationPanel::editRequested, this,
                &MainWindow::editAtlasInKetPlus);
        connect(panel, &AtlasConversationPanel::newRequested, this, [this] {
            setAtlasWorkspaceVisible(true);
            openAtlasBuilder(createAtlasTab());
        });
        tabs->addTab(panel, tr("Atlases"));
    }
    tabs->setCurrentWidget(conversationAtlasPanel_);
    updateAgentDetailTabs();
}

void MainWindow::editAtlasInKetPlus(const AtlasDescriptor& atlas) {
    if (atlas.path.isEmpty() || workspaceRoot_.isEmpty())
        return;
    setAtlasWorkspaceVisible(true);
    auto* picker = createAtlasTab();
    openAtlasBuilder(picker);
    if (auto* builder = qobject_cast<AtlasBuilderTab*>(atlasTabs_->currentWidget()))
        builder->openExistingAtlas(atlas);
}

void MainWindow::closeAtlasTab(const int index) {
    if (!atlasTabs_ || index < 0 || index >= atlasTabs_->count())
        return;
    auto* tab = atlasTabs_->widget(index);
    atlasTabs_->removeTab(index);
    delete tab;
    if (atlasTabs_->count() == 0)
        createAtlasTab();
    updateAtlasActivity();
}

void MainWindow::setAtlasWorkspaceVisible(const bool visible) {
    if (!visible) {
        if (!atlasWorkspaceMode_)
            return;
        atlasWorkspaceMode_ = false;
        atlasTabs_->hide();
        editorSplit_->show();
        terminalAction_->setEnabled(true);
        setShellMode(ActivityBar::Mode::Explorer);
        updateAtlasActivity();
        updateEditorActions();
        return;
    }
    setRemoteWorkspaceVisible(false);
    setAgentWorkspaceVisible(false);
    if (!atlasTabs_) {
        auto* atlasHost = new AtlasTabWidget(workspaceSplit_);
        atlasTabs_ = atlasHost;
        atlasTabs_->setObjectName(QStringLiteral("atlasWorkspaceTabs"));
        atlasTabs_->setDocumentMode(true);
        atlasTabs_->setMovable(true);
        atlasTabs_->setTabsClosable(false);
        atlasTabs_->tabBar()->setExpanding(false);
        atlasTabs_->tabBar()->setElideMode(Qt::ElideRight);
        atlasTabs_->tabBar()->setUsesScrollButtons(true);
        workspaceSplit_->addWidget(atlasTabs_);
        connect(atlasHost->addButton(), &QAbstractButton::clicked, this,
                &MainWindow::openAtlasPicker);
        connect(atlasTabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeAtlasTab);
        connect(atlasTabs_, &QTabWidget::currentChanged, this, &MainWindow::updateAtlasActivity);
    }
    if (explorer_)
        explorer_->hide();
    if (gitChanges_)
        gitChanges_->hide();
    if (searchPanel_) {
        searchPanel_->hide();
        const QSignalBlocker searchBlocker(searchPanelAction_);
        searchPanelAction_->setChecked(false);
    }
    if (terminal_)
        terminal_->hide();
    editorSplit_->hide();
    atlasWorkspaceMode_ = true;
    atlasTabs_->show();
    const QSignalBlocker explorerBlocker(explorerAction_);
    const QSignalBlocker sourceBlocker(sourceControlAction_);
    const QSignalBlocker terminalBlocker(terminalAction_);
    explorerAction_->setChecked(false);
    sourceControlAction_->setChecked(false);
    terminalAction_->setChecked(false);
    terminalAction_->setEnabled(false);
    setShellMode(ActivityBar::Mode::Atlas);
    if (atlasTabs_->count() == 0) {
        refreshAtlasIndex();
        createAtlasTab();
    }
    QList<int> sizes(workspaceSplit_->count(), 0);
    sizes[workspaceSplit_->indexOf(atlasTabs_)] = qMax(800, workspaceSplit_->width());
    workspaceSplit_->setSizes(sizes);
    updateAtlasActivity();
    updateEditorActions();
}

void MainWindow::openAtlas(const AtlasDescriptor& atlas, AtlasTab* picker) {
    for (auto* window : QApplication::topLevelWidgets()) {
        auto* other = qobject_cast<MainWindow*>(window);
        if (!other || other == this)
            continue;
        auto* host = other->atlasTabs_;
        if (!host)
            continue;
        for (int index = 0; index < host->count(); ++index) {
            auto* tab = qobject_cast<AtlasTab*>(host->widget(index));
            auto* builder = qobject_cast<AtlasBuilderTab*>(host->widget(index));
            if ((!tab || tab->atlasPath() != atlas.path) &&
                (!builder || builder->atlasPath() != atlas.path))
                continue;
            other->setAtlasWorkspaceVisible(true);
            host->setCurrentIndex(index);
            other->updateAtlasActivity();
            other->showNormal();
            other->raise();
            other->activateWindow();
            return;
        }
    }
    for (auto* host : {atlasTabs_}) {
        if (!host)
            continue;
        for (int index = 0; index < host->count(); ++index) {
            auto* tab = qobject_cast<AtlasTab*>(host->widget(index));
            auto* builder = qobject_cast<AtlasBuilderTab*>(host->widget(index));
            if ((tab && tab->atlasPath() == atlas.path) ||
                (builder && builder->atlasPath() == atlas.path)) {
                setAtlasWorkspaceVisible(true);
                host->setCurrentIndex(index);
                updateAtlasActivity();
                return;
            }
        }
    }
    setAtlasWorkspaceVisible(true);
    auto* tab = picker ? picker : qobject_cast<AtlasTab*>(atlasTabs_->currentWidget());
    if (!tab || !tab->atlasId().isEmpty())
        tab = createAtlasTab();
    tab->openAtlas(atlas);
    updateAtlasActivity();
}

void MainWindow::updateAtlasActivity() {
    for (auto* host : {atlasTabs_}) {
        if (!host)
            continue;
        for (int index = 0; index < host->count(); ++index)
            if (auto* tab = qobject_cast<AtlasTab*>(host->widget(index)))
                tab->setActive(index == host->currentIndex() && atlasWorkspaceMode_);
            else if (auto* builder = qobject_cast<AtlasBuilderTab*>(host->widget(index))) {
                const bool active = index == host->currentIndex() && atlasWorkspaceMode_;
                builder->setActive(active);
                if (active && agentManager_ && !builder->conversationId().isEmpty() &&
                    agentManager_->activeConversationId(builder->workspaceRoot()) !=
                        builder->conversationId()) {
                    agentManager_->selectConversation(builder->workspaceRoot(),
                                                      builder->conversationId());
                }
            }
    }
}

} // namespace ketplus
