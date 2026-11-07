#include "app/MainWindow.h"
#include "app/RepositoryRouter.h"
#include "app/SpotlightDialog.h"
#include "atlas/AtlasTab.h"
#include "workspace/AtlasIndex.h"

#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexRateLimits.h"
#include "app/SettingsDialog.h"
#include "editor/EditorWidget.h"
#include "editor/TabPaths.h"
#include "git/GitChangesPanel.h"
#include "git/GitDiffView.h"
#include "git/GitService.h"
#include "preview/MarkdownPreviewPane.h"
#include "terminal/TerminalPanel.h"
#include "ui/FindReplaceBar.h"
#include "ui/Theme.h"
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
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
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

QString relativePathWithin(const QString& path, const QString& root) {
    if (path.isEmpty() || root.isEmpty()) {
        return {};
    }
    const QString relative = QDir(root).relativeFilePath(path);
    if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) ||
        QDir::isAbsolutePath(relative)) {
        return {};
    }
    return relative;
}

QString settingsDigestKey(const QString& prefix, const QString& path) {
    const QByteArray digest =
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("%1/%2").arg(prefix, QString::fromLatin1(digest));
}

} // namespace

ExplorerPanel* MainWindow::ensureExplorer() {
    if (explorer_ != nullptr) {
        return explorer_;
    }

    explorer_ = new ExplorerPanel(workspaceSplit_);
    explorer_->setAttribute(Qt::WA_StyledBackground, true);
    workspaceSplit_->insertWidget(0, explorer_);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(explorer_), 0);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(editorSplit_), 1);

    connect(explorer_, &ExplorerPanel::fileOpenRequested, this, &MainWindow::openFile);
    connect(explorer_, &ExplorerPanel::gitDiffRequested, this,
            [this](const QString& path) { requestFileDiff(path); });
    connect(explorer_, &ExplorerPanel::openFolderRequested, this, &MainWindow::chooseFolder);
    connect(explorer_, &ExplorerPanel::hideRequested, this, [this] { setExplorerVisible(false); });
    connect(explorer_, &ExplorerPanel::pathRenamed, this,
            [this](const QString& from, const QString& to) {
                for (EditorWidget* editor : tab_paths::retarget(tabs_, from, to)) {
                    editor->configureLexerForPath(editor->document().filePath());
                    editor->applyTheme(theme_.palette());
                    updateTabTitle(editor);
                }
            });
    // Clean tabs of a trashed file close; tabs with edits stay, as the confirmation said.
    connect(explorer_, &ExplorerPanel::pathTrashed, this, [this](const QString& path) {
        for (EditorWidget* editor : tab_paths::editorsWithin(tabs_, path)) {
            if (!editor->document().isModified()) {
                closeTab(tabs_->indexOf(editor));
            }
        }
    });
    explorer_->setUnsavedTabsQuery([this](const QString& path) {
        const auto editors = tab_paths::editorsWithin(tabs_, path);
        return static_cast<int>(std::count_if(editors.cbegin(), editors.cend(), [](auto* editor) {
            return editor->document().isModified();
        }));
    });

    if (!workspaceRoot_.isEmpty()) {
        explorer_->setRootPath(fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_);
    }
    return explorer_;
}

