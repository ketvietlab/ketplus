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
void MainWindow::createActions() {
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));

    auto* newAction = fileMenu->addAction(QStringLiteral("&New Tab"));
    newAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    connect(newAction, &QAction::triggered, this, &MainWindow::createNewDocument);

    auto* openAction = fileMenu->addAction(QStringLiteral("&Open…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openDocument);

    auto* openFolderAction = fileMenu->addAction(QStringLiteral("Open &Folder…"));
    openFolderAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+O")));
    connect(openFolderAction, &QAction::triggered, this, &MainWindow::chooseFolder);

    recentFoldersMenu_ = fileMenu->addMenu(QStringLiteral("Open Recent Folder"));
    connect(recentFoldersMenu_, &QMenu::aboutToShow, this,
            &MainWindow::rebuildRecentFoldersMenu);

    auto* closeTabAction = fileMenu->addAction(QStringLiteral("&Close Tab"));
    closeTabAction->setShortcut(QKeySequence::Close);
    connect(closeTabAction, &QAction::triggered, this, &MainWindow::closeCurrentTab);

    fileMenu->addSeparator();

    auto* saveAction = fileMenu->addAction(QStringLiteral("&Save"));
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &MainWindow::saveCurrentDocument);

    auto* saveAsAction = fileMenu->addAction(QStringLiteral("Save &As…"));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveCurrentDocumentAs);

    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(QStringLiteral("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    undoAction_ = editMenu->addAction(QStringLiteral("&Undo"));
    undoAction_->setShortcut(QKeySequence::Undo);
    connect(undoAction_, &QAction::triggered, this, [this] {
        if (auto* editor = currentEditor()) {
            editor->undoEdit();
        }
    });

    redoAction_ = editMenu->addAction(QStringLiteral("&Redo"));
    redoAction_->setShortcut(QKeySequence::Redo);
    connect(redoAction_, &QAction::triggered, this, [this] {
        if (auto* editor = currentEditor()) {
            editor->redoEdit();
        }
    });

    editMenu->addSeparator();
    cutAction_ = editMenu->addAction(QStringLiteral("Cu&t"));
    cutAction_->setShortcut(QKeySequence::Cut);
    connect(cutAction_, &QAction::triggered, this, [this] {
        if (auto* editor = currentEditor()) {
            editor->cutSelection();
        }
    });

    copyAction_ = editMenu->addAction(QStringLiteral("&Copy"));
    copyAction_->setShortcut(QKeySequence::Copy);
    connect(copyAction_, &QAction::triggered, this, [this] {
        if (auto* editor = currentEditor()) {
            editor->copySelection();
        }
    });

    pasteAction_ = editMenu->addAction(QStringLiteral("&Paste"));
    pasteAction_->setShortcut(QKeySequence::Paste);
    connect(pasteAction_, &QAction::triggered, this, [this] {
        if (auto* editor = currentEditor()) {
            editor->pasteClipboard();
        }
    });

    deleteAction_ = editMenu->addAction(QStringLiteral("&Delete"));
    deleteAction_->setShortcut(QKeySequence::Delete);
    connect(deleteAction_, &QAction::triggered, this, [this] {
        if (auto* editor = currentEditor()) {
            editor->deleteSelection();
        }
    });

    selectAllAction_ = editMenu->addAction(QStringLiteral("Select &All"));
    selectAllAction_->setShortcut(QKeySequence::SelectAll);
    connect(selectAllAction_, &QAction::triggered, this, [this] {
        if (auto* editor = currentEditor()) {
            editor->selectAllText();
        }
    });

    editMenu->addSeparator();
    auto* findAction = editMenu->addAction(QStringLiteral("&Find…"));
    findAction->setShortcut(QKeySequence::Find);
    connect(findAction, &QAction::triggered, this, [this] { openFindBar(false); });

    auto* findNextAction = editMenu->addAction(QStringLiteral("Find &Next"));
    findNextAction->setShortcut(QKeySequence::FindNext);
    connect(findNextAction, &QAction::triggered, this, [this] { findNext(false); });

    auto* findPreviousAction = editMenu->addAction(QStringLiteral("Find &Previous"));
    findPreviousAction->setShortcut(QKeySequence::FindPrevious);
    connect(findPreviousAction, &QAction::triggered, this, [this] { findNext(true); });

    auto* replaceAction = editMenu->addAction(QStringLiteral("&Replace…"));
    replaceAction->setShortcut(QKeySequence::Replace);
    connect(replaceAction, &QAction::triggered, this, [this] { openFindBar(true); });

    editMenu->addSeparator();
    auto* settingsAction = editMenu->addAction(QStringLiteral("Settings…"));
    settingsAction->setMenuRole(QAction::PreferencesRole);
    settingsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::showSettings);

    connect(editMenu, &QMenu::aboutToShow, this, &MainWindow::updateEditorActions);

    gitMenu_ = menuBar()->addMenu(QStringLiteral("&Git"));
    sourceControlAction_ = gitMenu_->addAction(QStringLiteral("Source Control"));
    sourceControlAction_->setCheckable(true);
    sourceControlAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+G")));
    connect(sourceControlAction_, &QAction::toggled, this,
            &MainWindow::setSourceControlVisible);

    gitMenu_->addSeparator();
    viewDiffAction_ = gitMenu_->addAction(QStringLiteral("View Current File Diff"));
    connect(viewDiffAction_, &QAction::triggered, this, &MainWindow::requestCurrentFileDiff);

    auto* refreshGitAction = gitMenu_->addAction(QStringLiteral("Refresh Status"));
    connect(refreshGitAction, &QAction::triggered, git_, &GitService::refresh);

    gitMenu_->addSeparator();
    worktreeMenu_ = gitMenu_->addMenu(QStringLiteral("Switch Worktree"));
    connect(worktreeMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildWorktreeMenu);
    connect(gitMenu_, &QMenu::aboutToShow, this, [this] {
        updateGitActions();
        git_->scheduleRefresh(0);
    });

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    explorerAction_ = viewMenu->addAction(QStringLiteral("Explorer"));
    explorerAction_->setCheckable(true);
    explorerAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
    connect(explorerAction_, &QAction::toggled, this, &MainWindow::setExplorerVisible);

    viewMenu->addAction(sourceControlAction_);

    auto* focusExplorerAction = viewMenu->addAction(QStringLiteral("Focus Explorer"));
    focusExplorerAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));
    connect(focusExplorerAction, &QAction::triggered, this, &MainWindow::focusExplorer);

    markdownPreviewAction_ = viewMenu->addAction(QStringLiteral("Markdown Preview"));
    markdownPreviewAction_->setCheckable(true);
    markdownPreviewAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    connect(markdownPreviewAction_, &QAction::toggled, this,
            &MainWindow::setMarkdownPreviewVisible);

    terminalAction_ = viewMenu->addAction(QStringLiteral("Terminal"));
    terminalAction_->setCheckable(true);
    terminalAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+`")));
    connect(terminalAction_, &QAction::toggled, this, &MainWindow::setTerminalVisible);

    auto* focusTerminalAction = viewMenu->addAction(QStringLiteral("Focus Terminal"));
    focusTerminalAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+`")));
    connect(focusTerminalAction, &QAction::triggered, this, &MainWindow::focusTerminal);

    viewMenu->addSeparator();
    auto* nextTabAction = viewMenu->addAction(QStringLiteral("Next Tab"));
    nextTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Tab")));
    connect(nextTabAction, &QAction::triggered, this, [this] { activateAdjacentTab(1); });

    auto* previousTabAction = viewMenu->addAction(QStringLiteral("Previous Tab"));
    previousTabAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+Tab")));
    connect(previousTabAction, &QAction::triggered, this, [this] { activateAdjacentTab(-1); });

    viewMenu->addSeparator();
    auto* appearanceMenu = viewMenu->addMenu(QStringLiteral("Appearance"));
    auto* appearanceGroup = new QActionGroup(this);
    appearanceGroup->setExclusive(true);

    const auto addAppearanceAction = [this, appearanceMenu, appearanceGroup](
                                         const QString& label, const ThemeManager::Mode mode) {
        auto* action = appearanceMenu->addAction(label);
        action->setCheckable(true);
        action->setData(static_cast<int>(mode));
        action->setChecked(theme_.mode() == mode);
        appearanceGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { theme_.setMode(mode); });
    };

    addAppearanceAction(QStringLiteral("System"), ThemeManager::Mode::System);
    addAppearanceAction(QStringLiteral("Light"), ThemeManager::Mode::Light);
    addAppearanceAction(QStringLiteral("Dark"), ThemeManager::Mode::Dark);
    connect(&theme_, &ThemeManager::themeChanged, this, [this, appearanceGroup] {
        for (auto* action : appearanceGroup->actions()) {
            action->setChecked(action->data().toInt() == static_cast<int>(theme_.mode()));
        }
    });
}
} // namespace ketplus
