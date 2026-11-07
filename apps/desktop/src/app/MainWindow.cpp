#include "app/MainWindow.h"

#include "app/RepositoryRouter.h"
#include "app/SettingsWorkspacePage.h"
#include "app/SpotlightDialog.h"
#include "app/WorktreesWorkspacePage.h"
#include "atlas/AtlasTab.h"
#include "editor/DiskChangePrompt.h"
#include "editor/DocumentFormatControls.h"
#include "workspace/AtlasIndex.h"

#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexRateLimits.h"
#include "app/SettingsDialog.h"
#include "editor/EditorSplitPane.h"
#include "editor/EditorWidget.h"
#include "git/GitChangesPanel.h"
#include "git/GitDiffView.h"
#include "git/GitService.h"
#include "index/SymbolIndex.h"
#include "preview/MarkdownPreviewPane.h"
#include "terminal/TerminalPanel.h"
#include "ui/FindReplaceBar.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "update/AppUpdater.h"
#include "workspace/ActivityBar.h"
#include "workspace/ExplorerPanel.h"
#include "workspace/SearchPanel.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

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

MainWindow::MainWindow(ThemeManager& theme, QWidget* parent)
    : QMainWindow(parent), theme_(theme), appearanceSettings_(AppearanceSettings::load()),
      viewOptions_(EditorViewOptions::load()), activityBar_(new ActivityBar(this)),
      mainSplit_(new QSplitter(Qt::Vertical, this)),
      workspaceSplit_(new QSplitter(Qt::Horizontal, mainSplit_)),
      editorSplit_(new QSplitter(Qt::Horizontal, workspaceSplit_)), tabs_(new QTabWidget),
      findBar_(new FindReplaceBar),
      cursorPositionLabel_(new QLabel(QStringLiteral("Ln 1, Col 1"), this)),
      documentStateLabel_(new QLabel(QStringLiteral("New"), this)),
      gitButton_(new QToolButton(this)) {
    repositoryLookupPool_.setMaxThreadCount(2);
    theme_.setInterfaceFontSizePixels(
        InterfaceZoomMetrics::snap(appearanceSettings_.interfaceFontSizePixels));
    setWindowTitle(QStringLiteral("KétPlus Pro"));
    atlasIndex_ = new AtlasIndex(this);
    connect(atlasIndex_, &AtlasIndex::changed, this, &MainWindow::populateSpotlight);
    refreshAtlasIndex();
    resize(1100, 720);
    setMinimumSize(680, 420);
    setUnifiedTitleAndToolBarOnMac(true);
    mainSplit_->setObjectName(QStringLiteral("mainSplit"));
    editorSplit_->setObjectName(QStringLiteral("editorSplit"));

    tabs_->setDocumentMode(true);
    tabs_->setMovable(true);
    tabs_->setTabsClosable(true);
    tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);

    editorPane_ = new QWidget(editorSplit_);
    auto* editorPane = editorPane_;
    auto* centralLayout = new QVBoxLayout(editorPane);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    documentSplit_ = new QSplitter(Qt::Horizontal, editorPane);
    documentSplit_->setObjectName(QStringLiteral("documentSplit"));
    documentSplit_->setChildrenCollapsible(false);
    documentSplit_->setHandleWidth(1);
    documentSplit_->addWidget(tabs_);
    centralLayout->addWidget(documentSplit_, 1);
    centralLayout->addWidget(findBar_);
    editorSplit_->setChildrenCollapsible(false);
    editorSplit_->setHandleWidth(1);
    editorSplit_->addWidget(editorPane);
    workspaceSplit_->setChildrenCollapsible(false);
    workspaceSplit_->setHandleWidth(1);
    workspaceSplit_->setMinimumSize(320, 240);
    workspaceSplit_->addWidget(editorSplit_);
    mainSplit_->setChildrenCollapsible(false);
    mainSplit_->setHandleWidth(1);
    mainSplit_->addWidget(workspaceSplit_);
    mainSplit_->setStretchFactor(0, 1);
    auto* central = new QWidget(this);
    auto* shellLayout = new QHBoxLayout(central);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    shellLayout->setSpacing(0);
    shellLayout->addWidget(activityBar_);
    auto* productSurface = new QWidget(central);
    productSurface->setObjectName(QStringLiteral("productSurface"));
    productSurface->setAttribute(Qt::WA_StyledBackground, true);
    productSurface->setStyleSheet(QStringLiteral("QWidget#productSurface { background: %1; }")
                                      .arg(theme_.palette().pageBackground));
    auto* productLayout = new QVBoxLayout(productSurface);
    productLayout->setContentsMargins(0, 0, 0, 0);
    productLayout->setSpacing(0);
    productLayout->addWidget(mainSplit_, 1);
    shellLayout->addWidget(productSurface, 1);
    setCentralWidget(central);

    git_ = new GitService(this);
    createActions();
    activityBar_->applyPalette(theme_.palette(), theme_.interfaceFontSizePixels());
    connect(activityBar_, &ActivityBar::quickSearchRequested, this, &MainWindow::showSpotlight);
    connect(activityBar_, &ActivityBar::repositoryRequested, this, &MainWindow::chooseFolder);
    connect(activityBar_, &ActivityBar::machineRequested, this, [this](const QString& id) {
        if (id == selectedMachineId_)
            return;
        selectedMachineId_ = id;
        activityBar_->setRemoteMachineSelected(id != QStringLiteral("local"));
        if (id == QStringLiteral("local")) {
            refreshSidebarRepositories();
            setAgentWorkspaceVisible(true);
        } else {
            activityBar_->setRepositories({});
            setRemoteWorkspaceVisible(true);
        }
    });
    connect(activityBar_, &ActivityBar::repositoryActivationRequested, this,
            [this](const QString& path) {
                // Open in this window; the sidebar spans repositories.
                if (normalizedPath(path) != activeSidebarRepository())
                    openFolder(path);
            });
    connect(activityBar_, &ActivityBar::newConversationRequested, this,
            [this](const QString& repositoryPath) {
                // The action is offered on every repository; open another one in this window.
                if (!repositoryPath.isEmpty() &&
                    normalizedPath(repositoryPath) != activeSidebarRepository())
                    openFolder(repositoryPath);
                setAgentWorkspaceVisible(true);
                startNewAgentConversation();
            });
    connect(activityBar_, &ActivityBar::conversationRequested, this,
            &MainWindow::openSidebarConversation);
    connect(activityBar_, &ActivityBar::conversationContextMenuRequested, this,
            [this](const QString& worktreePath, const QString& conversationId) {
                ensureAgentWorkspace();
                showAgentConversationContextMenu(normalizedPath(worktreePath), conversationId);
            });
    connect(activityBar_, &ActivityBar::conversationRenameRequested, this,
            [this](const QString& worktreePath, const QString& conversationId) {
                ensureAgentWorkspace();
                // Leave the double-click event before running the modal dialog.
                QMetaObject::invokeMethod(
                    this,
                    [this, path = normalizedPath(worktreePath), conversationId] {
                        renameAgentConversation(path, conversationId);
                    },
                    Qt::QueuedConnection);
            });
    connect(activityBar_, &ActivityBar::settingsRequested, this, &MainWindow::showSettings);
    connect(activityBar_, &ActivityBar::updatesRequested, this, &MainWindow::checkForUpdates);
    connect(&AppUpdater::instance(), &AppUpdater::pendingVersionChanged, activityBar_,
            &ActivityBar::setPendingUpdate);
    activityBar_->setPendingUpdate(AppUpdater::instance().pendingVersion());
    connect(activityBar_, &ActivityBar::identityRequested, this,
            [this] { setRemoteWorkspaceVisible(true); });
    connect(activityBar_, &ActivityBar::modeRequested, this, [this](const ActivityBar::Mode mode) {
        switch (mode) {
        case ActivityBar::Mode::Explorer:
            setExplorerVisible(true);
            break;
        case ActivityBar::Mode::Search:
            setSearchPanelVisible(true);
            break;
        case ActivityBar::Mode::SourceControl:
            setSourceControlVisible(true);
            break;
        case ActivityBar::Mode::AgentWorkspace:
            setAgentWorkspaceVisible(true);
            break;
        case ActivityBar::Mode::Atlas:
            setAtlasWorkspaceVisible(true);
            break;
        case ActivityBar::Mode::Remote:
            setRemoteWorkspaceVisible(true);
            break;
        case ActivityBar::Mode::Settings:
            showSettings();
            break;
        }
    });
    statusBar()->setSizeGripEnabled(false);
    statusBar()->setContentsMargins(activityBar_->sidebarWidth(), 0, 0, 0);
    connect(activityBar_, &ActivityBar::sidebarWidthChanged, this,
            [this](const int width) { statusBar()->setContentsMargins(width, 0, 0, 0); });
    statusBar()->showMessage(QStringLiteral("Ready"));
    gitButton_->setProperty("kvRole", QStringLiteral("statusGit"));
    gitButton_->setPopupMode(QToolButton::InstantPopup);
    // The status bar keeps Git status actions; switching worktrees stays in the Git menu.
    auto* statusGitMenu = new QMenu(this);
    statusGitMenu->setObjectName(QStringLiteral("statusGitMenu"));
    connect(statusGitMenu, &QMenu::aboutToShow, this, [this, statusGitMenu] {
        updateGitActions();
        git_->scheduleRefresh(0);
        statusGitMenu->clear();
        for (QAction* action : gitMenu_->actions()) {
            if (action != worktreeMenu_->menuAction()) {
                statusGitMenu->addAction(action);
            }
        }
        const auto actions = statusGitMenu->actions();
        if (!actions.isEmpty() && actions.last()->isSeparator()) {
            statusGitMenu->removeAction(actions.last());
        }
    });
    gitButton_->setMenu(statusGitMenu);
    gitButton_->setToolTip(QStringLiteral("Git repository"));
    gitButton_->setAccessibleName(QStringLiteral("Git repository"));
    gitButton_->hide();
    statusBar()->addPermanentWidget(gitButton_);
    for (QLabel* label : {documentStateLabel_, cursorPositionLabel_}) {
        label->setProperty("kvRole", QStringLiteral("status"));
        statusBar()->addPermanentWidget(label);
    }
    const auto addStatusButton = [this](QMenu* menu, const QString& accessibleName) {
        auto* button = new QToolButton(this);
        button->setProperty("kvRole", QStringLiteral("statusButton"));
        button->setPopupMode(QToolButton::InstantPopup);
        button->setMenu(menu);
        button->setToolTip(accessibleName);
        button->setAccessibleName(accessibleName);
        statusBar()->addPermanentWidget(button);
        return button;
    };
    format_->addStatusButtons(addStatusButton);

    connect(tabs_, &QTabWidget::tabCloseRequested, this,
            [this](const int index) { closeTab(index); });
    connect(tabs_->tabBar(), &QTabBar::customContextMenuRequested, this,
            &MainWindow::showTabContextMenu);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        // Switching tabs targets the tab editor, so the newly current tab is activated.
        splitFocused_ = false;
        updateAtlasActivity();
        activateCurrentTab();
        rememberActiveFileForWorktree();
        updateEditorActions();
        updateDocumentState();
        updateGitActions();
        updateMarkdownPreview();
        trackNavigation(currentEditor());
        if (findBar_->isVisible() && highlightTimer_ != nullptr) {
            highlightTimer_->start();
        }
    });
    connect(findBar_, &FindReplaceBar::findRequested, this, &MainWindow::findNext);
    connect(findBar_, &FindReplaceBar::replaceRequested, this, &MainWindow::replaceCurrentMatch);
    connect(findBar_, &FindReplaceBar::replaceAllRequested, this, &MainWindow::replaceAllMatches);
    connect(findBar_, &FindReplaceBar::closeRequested, this, [this] {
        highlightTimer_->stop();
        clearMatchHighlights();
        if (auto* editor = currentEditor()) {
            editor->setFocus();
        }
    });

    // Debounce match highlighting so typing a query never rescans on every key.
    highlightTimer_ = new QTimer(this);
    highlightTimer_->setSingleShot(true);
    highlightTimer_->setInterval(200);
    connect(highlightTimer_, &QTimer::timeout, this, [this] { refreshMatchHighlights(); });
    connect(findBar_, &FindReplaceBar::queryChanged, highlightTimer_, qOverload<>(&QTimer::start));
    connect(findBar_, &FindReplaceBar::searchOptionsChanged, this, [this] {
        if (auto* editor = currentEditor()) {
            if (!findBar_->inSelection()) {
                editor->clearSearchScope();
            } else if (!editor->hasSearchScope()) {
                editor->setSearchScopeToSelection();
            }
        }
        highlightTimer_->start();
    });
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this,
            &MainWindow::updateEditorActions);
    // Editing commands follow whichever editor view the user focused last.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget* old, QWidget* now) {
        // Entering or leaving an agent workspace changes who takes the editing commands.
        if (agentWorkspaceContaining(old) != nullptr || agentWorkspaceContaining(now) != nullptr)
            updateEditorActions();
        if (now == nullptr || splitPane_ == nullptr) {
            return;
        }
        if (now == splitPane_ || splitPane_->isAncestorOf(now)) {
            splitFocused_ = true;
        } else if (now == tabs_ || tabs_->isAncestorOf(now)) {
            splitFocused_ = false;
        } else {
            return;
        }
        updateEditorActions();
        updateDocumentState();
    });

    connect(&theme_, &ThemeManager::themeChanged, this, [this, productSurface] {
        activityBar_->applyPalette(theme_.palette(), theme_.interfaceFontSizePixels());
        productSurface->setStyleSheet(QStringLiteral("QWidget#productSurface { background: %1; }")
                                          .arg(theme_.palette().pageBackground));
        applyThemeToEditors();
        if (worktreesWorkspace_ != nullptr)
            worktreesWorkspace_->applyPalette(theme_.palette(), theme_.interfaceFontSizePixels());
        if (settingsWorkspace_ != nullptr)
            settingsWorkspace_->applyPalette(theme_.palette(), theme_.interfaceFontSizePixels());
    });
    connect(git_, &GitService::snapshotChanged, this, &MainWindow::updateGitSnapshot);
    connect(git_, &GitService::worktreesChanged, this, &MainWindow::updateWorktrees);
    connect(git_, &GitService::snapshotChanged, this, [this] { refreshWorktreesWorkspace(); });
    connect(git_, &GitService::worktreesChanged, this, [this] { refreshWorktreesWorkspace(); });
    connect(git_, &GitService::diffReady, this,
            [this](const QString& path, const GitDiffMode mode, const QString& diff,
                   const QString& error) {
                const bool fromAgent = agentDiffRequests_.remove(normalizedPath(path));
                if (!error.isEmpty()) {
                    // Routine (outside the repository, git stderr): no modal dialog.
                    statusBar()->showMessage(
                        QStringLiteral("Unable to show Git diff: %1").arg(error), 6000);
                } else if (diff.isEmpty() && fromAgent && QFileInfo(path).isFile()) {
                    openFile(path);
                } else if (diff.isEmpty()) {
                    statusBar()->showMessage(QStringLiteral("No changes for this file."), 3000);
                } else {
                    showDiff(path, mode, diff);
                }
            });
    connect(git_, &GitService::errorOccurred, this, [this](const QString& error) {
        refreshAgentWorktrees();
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 4000);
        }
    });
    connect(qApp, &QApplication::applicationStateChanged, this,
            [this](const Qt::ApplicationState state) {
                if (state == Qt::ApplicationActive) {
                    disk_change::refreshCleanTabs(tabs_);
                    git_->scheduleRefresh(0);
                    refreshAgentWorktrees();
                }
            });

    createNewDocument();
    updateEditorActions();
    if (qEnvironmentVariableIsSet("KETPLUS_OPEN_REMOTE_WORKSPACE")) {
        QTimer::singleShot(0, this, [this] { setRemoteWorkspaceVisible(true); });
    }
}