void MainWindow::setExplorerVisible(const bool visible) {
    if (visible) {
        hideUtilityWorkspaces();
        setRemoteWorkspaceVisible(false);
        setAtlasWorkspaceVisible(false);
        setAgentWorkspaceVisible(false);
        auto* explorer = ensureExplorer();
        if (gitChanges_ != nullptr) {
            gitChanges_->hide();
        }
        if (searchPanel_ != nullptr) {
            searchPanel_->hide();
            const QSignalBlocker searchBlocker(searchPanelAction_);
            searchPanelAction_->setChecked(false);
        }
        followConversationCheckout();
        explorer->show();
        editorSplit_->show();
        workspaceSplit_->show();
        mainSplit_->show();
        QList<int> sizes(workspaceSplit_->count(), 0);
        constexpr int navigationWidth = 224;
        sizes[workspaceSplit_->indexOf(explorer)] = navigationWidth;
        sizes[workspaceSplit_->indexOf(editorSplit_)] =
            qMax(420, workspaceSplit_->width() - navigationWidth);
        workspaceSplit_->setSizes(sizes);
        const QSignalBlocker explorerBlocker(explorerAction_);
        const QSignalBlocker sourceControlBlocker(sourceControlAction_);
        explorerAction_->setChecked(true);
        sourceControlAction_->setChecked(false);
        setShellMode(ActivityBar::Mode::Explorer);
        explorer->focusTree();
    } else if (explorer_ != nullptr) {
        explorer_->hide();
        const QSignalBlocker blocker(explorerAction_);
        explorerAction_->setChecked(false);
        if (auto* editor = currentEditor()) {
            editor->setFocus();
        }
    }
}

void MainWindow::focusExplorer() { setExplorerVisible(true); }

GitChangesPanel* MainWindow::ensureGitChanges() {
    if (gitChanges_ != nullptr) {
        return gitChanges_;
    }

    gitChanges_ = new GitChangesPanel(workspaceSplit_);
    gitChanges_->setAttribute(Qt::WA_StyledBackground, true);
    workspaceSplit_->insertWidget(0, gitChanges_);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(gitChanges_), 0);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(editorSplit_), 1);
    connect(gitChanges_, &GitChangesPanel::diffRequested, this,
            [this](const QString& path, const GitDiffMode mode) { requestFileDiff(path, mode); });
    connect(gitChanges_, &GitChangesPanel::fileOpenRequested, this, &MainWindow::openFile);
    connect(gitChanges_, &GitChangesPanel::refreshRequested, git_, &GitService::refresh);
    connect(gitChanges_, &GitChangesPanel::stageRequested, git_, &GitService::stageFile);
    connect(gitChanges_, &GitChangesPanel::unstageRequested, git_, &GitService::unstageFile);
    connect(gitChanges_, &GitChangesPanel::commitRequested, git_, &GitService::commitStaged);
    connect(git_, &GitService::mutationRunningChanged, gitChanges_,
            &GitChangesPanel::setMutationRunning);
    connect(git_, &GitService::mutationFinished, gitChanges_, &GitChangesPanel::mutationFinished);
    gitChanges_->setMutationRunning(git_->mutationRunning());
    connect(gitChanges_, &GitChangesPanel::hideRequested, this,
            [this] { setSourceControlVisible(false); });
    gitChanges_->setSnapshot(gitSnapshot_);
    return gitChanges_;
}

void MainWindow::setSourceControlVisible(const bool visible) {
    if (visible) {
        hideUtilityWorkspaces();
        setRemoteWorkspaceVisible(false);
        setAtlasWorkspaceVisible(false);
        setAgentWorkspaceVisible(false);
        auto* changes = ensureGitChanges();
        if (explorer_ != nullptr) {
            explorer_->hide();
        }
        if (searchPanel_ != nullptr) {
            searchPanel_->hide();
            const QSignalBlocker searchBlocker(searchPanelAction_);
            searchPanelAction_->setChecked(false);
        }
        followConversationCheckout();
        changes->show();
        editorSplit_->show();
        workspaceSplit_->show();
        mainSplit_->show();
        QList<int> sizes(workspaceSplit_->count(), 0);
        constexpr int navigationWidth = 246;
        sizes[workspaceSplit_->indexOf(changes)] = navigationWidth;
        sizes[workspaceSplit_->indexOf(editorSplit_)] =
            qMax(420, workspaceSplit_->width() - navigationWidth);
        workspaceSplit_->setSizes(sizes);
        const QSignalBlocker explorerBlocker(explorerAction_);
        const QSignalBlocker sourceControlBlocker(sourceControlAction_);
        explorerAction_->setChecked(false);
        sourceControlAction_->setChecked(true);
        setShellMode(ActivityBar::Mode::SourceControl);
        changes->focusChanges();
        git_->scheduleRefresh(0);
    } else if (gitChanges_ != nullptr) {
        gitChanges_->hide();
        const QSignalBlocker blocker(sourceControlAction_);
        sourceControlAction_->setChecked(false);
        if (auto* editor = currentEditor()) {
            editor->setFocus();
        }
    }
}

