#include "app/MainWindow.h"

#include "app/SettingsDialog.h"
#include "editor/EditorWidget.h"
#include "git/GitChangesPanel.h"
#include "git/GitDiffView.h"
#include "git/GitService.h"
#include "preview/MarkdownPreviewPane.h"
#include "terminal/TerminalPanel.h"
#include "ui/FindReplaceBar.h"
#include "ui/Theme.h"
#include "workspace/ExplorerPanel.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
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
#include <QToolButton>
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
} // namespace

void MainWindow::requestCurrentFileDiff() {
    const auto* editor = currentEditor();
    if (editor == nullptr || editor->document().isUntitled()) {
        statusBar()->showMessage(QStringLiteral("Open a tracked file to view its diff."), 3000);
        return;
    }
    requestFileDiff(editor->document().filePath());
}

void MainWindow::requestFileDiff(const QString& filePath, const GitDiffMode mode) {
    statusBar()->showMessage(QStringLiteral("Loading Git diff…"));
    git_->requestDiff(filePath, mode);
}

void MainWindow::showDiff(const QString& filePath, const GitDiffMode mode, const QString& diff) {
    const QString absolutePath = normalizedPath(filePath);
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
    rebuildWorktreeMenu();
    updateGitActions();
}

void MainWindow::rebuildWorktreeMenu() {
    worktreeMenu_->clear();
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
        if (!worktree.lockReason.isEmpty()) {
            name.append(QStringLiteral(" (locked)"));
        }

        auto* action = worktreeMenu_->addAction(name);
        action->setCheckable(true);
        action->setChecked(worktree.current);
        action->setEnabled(!action->isChecked() && !worktree.bare && !worktree.prunable);
        action->setToolTip(worktree.path);
        connect(action, &QAction::triggered, this,
                [this, path = worktree.path] { switchToWorktree(path); });
    }
}

void MainWindow::updateGitActions() {
    const bool repositoryOpen = gitSnapshot_.isRepository();
    gitMenu_->setEnabled(repositoryOpen);
    sourceControlAction_->setEnabled(repositoryOpen);
    viewDiffAction_->setEnabled(repositoryOpen && !currentWorkspaceRelativeFile().isEmpty());
    worktreeMenu_->setEnabled(repositoryOpen && worktrees_.size() > 1);
}

void MainWindow::switchToWorktree(const QString& path) {
    const QString targetRoot = normalizedPath(path);
    if (targetRoot == normalizedPath(workspaceRoot_)) {
        return;
    }

    const QString carriedRelativePath = currentWorkspaceRelativeFile();
    rememberActiveFileForWorktree();
    QString targetRelativePath = activeFileByWorktree_.value(targetRoot);
    if (targetRelativePath.isEmpty()) {
        targetRelativePath = carriedRelativePath;
    }

    openFolder(targetRoot);
    if (!targetRelativePath.isEmpty()) {
        const QString targetFile = QDir(targetRoot).filePath(targetRelativePath);
        if (QFileInfo(targetFile).isFile()) {
            openFile(targetFile);
        }
    }
    statusBar()->showMessage(QStringLiteral("Switched worktree to %1").arg(targetRoot), 3000);
}

void MainWindow::rememberActiveFileForWorktree() {
    const QString relativePath = currentWorkspaceRelativeFile();
    if (!relativePath.isEmpty() && !workspaceRoot_.isEmpty()) {
        activeFileByWorktree_.insert(normalizedPath(workspaceRoot_), relativePath);
    }
}

QString MainWindow::currentWorkspaceRelativeFile() const {
    const auto* editor = currentEditor();
    if (editor == nullptr || editor->document().isUntitled()) {
        return {};
    }
    return relativePathWithin(editor->document().filePath(), workspaceRoot_);
}
} // namespace ketplus