MainWindow::~MainWindow() {
    // Stop the file index and workspace search workers first: they post results back to
    // this window, so they must finish before anything else is torn down.
    cancelWorkspaceSearch();
    if (symbolIndexCancelled_ != nullptr) {
        symbolIndexCancelled_->store(true);
    }
    if (symbolIndexThread_ != nullptr) {
        symbolIndexThread_->wait();
    }
    if (workspaceIndexCancelled_ != nullptr) {
        workspaceIndexCancelled_->store(true);
    }
    if (workspaceIndexThread_ != nullptr) {
        workspaceIndexThread_->wait();
    }
    // Child services can still be probing Git when the window closes. Stop
    // them while the event application and their owners are alive; QObject's
    // later child teardown must not destroy a running process.
    for (auto* process : findChildren<QProcess*>()) {
        if (process->state() == QProcess::NotRunning)
            continue;
        process->terminate();
        if (!process->waitForFinished(250)) {
            process->kill();
            process->waitForFinished();
        }
    }
    repositoryLookupPool_.clear();
    repositoryLookupPool_.waitForDone();
}

void MainWindow::openFile(const QString& filePath) {
    if (atlasWorkspaceMode_ || worktreesWorkspaceMode_ || settingsWorkspaceMode_)
        setExplorerVisible(true);
    if (showAgentFile(filePath))
        return;
    const QString absolutePath = normalizedPath(filePath);
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* existing = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (existing != nullptr && !existing->document().isUntitled() &&
            normalizedPath(existing->document().filePath()) == absolutePath) {
            const auto refreshed = existing->refreshFromDisk();
            if (!refreshed.ok)
                QMessageBox::warning(this, QStringLiteral("Unable to refresh file"),
                                     refreshed.error);
            tabs_->setCurrentIndex(index);
            return;
        }
    }

    EditorWidget* initialEditor = nullptr;
    if (tabs_->count() == 1) {
        auto* candidate = qobject_cast<EditorWidget*>(tabs_->widget(0));
        if (candidate != nullptr && candidate->document().isUntitled() &&
            !candidate->document().isModified()) {
            initialEditor = candidate;
        }
    }

    auto* editor = createEditor();
    const auto result = editor->loadFile(absolutePath);
    if (!result.ok) {
        QMessageBox::critical(this, QStringLiteral("Unable to open file"), result.error);
        editor->deleteLater();
        return;
    }

    if (initialEditor != nullptr) {
        tabs_->removeTab(tabs_->indexOf(initialEditor));
        initialEditor->deleteLater();
    }

    editor->applyTheme(theme_.palette());
    const int index = tabs_->addTab(editor, editor->document().displayName());
    configureTabCloseButton(index, editor);
    tabs_->setCurrentIndex(index);
    rememberRecentFile(absolutePath);
    if (editor->isLargeFileMode()) {
        statusBar()->showMessage(
            QStringLiteral("Large file mode: syntax highlighting and undo are disabled"), 5000);
    }
}