void MainWindow::requestCurrentFileDiff() {
    // The split view shares its source tab's document but has no file path of its own.
    const auto* editor = documentOwner();
    if (editor == nullptr || editor->document().isUntitled()) {
        statusBar()->showMessage(QStringLiteral("Open a tracked file to view its diff."), 3000);
        return;
    }
    requestFileDiff(editor->document().filePath());
}

void MainWindow::openFileDiff(const QString& filePath) { requestFileDiff(filePath); }

void MainWindow::openTranscriptFile(const QString& path, bool diff) {
    pendingTranscriptDiff_.clear();
    // Use the nearest checkout, including linked worktrees whose .git is a file.
    QDir folder(QFileInfo(path).absolutePath());
    QString checkout;
    do {
        if (QFileInfo::exists(folder.filePath(".git"))) {
            checkout = normalizedPath(folder.absolutePath());
            break;
        }
    } while (folder.cdUp());
    if (checkout.isEmpty()) {
        openFile(path);
        return;
    }
    const QString current =
        normalizedPath(fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_);
    if (diff) {
        pendingTranscriptDiff_ = path;
        pendingTranscriptDiffRoot_ = checkout;
    }
    if (current != checkout)
        setFileContextWorktree(checkout);
    if (!diff) {
        openFile(path);
    } else if (normalizedPath(git_->snapshot().repositoryRoot) == checkout) {
        pendingTranscriptDiff_.clear();
        requestFileDiff(path);
    }
    // Otherwise updateGitSnapshot opens the diff after asynchronous Git discovery.
}

void MainWindow::requestFileDiff(const QString& filePath, const GitDiffMode mode) {
    statusBar()->showMessage(QStringLiteral("Loading Git diff…"));
    // GitService answers only the newest request, so only it can be an agent's.
    agentDiffRequests_.clear();
    if (agentWorkspaceMode_)
        agentDiffRequests_.insert(normalizedPath(filePath));
    git_->requestDiff(filePath, mode);
}

void MainWindow::showDiff(const QString& filePath, const GitDiffMode mode, const QString& diff) {
    const QString absolutePath = normalizedPath(filePath);
    if (showAgentDiff(absolutePath, mode, diff)) {
        return;
    }
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* existing = qobject_cast<GitDiffView*>(tabs_->widget(index));
        if (existing != nullptr && normalizedPath(existing->filePath()) == absolutePath &&
            existing->mode() == mode) {
            existing->setDiff(absolutePath, mode, diff, theme_.palette());
            tabs_->setCurrentIndex(index);
            statusBar()->showMessage(QStringLiteral("Updated Git diff"), 2000);
            return;
        }
    }

    auto* diffView = new GitDiffView(this);
    diffView->applyEditorSettings(appearanceSettings_.editor);
    diffView->setDiff(absolutePath, mode, diff, theme_.palette());
    connect(diffView, &GitDiffView::openFileRequested, this, &MainWindow::openFile);
    connect(diffView, &GitDiffView::refreshRequested, this,
            [this](const QString& path, const GitDiffMode requestedMode) {
                requestFileDiff(path, requestedMode);
            });

    const QString displayName = QStringLiteral("%1 — Diff").arg(QFileInfo(absolutePath).fileName());
    const int index = tabs_->addTab(diffView, displayName);
    tabs_->setTabToolTip(index, absolutePath);
    configureTabCloseButton(index, diffView, displayName);
    tabs_->setCurrentIndex(index);
    statusBar()->showMessage(QStringLiteral("Opened Git diff"), 2000);
}

