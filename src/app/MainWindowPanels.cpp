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
MarkdownPreviewPane* MainWindow::ensureMarkdownPreview() {
    if (markdownPreview_ != nullptr) {
        return markdownPreview_;
    }

    markdownPreview_ = new MarkdownPreviewPane(editorSplit_);
    editorSplit_->addWidget(markdownPreview_);
    editorSplit_->setStretchFactor(0, 1);
    editorSplit_->setStretchFactor(1, 1);
    connect(markdownPreview_, &MarkdownPreviewPane::closeRequested, this,
            [this] { setMarkdownPreviewVisible(false); });
    connect(markdownPreview_, &MarkdownPreviewPane::fileOpenRequested, this,
            &MainWindow::openFile);
    connect(markdownPreview_, &MarkdownPreviewPane::statusMessageRequested, this,
            [this](const QString& message) { statusBar()->showMessage(message, 7000); });
    return markdownPreview_;
}

void MainWindow::setMarkdownPreviewVisible(const bool visible) {
    if (visible) {
        auto* preview = ensureMarkdownPreview();
        preview->show();
        const int availableWidth = qMax(640, editorSplit_->width());
        editorSplit_->setSizes({availableWidth * 3 / 5, availableWidth * 2 / 5});
        const QSignalBlocker blocker(markdownPreviewAction_);
        markdownPreviewAction_->setChecked(true);
        updateMarkdownPreview();
        return;
    }

    if (markdownPreview_ != nullptr) {
        markdownPreview_->hide();
    }
    const QSignalBlocker blocker(markdownPreviewAction_);
    markdownPreviewAction_->setChecked(false);
    if (auto* editor = currentEditor()) {
        editor->setFocus();
    }
}

void MainWindow::updateMarkdownPreview() {
    if (markdownPreview_ == nullptr || !markdownPreview_->isVisible()) {
        return;
    }
    auto* editor = currentEditor();
    if (editor == nullptr || editor->syntaxName() != QStringLiteral("markdown")) {
        markdownPreview_->showEmpty(theme_.palette());
        return;
    }
    markdownPreview_->setSource(QString::fromUtf8(editor->text()),
                                editor->document().filePath(), theme_.palette());
}

TerminalPanel* MainWindow::ensureTerminal() {
    if (terminal_ != nullptr) {
        return terminal_;
    }

    terminal_ = new TerminalPanel(mainSplit_);
    terminal_->applyTheme(theme_.palette());
    mainSplit_->addWidget(terminal_);
    mainSplit_->setStretchFactor(mainSplit_->indexOf(workspaceSplit_), 1);
    mainSplit_->setStretchFactor(mainSplit_->indexOf(terminal_), 0);
    connect(terminal_, &TerminalPanel::closeRequested, this,
            [this] { setTerminalVisible(false); });
    connect(terminal_, &TerminalPanel::statusMessageRequested, this,
            [this](const QString& message) { statusBar()->showMessage(message, 5000); });
    return terminal_;
}

void MainWindow::setTerminalVisible(const bool visible) {
    if (visible) {
        auto* terminal = ensureTerminal();
        terminal->show();
        const int availableHeight = qMax(420, mainSplit_->height());
        const int terminalHeight = qBound(160, availableHeight / 3, 300);
        mainSplit_->setSizes({availableHeight - terminalHeight, terminalHeight});
        terminal->start(workspaceRoot_.isEmpty() ? QDir::homePath() : workspaceRoot_);
        terminal->focusTerminal();
        const QSignalBlocker blocker(terminalAction_);
        terminalAction_->setChecked(true);
        return;
    }

    if (terminal_ != nullptr) {
        terminal_->hide();
    }
    const QSignalBlocker blocker(terminalAction_);
    terminalAction_->setChecked(false);
    if (auto* editor = currentEditor()) {
        editor->setFocus();
    }
}

void MainWindow::focusTerminal() {
    setTerminalVisible(true);
}

ExplorerPanel* MainWindow::ensureExplorer() {
    if (explorer_ != nullptr) {
        return explorer_;
    }

    explorer_ = new ExplorerPanel(workspaceSplit_);
    workspaceSplit_->insertWidget(0, explorer_);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(explorer_), 0);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(editorSplit_), 1);

    connect(explorer_, &ExplorerPanel::fileOpenRequested, this, &MainWindow::openFile);
    connect(explorer_, &ExplorerPanel::gitDiffRequested, this,
            [this](const QString& path) { requestFileDiff(path); });
    connect(explorer_, &ExplorerPanel::openFolderRequested, this, &MainWindow::chooseFolder);
    connect(explorer_, &ExplorerPanel::hideRequested, this, [this] { setExplorerVisible(false); });

    if (!workspaceRoot_.isEmpty()) {
        explorer_->setRootPath(workspaceRoot_);
    }
    return explorer_;
}

void MainWindow::setExplorerVisible(const bool visible) {
    if (visible) {
        auto* explorer = ensureExplorer();
        if (gitChanges_ != nullptr) {
            gitChanges_->hide();
        }
        explorer->show();
        QList<int> sizes(workspaceSplit_->count(), 0);
        sizes[workspaceSplit_->indexOf(explorer)] = 260;
        sizes[workspaceSplit_->indexOf(editorSplit_)] =
            qMax(420, workspaceSplit_->width() - 260);
        workspaceSplit_->setSizes(sizes);
        const QSignalBlocker explorerBlocker(explorerAction_);
        const QSignalBlocker sourceControlBlocker(sourceControlAction_);
        explorerAction_->setChecked(true);
        sourceControlAction_->setChecked(false);
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

void MainWindow::focusExplorer() {
    setExplorerVisible(true);
}

GitChangesPanel* MainWindow::ensureGitChanges() {
    if (gitChanges_ != nullptr) {
        return gitChanges_;
    }

    gitChanges_ = new GitChangesPanel(workspaceSplit_);
    workspaceSplit_->insertWidget(0, gitChanges_);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(gitChanges_), 0);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(editorSplit_), 1);
    connect(gitChanges_, &GitChangesPanel::diffRequested, this,
            [this](const QString& path, const GitDiffMode mode) {
                requestFileDiff(path, mode);
            });
    connect(gitChanges_, &GitChangesPanel::fileOpenRequested, this, &MainWindow::openFile);
    connect(gitChanges_, &GitChangesPanel::refreshRequested, git_, &GitService::refresh);
    connect(gitChanges_, &GitChangesPanel::hideRequested, this,
            [this] { setSourceControlVisible(false); });
    gitChanges_->setSnapshot(gitSnapshot_);
    return gitChanges_;
}

void MainWindow::setSourceControlVisible(const bool visible) {
    if (visible) {
        auto* changes = ensureGitChanges();
        if (explorer_ != nullptr) {
            explorer_->hide();
        }
        changes->show();
        QList<int> sizes(workspaceSplit_->count(), 0);
        sizes[workspaceSplit_->indexOf(changes)] = 280;
        sizes[workspaceSplit_->indexOf(editorSplit_)] =
            qMax(420, workspaceSplit_->width() - 280);
        workspaceSplit_->setSizes(sizes);
        const QSignalBlocker explorerBlocker(explorerAction_);
        const QSignalBlocker sourceControlBlocker(sourceControlAction_);
        explorerAction_->setChecked(false);
        sourceControlAction_->setChecked(true);
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

} // namespace ketplus