void MainWindow::openFolder(const QString& folderPath) {
    const QFileInfo folder(folderPath);
    if (!folder.isDir()) {
        QMessageBox::critical(this, QStringLiteral("Unable to open folder"),
                              QStringLiteral("The selected path is not a folder."));
        return;
    }

    rememberActiveFileForWorktree();
    workspaceRoot_ = normalizedPath(folder.absoluteFilePath());
    fileContextRoot_.clear();
    const QString cachedRepositoryRoot = RepositoryRouter::cachedRepositoryRoot(workspaceRoot_);
    repositoryDisplayRoot_ = cachedRepositoryRoot.isEmpty() ? workspaceRoot_ : cachedRepositoryRoot;
    rememberRecentFolder(workspaceRoot_);
    scheduleSidebarRefresh(true);
    workspaceFilesIndexedAt_ = 0;
    const bool keepAgentWorkspace = agentWorkspaceMode_;
    auto* explorer = ensureExplorer();
    explorer->setRootPath(workspaceRoot_);
    if (keepAgentWorkspace) {
        ensureAgentWorkspace();
        explorer->hide();
    } else {
        setExplorerVisible(true);
    }
    syncAgentWorkspaceContext(keepAgentWorkspace);
    setWindowTitle(QStringLiteral("%1 — KétPlus Pro").arg(folder.fileName()));
    statusBar()->showMessage(QStringLiteral("Opened %1").arg(workspaceRoot_), 2500);
    git_->setWorkspacePath(workspaceRoot_);
    if (!suppressWorkspaceRestore_ && !restoringWorkspaceState_) {
        restoreWorkspaceState();
    }
    emit repositoryOpened(workspaceRoot_);
    refreshAtlasIndex();
}