void MainWindow::updateGitSnapshot(const GitSnapshot& snapshot) {
    gitSnapshot_ = snapshot;
    if (explorer_) {
        QHash<QString, QString> statuses;
        for (const auto& file : snapshot.files)
            statuses.insert(QDir(snapshot.repositoryRoot).filePath(file.path),
                            file.untracked ? QStringLiteral("U")
                                           : QString(file.hasUnstagedChange() ? file.worktreeCode
                                                                              : file.indexCode));
        explorer_->setFileStatuses(statuses);
    }
    if (!pendingTranscriptDiff_.isEmpty() && snapshot.isRepository() &&
        normalizedPath(snapshot.repositoryRoot) == pendingTranscriptDiffRoot_ &&
        normalizedPath(snapshot.repositoryRoot) ==
            normalizedPath(fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_)) {
        const auto path = pendingTranscriptDiff_;
        pendingTranscriptDiff_.clear();
        requestFileDiff(path);
    }
    if (repositoryDisplayRoot_.isEmpty())
        repositoryDisplayRoot_ = workspaceRoot_;
    scheduleSidebarRefresh();
    activityBar_->setSourceControlCount(snapshot.files.size());
    atlasIndex_->scheduleRefresh();
    if (gitChanges_ != nullptr) {
        gitChanges_->setSnapshot(snapshot);
    }
    if (!snapshot.isRepository()) {
        gitButton_->hide();
        updateGitActions();
        return;
    }

    QString branch = snapshot.branch;
    if (branch.isEmpty()) {
        branch = snapshot.head.left(8);
    }
    QString text = QStringLiteral("Git: %1").arg(branch);
    if (!snapshot.files.isEmpty()) {
        text.append(QStringLiteral(" • %1").arg(snapshot.files.size()));
    }
    if (snapshot.ahead > 0) {
        text.append(QStringLiteral(" ↑%1").arg(snapshot.ahead));
    }
    if (snapshot.behind > 0) {
        text.append(QStringLiteral(" ↓%1").arg(snapshot.behind));
    }
    gitButton_->setText(text);
    gitButton_->setToolTip(QStringLiteral("%1\n%2 changed file(s)")
                               .arg(snapshot.repositoryRoot)
                               .arg(snapshot.files.size()));
    gitButton_->show();
    updateGitActions();
}

void MainWindow::updateWorktrees(const QVector<GitWorktree>& worktrees) {
    worktrees_ = worktrees;
    QString primaryWorktree;
    for (const auto& worktree : worktrees_) {
        const QString gitMetadata = QDir(worktree.path).filePath(QStringLiteral(".git"));
        if (worktree.bare || worktree.prunable || !QFileInfo(worktree.path).isDir() ||
            !QFileInfo(gitMetadata).exists())
            continue;
        const QString path = normalizedPath(worktree.path);
        if (primaryWorktree.isEmpty() || QFileInfo(gitMetadata).isDir())
            primaryWorktree = path;
    }
    if (!primaryWorktree.isEmpty()) {
        repositoryDisplayRoot_ = primaryWorktree;
        // Remember every worktree's repository so its conversations stay grouped after switching.
        for (const auto& worktree : std::as_const(worktrees_))
            sidebarRepositoryByPath_.insert(normalizedPath(worktree.path),
                                            normalizedPath(primaryWorktree));
    }
    // Linked worktree paths now resolve to this repository's conversation group.
    scheduleSidebarRefresh();
    rebuildWorktreeMenu();
    refreshAgentWorktrees();
    updateGitActions();
}

