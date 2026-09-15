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
#include <QShortcut>

#include <ScintillaMessages.h>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>

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

    recentFilesMenu_ = fileMenu->addMenu(QStringLiteral("Open Recent File"));
    connect(recentFilesMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentFilesMenu);

    auto* closeTabAction = fileMenu->addAction(QStringLiteral("&Close Tab"));
    closeTabAction->setShortcut(QKeySequence::Close);
    connect(closeTabAction, &QAction::triggered, this, &MainWindow::closeCurrentTab);

    reopenClosedTabAction_ = fileMenu->addAction(QStringLiteral("&Reopen Closed Tab"));
    reopenClosedTabAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+T")));
    reopenClosedTabAction_->setEnabled(false);
    connect(reopenClosedTabAction_, &QAction::triggered, this, &MainWindow::reopenClosedTab);

    fileMenu->addSeparator();

    auto* saveAction = fileMenu->addAction(QStringLiteral("&Save"));
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &MainWindow::saveCurrentDocument);

    auto* saveAsAction = fileMenu->addAction(QStringLiteral("Save &As…"));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveCurrentDocumentAs);

    auto* saveAllAction = fileMenu->addAction(QStringLiteral("Save A&ll"));
    saveAllAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+S")));
    connect(saveAllAction, &QAction::triggered, this, &MainWindow::saveAllDocuments);

    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(QStringLiteral("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    undoAction_ = editMenu->addAction(QStringLiteral("&Undo"));
    undoAction_->setShortcut(QKeySequence::Undo);
    connect(undoAction_, &QAction::triggered, this, [this] {
        if (auto* editor = activeEditor()) {
            editor->undoEdit();
        }
    });

    redoAction_ = editMenu->addAction(QStringLiteral("&Redo"));
    redoAction_->setShortcut(QKeySequence::Redo);
    connect(redoAction_, &QAction::triggered, this, [this] {
        if (auto* editor = activeEditor()) {
            editor->redoEdit();
        }
    });

    editMenu->addSeparator();
    cutAction_ = editMenu->addAction(QStringLiteral("Cu&t"));
    cutAction_->setShortcut(QKeySequence::Cut);
    connect(cutAction_, &QAction::triggered, this, [this] {
        if (auto* editor = activeEditor()) {
            editor->cutSelection();
        }
    });

    copyAction_ = editMenu->addAction(QStringLiteral("&Copy"));
    copyAction_->setShortcut(QKeySequence::Copy);
    connect(copyAction_, &QAction::triggered, this, [this] {
        if (auto* editor = activeEditor()) {
            editor->copySelection();
        }
    });

    pasteAction_ = editMenu->addAction(QStringLiteral("&Paste"));
    pasteAction_->setShortcut(QKeySequence::Paste);
    connect(pasteAction_, &QAction::triggered, this, [this] {
        if (auto* editor = activeEditor()) {
            editor->pasteClipboard();
        }
    });

    deleteAction_ = editMenu->addAction(QStringLiteral("&Delete"));
    deleteAction_->setShortcut(QKeySequence::Delete);
    connect(deleteAction_, &QAction::triggered, this, [this] {
        if (auto* editor = activeEditor()) {
            editor->deleteSelection();
        }
    });

    selectAllAction_ = editMenu->addAction(QStringLiteral("Select &All"));
    selectAllAction_->setShortcut(QKeySequence::SelectAll);
    connect(selectAllAction_, &QAction::triggered, this, [this] {
        if (auto* editor = activeEditor()) {
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

    const auto addEditorAction = [this](QMenu* menu, const QString& label,
                                        const QKeySequence& shortcut,
                                        const std::function<void(EditorWidget&)>& handler) {
        auto* action = menu->addAction(label);
        action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, [this, handler] {
            if (auto* editor = activeEditor(); editor != nullptr && !editor->isHibernated()) {
                handler(*editor);
            }
        });
        return action;
    };
    const auto addViewOption = [this](QMenu* menu, const QString& label,
                                      const QKeySequence& shortcut,
                                      bool EditorViewOptions::*field) {
        auto* action = menu->addAction(label);
        action->setCheckable(true);
        action->setChecked(viewOptions_.*field);
        action->setShortcut(shortcut);
        connect(action, &QAction::toggled, this, [this, field](const bool checked) {
            auto options = viewOptions_;
            options.*field = checked;
            applyViewOptions(options);
        });
        return action;
    };

    editMenu->addSeparator();
    auto* lineMenu = editMenu->addMenu(QStringLiteral("&Line"));
    addEditorAction(lineMenu, QStringLiteral("&Duplicate Line"),
                    QKeySequence(QStringLiteral("Ctrl+Shift+D")),
                    [](EditorWidget& editor) { editor.duplicateLines(); });
    addEditorAction(lineMenu, QStringLiteral("Move Line &Up"), QKeySequence(QStringLiteral("Alt+Up")),
                    [](EditorWidget& editor) { editor.moveLinesUp(); });
    addEditorAction(lineMenu, QStringLiteral("Move Line Dow&n"),
                    QKeySequence(QStringLiteral("Alt+Down")),
                    [](EditorWidget& editor) { editor.moveLinesDown(); });
    addEditorAction(lineMenu, QStringLiteral("De&lete Line"),
                    QKeySequence(QStringLiteral("Ctrl+Shift+K")),
                    [](EditorWidget& editor) { editor.deleteLines(); });
    addEditorAction(lineMenu, QStringLiteral("&Join Lines"), QKeySequence(QStringLiteral("Ctrl+J")),
                    [](EditorWidget& editor) { editor.joinLines(); });
    lineMenu->addSeparator();
    addEditorAction(lineMenu, QStringLiteral("&Sort Lines"), {},
                    [](EditorWidget& editor) { editor.sortLines(false); });
    addEditorAction(lineMenu, QStringLiteral("Sort Lines and &Remove Duplicates"), {},
                    [](EditorWidget& editor) { editor.sortLines(true); });
    addEditorAction(lineMenu, QStringLiteral("&Trim Trailing Whitespace"), {},
                    [](EditorWidget& editor) { editor.trimTrailingWhitespace(); });

    addEditorAction(editMenu, QStringLiteral("Toggle &Comment"),
                    QKeySequence(QStringLiteral("Ctrl+/")), [this](EditorWidget& editor) {
                        if (!editor.toggleComment()) {
                            statusBar()->showMessage(
                                QStringLiteral("Comments are not available for this file type"),
                                2500);
                        }
                    });
    auto* caseMenu = editMenu->addMenu(QStringLiteral("Convert Ca&se"));
    addEditorAction(caseMenu, QStringLiteral("&UPPERCASE"),
                    QKeySequence(QStringLiteral("Ctrl+K, Ctrl+U")),
                    [](EditorWidget& editor) { editor.convertCase(true); });
    addEditorAction(caseMenu, QStringLiteral("&lowercase"),
                    QKeySequence(QStringLiteral("Ctrl+K, Ctrl+L")),
                    [](EditorWidget& editor) { editor.convertCase(false); });
    editMenu->addSeparator();
    addViewOption(editMenu, QStringLiteral("Auto-Close Brackets"), {},
                  &EditorViewOptions::autoCloseBrackets);
    addViewOption(editMenu, QStringLiteral("Auto-Indent"), {}, &EditorViewOptions::autoIndent);
    addViewOption(editMenu, QStringLiteral("Word Completion"), {},
                  &EditorViewOptions::wordCompletion);
#ifdef Q_OS_MACOS
    const QKeySequence completionShortcut(QStringLiteral("Meta+Space"));
#else
    const QKeySequence completionShortcut(QStringLiteral("Ctrl+Space"));
#endif
    addEditorAction(editMenu, QStringLiteral("Suggest Words"), completionShortcut,
                    [this](EditorWidget& editor) {
                        if (!editor.showWordCompletions(true)) {
                            statusBar()->showMessage(QStringLiteral("No word suggestions"), 2000);
                        }
                    });

    auto* selectionMenu = editMenu->addMenu(QStringLiteral("Selectio&n"));
    addEditorAction(selectionMenu, QStringLiteral("Add &Next Occurrence"),
                    QKeySequence(QStringLiteral("Ctrl+D")),
                    [](EditorWidget& editor) { editor.addNextOccurrence(); });
    addEditorAction(selectionMenu, QStringLiteral("Select &All Occurrences"),
                    QKeySequence(QStringLiteral("Ctrl+Shift+L")),
                    [](EditorWidget& editor) { editor.selectAllOccurrences(); });
    selectionMenu->addSeparator();
    addEditorAction(selectionMenu, QStringLiteral("Add Cursor &Above"),
                    QKeySequence(QStringLiteral("Ctrl+Alt+Up")),
                    [](EditorWidget& editor) { editor.addCursorVertically(true); });
    addEditorAction(selectionMenu, QStringLiteral("Add Cursor &Below"),
                    QKeySequence(QStringLiteral("Ctrl+Alt+Down")),
                    [](EditorWidget& editor) { editor.addCursorVertically(false); });
    addEditorAction(selectionMenu, QStringLiteral("&Split Selection into Lines"),
                    QKeySequence(QStringLiteral("Alt+Shift+I")),
                    [](EditorWidget& editor) { editor.splitSelectionIntoLines(); });
    addEditorAction(selectionMenu, QStringLiteral("Single &Cursor"), {},
                    [](EditorWidget& editor) { editor.collapseToMainSelection(); });

    editMenu->addSeparator();
    auto* settingsAction = editMenu->addAction(QStringLiteral("Settings…"));
    settingsAction->setMenuRole(QAction::PreferencesRole);
    settingsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::showSettings);

    connect(editMenu, &QMenu::aboutToShow, this, &MainWindow::updateEditorActions);

    auto* goMenu = menuBar()->addMenu(QStringLiteral("&Go"));
    auto* goToLineAction = goMenu->addAction(QStringLiteral("Go to &Line…"));
    goToLineAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    connect(goToLineAction, &QAction::triggered, this, &MainWindow::goToLine);
    addEditorAction(goMenu, QStringLiteral("Jump to Matching &Bracket"),
                    QKeySequence(QStringLiteral("Ctrl+Shift+\\")), [this](EditorWidget& editor) {
                        if (!editor.jumpToMatchingBrace()) {
                            statusBar()->showMessage(
                                QStringLiteral("No matching bracket at the cursor"), 2500);
                        }
                    });
    goMenu->addSeparator();
    addEditorAction(goMenu, QStringLiteral("&Toggle Bookmark"),
                    QKeySequence(QStringLiteral("Ctrl+F2")),
                    [](EditorWidget& editor) { editor.toggleBookmark(); });
    const auto reportMissingBookmarks = [this](const bool found) {
        if (!found) {
            statusBar()->showMessage(QStringLiteral("No bookmarks in this file"), 2500);
        }
    };
    addEditorAction(goMenu, QStringLiteral("&Next Bookmark"), QKeySequence(QStringLiteral("F2")),
                    [reportMissingBookmarks](EditorWidget& editor) {
                        reportMissingBookmarks(editor.goToNextBookmark());
                    });
    addEditorAction(goMenu, QStringLiteral("&Previous Bookmark"),
                    QKeySequence(QStringLiteral("Shift+F2")),
                    [reportMissingBookmarks](EditorWidget& editor) {
                        reportMissingBookmarks(editor.goToPreviousBookmark());
                    });
    addEditorAction(goMenu, QStringLiteral("&Clear Bookmarks"), {},
                    [](EditorWidget& editor) { editor.clearBookmarks(); });

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
    addViewOption(viewMenu, QStringLiteral("Word Wrap"), QKeySequence(QStringLiteral("Alt+Z")),
                  &EditorViewOptions::wordWrap);
    addViewOption(viewMenu, QStringLiteral("Show Whitespace"), {},
                  &EditorViewOptions::showWhitespace);
    addViewOption(viewMenu, QStringLiteral("Indent Guides"), {}, &EditorViewOptions::indentGuides);
    addViewOption(viewMenu, QStringLiteral("Column Ruler"), {}, &EditorViewOptions::showRuler);
    addViewOption(viewMenu, QStringLiteral("Code Folding"), {}, &EditorViewOptions::codeFolding);
    addEditorAction(viewMenu, QStringLiteral("Fold All"), {},
                    [](EditorWidget& editor) { editor.setAllFoldsExpanded(false); });
    addEditorAction(viewMenu, QStringLiteral("Unfold All"), {},
                    [](EditorWidget& editor) { editor.setAllFoldsExpanded(true); });

    auto* indentationMenu = viewMenu->addMenu(QStringLiteral("Indentation"));
    addViewOption(indentationMenu, QStringLiteral("Indent Using Tabs"), {},
                  &EditorViewOptions::useTabs);
    indentationMenu->addSeparator();
    auto* tabWidthGroup = new QActionGroup(this);
    tabWidthGroup->setExclusive(true);
    for (const int width : {2, 4, 8}) {
        auto* action = indentationMenu->addAction(QStringLiteral("Tab Width: %1").arg(width));
        action->setCheckable(true);
        action->setChecked(viewOptions_.tabWidth == width);
        tabWidthGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, width] {
            auto options = viewOptions_;
            options.tabWidth = width;
            applyViewOptions(options);
        });
    }

    viewMenu->addSeparator();
    auto* zoomInAction = viewMenu->addAction(QStringLiteral("Zoom In"));
    zoomInAction->setShortcuts({QKeySequence::ZoomIn, QKeySequence(QStringLiteral("Ctrl+="))});
    connect(zoomInAction, &QAction::triggered, this, [this] { changeZoom(1); });
    auto* zoomOutAction = viewMenu->addAction(QStringLiteral("Zoom Out"));
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction, &QAction::triggered, this, [this] { changeZoom(-1); });
    auto* resetZoomAction = viewMenu->addAction(QStringLiteral("Reset Zoom"));
    resetZoomAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(resetZoomAction, &QAction::triggered, this, [this] { changeZoom(-viewOptions_.zoom); });

    viewMenu->addSeparator();
    auto* layoutMenu = viewMenu->addMenu(QStringLiteral("Editor Layout"));
    auto* splitRightAction = layoutMenu->addAction(QStringLiteral("Split Right"));
    splitRightAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+\\")));
    connect(splitRightAction, &QAction::triggered, this,
            [this] { openSplit(currentEditor(), Qt::Horizontal); });
    auto* splitDownAction = layoutMenu->addAction(QStringLiteral("Split Down"));
    splitDownAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+\\")));
    connect(splitDownAction, &QAction::triggered, this,
            [this] { openSplit(currentEditor(), Qt::Vertical); });
    auto* focusOtherViewAction = layoutMenu->addAction(QStringLiteral("Focus Other View"));
    focusOtherViewAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+F")));
    connect(focusOtherViewAction, &QAction::triggered, this, &MainWindow::focusOtherView);
    closeSplitAction_ = layoutMenu->addAction(QStringLiteral("Close Split"));
    closeSplitAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+W")));
    closeSplitAction_->setEnabled(false);
    connect(closeSplitAction_, &QAction::triggered, this, &MainWindow::closeSplit);

    fullScreenAction_ = viewMenu->addAction(QStringLiteral("Full Screen"));
    fullScreenAction_->setCheckable(true);
    fullScreenAction_->setShortcut(QKeySequence::FullScreen);
    connect(fullScreenAction_, &QAction::toggled, this, &MainWindow::setFullScreen);
    distractionFreeAction_ = viewMenu->addAction(QStringLiteral("Distraction-Free Mode"));
    distractionFreeAction_->setCheckable(true);
    distractionFreeAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+K, Z")));
    connect(distractionFreeAction_, &QAction::toggled, this, &MainWindow::setDistractionFree);

    // Escape leaves full screen; it is only active while full screen so editors keep it otherwise.
    exitFullScreenShortcut_ = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    exitFullScreenShortcut_->setContext(Qt::WindowShortcut);
    exitFullScreenShortcut_->setEnabled(false);
    connect(exitFullScreenShortcut_, &QShortcut::activated, this, [this] {
        // Let Escape dismiss editor state first: the suggestion list, then extra carets.
        if (auto* editor = activeEditor()) {
            if (editor->send(static_cast<unsigned int>(Scintilla::Message::AutoCActive)) != 0) {
                editor->send(static_cast<unsigned int>(Scintilla::Message::AutoCCancel));
                return;
            }
            if (editor->selectionCount() > 1) {
                editor->collapseToMainSelection();
                return;
            }
        }
        if (distractionFree_) {
            setDistractionFree(false);
        } else {
            setFullScreen(false);
        }
    });

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