void MainWindow::setShellMode(const ActivityBar::Mode mode) {
    if (compactConversationWindow_ && mode != ActivityBar::Mode::AgentWorkspace)
        setCompactConversationWindow(false);
    if (mode != ActivityBar::Mode::Search)
        hideSearchPanel();
    if (mode == ActivityBar::Mode::Settings) {
        if (worktreesWorkspace_ != nullptr)
            worktreesWorkspace_->hide();
        worktreesWorkspaceMode_ = false;
    } else if (!(mode == ActivityBar::Mode::AgentWorkspace && worktreesWorkspaceMode_)) {
        hideUtilityWorkspaces();
    }
    activityBar_->setMode(mode);
    activityBar_->show();
    // Search keeps the editor on screen, so its encoding/EOL/language controls stay reachable.
    const bool explorerStatus =
        mode == ActivityBar::Mode::Explorer || mode == ActivityBar::Mode::Search;
    statusBar()->setVisible(explorerStatus);
    if (explorerStatus)
        statusBar()->setContentsMargins(activityBar_->sidebarWidth(), 0, 0, 0);
}

bool MainWindow::restoreLastWorkspace() {
    const QString path = QSettings().value(QStringLiteral("workspace/lastRoot")).toString();
    if (path.isEmpty() || !QFileInfo(path).isDir()) {
        return false;
    }
    openFolder(path);
    return true;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (editor != nullptr && !maybeCloseEditor(editor)) {
            event->ignore();
            return;
        }
    }
    if (agentDetailTabs_ != nullptr) {
        for (int index = 0; index < agentDetailTabs_->count(); ++index) {
            auto* editor = qobject_cast<EditorWidget*>(agentDetailTabs_->widget(index));
            if (editor != nullptr && !maybeCloseEditor(editor)) {
                event->ignore();
                return;
            }
        }
    }
    saveWorkspaceState();
    event->accept();
}

} // namespace ketplus