void MainWindow::prioritizeWorktree(QVector<GitWorktree>& worktrees) const {
    const QString promoted = normalizedPath(promotedWorktreePath_);
    if (promoted.isEmpty())
        return;
    const auto selected = std::find_if(worktrees.cbegin(), worktrees.cend(),
                                       [&promoted](const GitWorktree& worktree) {
                                           return normalizedPath(worktree.path) == promoted;
                                       });
    if (selected == worktrees.cend() || selected == worktrees.cbegin())
        return;
    const qsizetype index = std::distance(worktrees.cbegin(), selected);
    worktrees.prepend(worktrees.takeAt(index));
}

void MainWindow::rebuildWorktreeMenu() {
    worktreeMenu_->clear();
    worktreeMenu_->addAction(manageWorktreesAction_);
    worktreeMenu_->addSeparator();
    if (worktrees_.isEmpty()) {
        auto* emptyAction = worktreeMenu_->addAction(QStringLiteral("No linked worktrees"));
        emptyAction->setEnabled(false);
        return;
    }

    for (const auto& worktree : worktrees_) {
        QString name = worktree.branch;
        if (name.isEmpty()) {
            name = QStringLiteral("Detached %1").arg(worktree.head.left(8));
        }
        const QString directoryName = QFileInfo(worktree.path).fileName();
        if (!directoryName.isEmpty() && directoryName != name) {
            name.append(QStringLiteral(" — %1").arg(directoryName));
        }
        if (worktree.locked) {
            name.append(QStringLiteral(" (locked)"));
        }

        auto* action = worktreeMenu_->addAction(name);
        action->setCheckable(true);
        action->setChecked(worktree.current);
        action->setEnabled(!action->isChecked() && !worktree.bare && !worktree.prunable);
        action->setToolTip(worktree.path);
        connect(action, &QAction::triggered, this,
                [this, path = worktree.path] { setFileContextWorktree(path); });
    }
}

void MainWindow::updateGitActions() {
    const bool repositoryOpen = gitSnapshot_.isRepository();
    gitMenu_->setEnabled(repositoryOpen);
    sourceControlAction_->setEnabled(repositoryOpen);
    activityBar_->setSourceControlAvailable(repositoryOpen);
    viewDiffAction_->setEnabled(repositoryOpen && !currentWorkspaceRelativeFile().isEmpty());
    worktreeMenu_->setEnabled(repositoryOpen);
}

void MainWindow::switchToWorktree(const QString& path) {
    const QString targetRoot = normalizedPath(path);
    if (targetRoot == normalizedPath(workspaceRoot_)) {
        syncAgentWorkspaceContext(agentWorkspaceMode_);
        return;
    }
    if (!QFileInfo(targetRoot).isDir() ||
        !QFileInfo(QDir(targetRoot).filePath(QStringLiteral(".git"))).exists()) {
        statusBar()->showMessage(tr("This worktree is unavailable and was removed from the list."),
                                 5000);
        git_->scheduleRefresh(0);
        return;
    }

    const QString repositoryRoot = repositoryDisplayRoot_;
    const QString carriedRelativePath = currentWorkspaceRelativeFile();
    EditorWidget* carriedEditor = documentOwner();
    // The split shows a file from the current worktree; close it before switching.
    closeSplit();
    rememberActiveFileForWorktree();
    QString targetRelativePath = activeFileByWorktree_.value(targetRoot);
    if (targetRelativePath.isEmpty()) {
        targetRelativePath = carriedRelativePath;
    }

    suppressWorkspaceRestore_ = true;
    openFolder(targetRoot);
    suppressWorkspaceRestore_ = false;
    if (!repositoryRoot.isEmpty()) {
        repositoryDisplayRoot_ = repositoryRoot;
        scheduleSidebarRefresh();
    }
    if (!targetRelativePath.isEmpty()) {
        const QString targetFile = QDir(targetRoot).filePath(targetRelativePath);
        if (QFileInfo(targetFile).isFile()) {
            openFile(targetFile);
            if (carriedEditor != nullptr && !carriedEditor->document().isModified()) {
                const int carriedIndex = tabs_->indexOf(carriedEditor);
                if (carriedIndex >= 0 && normalizedPath(carriedEditor->document().filePath()) !=
                                             normalizedPath(targetFile)) {
                    closeTab(carriedIndex, false);
                }
            }
        }
    }
    statusBar()->showMessage(QStringLiteral("Switched worktree to %1").arg(targetRoot), 3000);
}

void MainWindow::setFileContextWorktree(const QString& path) {
    const QString target = normalizedPath(path);
    if (!QFileInfo(target).isDir() || !QFileInfo(QDir(target).filePath(".git")).exists()) {
        statusBar()->showMessage(tr("This worktree is unavailable."), 5000);
        return;
    }
    if (target != pendingTranscriptDiffRoot_)
        pendingTranscriptDiff_.clear();
    fileContextRoot_ = target;
    workspaceFilesIndexedAt_ = 0;
    ensureExplorer()->setRootPath(target);
    git_->setWorkspacePath(target);
    statusBar()->showMessage(tr("Files and Source Control: %1").arg(target), 5000);
}

// A conversation whose worktree was deleted stays open as history while the window keeps
// its own folder. Files and Source Control then show the checkout its agent last worked
// in, or say which folder they show instead.
void MainWindow::followConversationCheckout() {
    if (agentWorkspace_ == nullptr || agentManager_ == nullptr)
        return;
    const QString path = agentWorkspace_->activePath();
    if (path.isEmpty() || QFileInfo(path).isDir())
        return;
    const QString current =
        normalizedPath(fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_);
    const QString checkout = agentManager_->recentWorkingCheckout(path);
    if (checkout.isEmpty()) {
        statusBar()->showMessage(
            tr("This conversation's worktree was deleted. Showing %1.").arg(current), 8000);
        return;
    }
    if (checkout != current)
        setFileContextWorktree(checkout);
}

void MainWindow::rememberActiveFileForWorktree() {
    const QString relativePath = currentWorkspaceRelativeFile();
    if (!relativePath.isEmpty() && !workspaceRoot_.isEmpty()) {
        activeFileByWorktree_.insert(
            normalizedPath(fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_),
            relativePath);
    }
}

QString MainWindow::currentWorkspaceRelativeFile() const {
    const auto* editor = documentOwner();
    if (editor == nullptr || editor->document().isUntitled()) {
        return {};
    }
    return relativePathWithin(editor->document().filePath(),
                              fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_);
}

QString MainWindow::workspaceStateKey() const {
    const QString keyPath =
        gitSnapshot_.isRepository() ? gitSnapshot_.repositoryRoot : workspaceRoot_;
    return keyPath.isEmpty() ? QString{}
                             : settingsDigestKey(QStringLiteral("workspace/state"), keyPath);
}

void MainWindow::saveWorkspaceState() const {
    if (workspaceRoot_.isEmpty()) {
        return;
    }
    QString mode = QStringLiteral("editor");
    if (atlasWorkspaceMode_) {
        mode = QStringLiteral("atlas");
    } else if (agentWorkspaceMode_) {
        mode = QStringLiteral("agent");
    } else if (gitChanges_ != nullptr && gitChanges_->isVisible()) {
        mode = QStringLiteral("source");
    } else if (searchPanel_ != nullptr && searchPanel_->isVisible()) {
        mode = QStringLiteral("search");
    } else if (explorer_ != nullptr && explorer_->isVisible()) {
        mode = QStringLiteral("explorer");
    }

    QString activeFile = currentWorkspaceRelativeFile();
    if (activeFile.isEmpty()) {
        activeFile = activeFileByWorktree_.value(
            normalizedPath(fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_));
    }

    QVariantMap state{
        {QStringLiteral("workspaceRoot"), workspaceRoot_},
        {QStringLiteral("worktreePath"), workspaceRoot_},
        {QStringLiteral("fileContextRoot"), fileContextRoot_},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("activeFile"), activeFile},
        {QStringLiteral("terminalVisible"), terminal_ != nullptr && terminal_->isVisible()},
        {QStringLiteral("documentMarkdownPreviewVisible"),
         currentEditor() != nullptr && currentEditor()->isMarkdownPreviewVisible()}};
    if (agentManager_ != nullptr) {
        state.insert(QStringLiteral("activeConversationId"),
                     agentManager_->activeConversationId(workspaceRoot_));
    }

    QSettings settings;
    settings.setValue(QStringLiteral("workspace/lastRoot"), workspaceRoot_);
    const QString primaryKey = workspaceStateKey();
    if (!primaryKey.isEmpty()) {
        settings.setValue(primaryKey, state);
    }
    settings.setValue(settingsDigestKey(QStringLiteral("workspace/state"), workspaceRoot_), state);
}

void MainWindow::restoreWorkspaceState() {
    if (workspaceRoot_.isEmpty()) {
        return;
    }
    QSettings settings;
    QVariantMap state =
        settings.value(settingsDigestKey(QStringLiteral("workspace/state"), workspaceRoot_))
            .toMap();
    if (state.isEmpty() && !workspaceStateKey().isEmpty()) {
        state = settings.value(workspaceStateKey()).toMap();
    }
    if (state.isEmpty()) {
        return;
    }

    const QString savedWorktree =
        normalizedPath(state.value(QStringLiteral("worktreePath")).toString());
    if (!savedWorktree.isEmpty() && savedWorktree != normalizedPath(workspaceRoot_) &&
        QFileInfo(savedWorktree).isDir()) {
        restoringWorkspaceState_ = true;
        openFolder(savedWorktree);
        restoringWorkspaceState_ = false;
        restoreWorkspaceState();
        return;
    }

    const auto savedFilesRoot = state.value(QStringLiteral("fileContextRoot")).toString();
    if (!savedFilesRoot.isEmpty())
        setFileContextWorktree(savedFilesRoot);
    const QString mode = state.value(QStringLiteral("mode"), QStringLiteral("explorer")).toString();
    if (mode == QStringLiteral("atlas")) {
        setAtlasWorkspaceVisible(true);
    } else if (mode == QStringLiteral("agent")) {
        setAgentWorkspaceVisible(true);
        const QString conversationId =
            state.value(QStringLiteral("activeConversationId")).toString();
        if (agentManager_ != nullptr && !conversationId.isEmpty()) {
            agentManager_->selectConversation(workspaceRoot_, conversationId);
        }
    } else if (mode == QStringLiteral("source")) {
        setSourceControlVisible(true);
    } else if (mode == QStringLiteral("search")) {
        setSearchPanelVisible(true);
    } else if (mode == QStringLiteral("explorer")) {
        setExplorerVisible(true);
    } else {
        setAtlasWorkspaceVisible(false);
        setAgentWorkspaceVisible(false);
        if (explorer_ != nullptr) {
            explorer_->hide();
        }
        if (gitChanges_ != nullptr) {
            gitChanges_->hide();
        }
        setShellMode(ActivityBar::Mode::Explorer);
    }
    const QString activeFile = state.value(QStringLiteral("activeFile")).toString();
    if (!activeFile.isEmpty()) {
        const QString path = QDir(fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_)
                                 .filePath(activeFile);
        if (QFileInfo(path).isFile()) {
            openFile(path);
        }
    }
    if (state.contains(QStringLiteral("documentMarkdownPreviewVisible")))
        setMarkdownPreviewVisible(
            state.value(QStringLiteral("documentMarkdownPreviewVisible")).toBool());
    setTerminalVisible(state.value(QStringLiteral("terminalVisible")).toBool());
    if (mode == QStringLiteral("atlas"))
        setAtlasWorkspaceVisible(true);
}

} // namespace ketplus
