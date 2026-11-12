#include "app/ArchivedSettingsPage.h"
#include "app/AtlasBuilderTab.h"
#include "app/AtlasConversationPanel.h"
#include "app/MainWindow.h"

#include "app/RepositoryRouter.h"
#include "app/SpotlightDialog.h"
#include "atlas/AtlasTab.h"
#include "editor/DiskChangePrompt.h"
#include "editor/DocumentFormatControls.h"
#include "workspace/AtlasIndex.h"

#include "DaemonClient.h"
#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexAppServer.h"
#include "agent/CodexRateLimits.h"
#include "app/SettingsDialog.h"
#include "app/SettingsWorkspacePage.h"
#include "editor/EditorSplitPane.h"
#include "editor/EditorWidget.h"
#include "git/GitChangesPanel.h"
#include "git/GitDiffView.h"
#include "git/GitService.h"
#include "index/SymbolIndex.h"
#include "machine/MachineComposition.h"
#include "machine/MachineRichTranscript.h"
#include "machine/MachineWorkspaceView.h"
#include "preview/MarkdownPreviewPane.h"
#include "remote/RemoteControlView.h"
#include "teams/TeamDirectory.h"
#include "terminal/TerminalPanel.h"
#include "ui/FindReplaceBar.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"
#include "workspace/ExplorerPanel.h"

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
#include <QJsonArray>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
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
#include <memory>

#include <algorithm>

namespace ketplus {
namespace {
constexpr int maximumResidentTabCount = 12;
constexpr qsizetype residentTextBudgetBytes = 64 * 1024 * 1024;
constexpr int maximumRecentFolderCount = 10;
constexpr int maximumRecentFileCount = 15;
constexpr int maximumClosedTabCount = 20;

} // namespace

void MainWindow::createNewDocument() {
    auto* editor = createEditor();
    const int index = tabs_->addTab(editor, editor->document().displayName());
    configureTabCloseButton(index, editor);
    tabs_->setCurrentIndex(index);
    editor->setFocus();
}

void MainWindow::openDocument() {
    const auto paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Open files"));
    for (const auto& path : paths) {
        openFile(path);
    }
}

void MainWindow::chooseFolder() {
    const QString startPath = workspaceRoot_.isEmpty() ? QDir::homePath() : workspaceRoot_;
    const QString path =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Open Folder"), startPath);
    if (!path.isEmpty()) {
        openFolder(path);
    }
}

bool MainWindow::saveCurrentDocument() {
    auto* editor = currentEditor();
    return editor == nullptr || saveEditor(editor, false);
}

bool MainWindow::saveCurrentDocumentAs() {
    auto* editor = currentEditor();
    return editor == nullptr || saveEditor(editor, true);
}

bool MainWindow::saveEditor(EditorWidget* editor, const bool choosePath) {
    if (splitEditor_ != nullptr && editor == splitEditor_ && splitSource_ != nullptr) {
        // The split view shares its source tab's document; save through the tab.
        editor = splitSource_;
    }
    if (!disk_change::restoreBeforeSave(this, editor)) {
        return false;
    }
    auto& document = editor->document();
    QString path = document.filePath();
    if (choosePath || document.isUntitled()) {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Save file"), path);
        if (path.isEmpty()) {
            return false;
        }
    }

    if (QFileInfo(path).absoluteFilePath() == document.filePath() &&
        !disk_change::confirmSave(this, editor)) {
        return false;
    }

    const QByteArray content = editor->text();
    const auto result = document.saveAs(path, content);
    if (!result.ok) {
        QMessageBox::critical(this, QStringLiteral("Unable to save file"), result.error);
        return false;
    }

    editor->markSaved();
    rememberRecentFile(path);
    // The index holds what this file said before the save, so that one file is read again.
    if (symbolIndex_ != nullptr && !workspaceRoot_.isEmpty()) {
        const QString relative = QDir(workspaceRoot_).relativeFilePath(path);
        if (!relative.startsWith(QStringLiteral(".."))) {
            symbolIndex_->reindexFile(workspaceRoot_, relative);
        }
    }
    editor->configureLexerForPath(path);
    editor->applyTheme(theme_.palette());
    if (splitPane_ != nullptr && editor == splitSource_) {
        // Save As may pick a new lexer, so refresh the split view's styles.
        splitPane_->refreshCurrentSource();
    }
    updateTabTitle(editor);
    updateMarkdownPreview();
    format_->updateIndicators();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 2500);
    git_->scheduleRefresh();
    if (QFileInfo(path).suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0)
        atlasIndex_->scheduleRefresh();
    if (editor->isLargeFileMode()) {
        statusBar()->showMessage(
            QStringLiteral("Large file mode: syntax highlighting and undo are disabled"), 5000);
    }
    return true;
}

bool MainWindow::maybeCloseEditor(EditorWidget* editor) {
    if (!editor->document().isModified()) {
        return true;
    }

    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Unsaved changes"),
        QStringLiteral("Save changes to %1?").arg(editor->document().displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

    if (answer == QMessageBox::Cancel) {
        return false;
    }
    if (answer == QMessageBox::Save) {
        return saveEditor(editor, false);
    }
    return true;
}

bool MainWindow::closeTab(const int index, const bool createReplacement) {
    auto* tab = tabs_->widget(index);
    if (tab == nullptr) {
        return false;
    }
    auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
    if (editor != nullptr && !maybeCloseEditor(editor)) {
        return false;
    }
    if (editor != nullptr && splitPane_ != nullptr) {
        splitPane_->removeSource(editor);
    }
    if (editor != nullptr && !editor->document().isUntitled()) {
        const QString path = editor->document().filePath();
        closedFilePaths_.removeAll(path);
        closedFilePaths_.append(path);
        while (closedFilePaths_.size() > maximumClosedTabCount) {
            closedFilePaths_.removeFirst();
        }
        reopenClosedTabAction_->setEnabled(true);
    }

    tabs_->removeTab(index);
    tab->deleteLater();
    if (createReplacement && tabs_->count() == 0) {
        createNewDocument();
    }
    updateEditorActions();
    return true;
}

void MainWindow::closeOtherTabs(const int keepIndex) {
    auto* keepTab = tabs_->widget(keepIndex);
    if (keepTab == nullptr) {
        return;
    }

    tabs_->setCurrentWidget(keepTab);
    for (int index = tabs_->count() - 1; index >= 0; --index) {
        if (tabs_->widget(index) != keepTab && !closeTab(index, false)) {
            break;
        }
    }
}

void MainWindow::closeAllTabs() {
    for (int index = tabs_->count() - 1; index >= 0; --index) {
        if (!closeTab(index, false)) {
            return;
        }
    }
    createNewDocument();
}

void MainWindow::closeCurrentTab() {
    if (atlasWorkspaceMode_) {
        closeAtlasTab(atlasTabs_->currentIndex());
        return;
    }
    if (agentWorkspaceMode_ && agentDetailTabs_ != nullptr &&
        agentDetailTabs_->currentIndex() >= 0) {
        closeAgentDetailTab(agentDetailTabs_->currentIndex());
        return;
    }
    if (tabs_->currentIndex() >= 0) {
        closeTab(tabs_->currentIndex());
    }
}

void MainWindow::showTabContextMenu(const QPoint& position) {
    const int index = tabs_->tabBar()->tabAt(position);
    if (index < 0) {
        return;
    }

    auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
    QMenu menu(this);
    auto* previewAction = addMarkdownPreviewContextAction(menu, editor);
    QAction* splitRightAction = nullptr;
    QAction* splitDownAction = nullptr;
    if (editor != nullptr) {
        menu.addSeparator();
        splitRightAction = menu.addAction(QStringLiteral("Open in Split Right"));
        splitDownAction = menu.addAction(QStringLiteral("Open in Split Down"));
    }
    menu.addSeparator();
    auto* closeAction = menu.addAction(QStringLiteral("Close"));
    auto* closeOthersAction = menu.addAction(QStringLiteral("Close Other Tabs"));
    auto* closeAllAction = menu.addAction(QStringLiteral("Close All Tabs"));
    closeOthersAction->setEnabled(tabs_->count() > 1);

    QAction* selected = menu.exec(tabs_->tabBar()->mapToGlobal(position));
    if (selected != nullptr && selected == previewAction) {
        tabs_->setCurrentIndex(index);
        setMarkdownPreviewVisible(previewAction->isChecked());
    } else if (selected != nullptr && selected == splitRightAction) {
        openSplit(editor, Qt::Horizontal);
    } else if (selected != nullptr && selected == splitDownAction) {
        openSplit(editor, Qt::Vertical);
    } else if (selected == closeAction) {
        closeTab(index);
    } else if (selected == closeOthersAction) {
        closeOtherTabs(index);
    } else if (selected == closeAllAction) {
        closeAllTabs();
    }
}

void MainWindow::showEditorContextMenu(EditorWidget* editor, const QPoint& position) {
    if (editor == nullptr) {
        return;
    }

    // The menu's editing commands act on this editor, even if the pointer never gave it focus.
    editor->setFocus(Qt::PopupFocusReason);
    updateEditorActions();
    QMenu menu(this);
    menu.addAction(undoAction_);
    menu.addAction(redoAction_);
    menu.addSeparator();
    menu.addAction(cutAction_);
    menu.addAction(copyAction_);
    menu.addAction(pasteAction_);
    menu.addAction(deleteAction_);
    menu.addSeparator();
    menu.addAction(selectAllAction_);
    menu.addSeparator();
    menu.addAction(goToDefinitionAction_);
    // The split view edits its source tab's document, so split from that tab. Agent detail
    // editors live outside the document tabs and cannot be split.
    auto* source = editor == splitEditor_ && splitSource_ != nullptr ? splitSource_ : editor;
    QAction* splitRightAction = nullptr;
    QAction* splitDownAction = nullptr;
    if (tabs_->indexOf(source) >= 0 && !agentWorkspaceMode_ && !atlasWorkspaceMode_) {
        menu.addSeparator();
        splitRightAction = menu.addAction(QStringLiteral("Split Right"));
        splitRightAction->setObjectName(QStringLiteral("editorSplitRightAction"));
        splitRightAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+\\")));
        splitDownAction = menu.addAction(QStringLiteral("Split Down"));
        splitDownAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+\\")));
        if (splitPane_ != nullptr) {
            menu.addAction(closeSplitAction_);
        }
    }
    QAction* previewAction = nullptr;
    if (editor->syntaxName() == QStringLiteral("markdown")) {
        menu.addSeparator();
        previewAction = addMarkdownPreviewContextAction(menu, editor);
    }

    QAction* selected = menu.exec(editor->mapToGlobal(position));
    if (selected == nullptr) {
        return;
    }
    if (selected == previewAction) {
        setMarkdownPreviewVisible(previewAction->isChecked());
    } else if (selected == splitRightAction) {
        openSplit(source, Qt::Horizontal);
    } else if (selected == splitDownAction) {
        openSplit(source, Qt::Vertical);
    }
}

QAction* MainWindow::addMarkdownPreviewContextAction(QMenu& menu, EditorWidget* editor) {
    // Only Markdown documents offer a preview.
    if (editor == nullptr || editor->syntaxName() != QStringLiteral("markdown")) {
        return nullptr;
    }
    auto* action = menu.addAction(QStringLiteral("Markdown Preview"));
    action->setCheckable(true);
    action->setChecked(editor->isMarkdownPreviewVisible());
    action->setShortcut(markdownPreviewAction_->shortcut());
    return action;
}

void MainWindow::activateAdjacentTab(const int offset) {
    auto* host =
        atlasWorkspaceMode_ ? atlasTabs_
        : agentWorkspaceMode_ && agentDetailTabs_ != nullptr && agentDetailTabs_->count() > 0
            ? agentDetailTabs_
            : tabs_;
    if (host->count() < 2) {
        return;
    }
    const int nextIndex = (host->currentIndex() + offset + host->count()) % host->count();
    host->setCurrentIndex(nextIndex);
}

EditorWidget* MainWindow::currentEditor() const {
    if (atlasWorkspaceMode_)
        return nullptr;
    if (agentWorkspaceMode_) {
        return agentDetailTabs_ == nullptr
                   ? nullptr
                   : qobject_cast<EditorWidget*>(agentDetailTabs_->currentWidget());
    }
    if (splitEditor_ != nullptr && splitFocused_ && splitEditor_->isVisible()) {
        return splitEditor_;
    }
    return qobject_cast<EditorWidget*>(tabs_->currentWidget());
}

void MainWindow::configureTabCloseButton(const int index, EditorWidget* editor) {
    configureTabCloseButton(index, editor, editor->document().displayName());
}

void MainWindow::configureTabCloseButton(const int index, QWidget* tab,
                                         const QString& accessibleName) {
    auto* button = new QToolButton(tabs_->tabBar());
    button->setText(QString::fromUtf8("×"));
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(QStringLiteral("Close tab"));
    button->setAccessibleName(QStringLiteral("Close %1").arg(accessibleName));
    button->setProperty("kvRole", QStringLiteral("tabClose"));
    connect(button, &QToolButton::clicked, this, [this, tab] {
        const int currentIndex = tabs_->indexOf(tab);
        if (currentIndex >= 0) {
            closeTab(currentIndex);
        }
    });
    tabs_->tabBar()->setTabButton(index, QTabBar::RightSide, button);
}

EditorWidget* MainWindow::createEditor() {
    auto* editor = new EditorWidget(this);
    editor->setPreviewTypography(appearanceSettings_.preview.fontSizePixels,
                                 appearanceSettings_.preview.lineHeightPixels);
    connect(editor, &EditorWidget::previewFileOpenRequested, this, &MainWindow::openFile);
    connect(editor, &EditorWidget::previewStatusMessageRequested, this,
            [this](const QString& message) { statusBar()->showMessage(message, 7000); });
    editor->setEditorSettings(appearanceSettings_.editor);
    editor->setViewOptions(viewOptions_);
    editor->applyTheme(theme_.palette());
    // Ctrl+wheel zooms one Scintilla view; mirror it to every editor and persist it.
    connect(editor, &ScintillaEditBase::zoom, this, [this](const int zoom) {
        if (zoom != viewOptions_.zoom) {
            auto options = viewOptions_;
            options.zoom = zoom;
            applyViewOptions(options);
        }
    });
    connect(editor, &EditorWidget::contextMenuRequested, this,
            [this, editor](const QPoint& position) { showEditorContextMenu(editor, position); });
    connect(
        editor, &EditorWidget::definitionRequested, this,
        [this, editor](const QString& symbol, const QString& fileToken, const QString& lineText) {
            resolveDefinition(editor, symbol, fileToken, lineText);
        });
    connect(editor, &EditorWidget::dirtyStateChanged, this, [this, editor](const bool dirty) {
        updateTabTitle(editor);
        if (currentEditor() == editor) {
            updateDocumentState();
        }
        if (!dirty) {
            enforceTabResourcePolicy();
        }
    });
    connect(editor, &EditorWidget::cursorPositionChanged, this,
            [this, editor](const int line, const int column) {
                if (currentEditor() == editor) {
                    cursorPositionLabel_->setText(
                        QStringLiteral("Ln %1, Col %2").arg(line).arg(column));
                    trackNavigation(editor);
                }
            });
    connect(editor, &EditorWidget::editorStateChanged, this, [this, editor] {
        if (currentEditor() == editor) {
            updateEditorActions();
            updateMarkdownPreview();
        }
    });
    connect(editor, &EditorWidget::notifyChange, this, [this, editor] {
        if (currentEditor() == editor) {
            updateMarkdownPreview();
        }
    });
    return editor;
}

void MainWindow::updateTabTitle(EditorWidget* editor) {
    if (splitPane_ != nullptr) {
        splitPane_->updateSourceTitle(editor);
    }
    QTabWidget* host = tabs_;
    int index = host->indexOf(editor);
    if (index < 0 && agentDetailTabs_ != nullptr) {
        host = agentDetailTabs_;
        index = host->indexOf(editor);
    }
    if (index < 0) {
        return;
    }

    auto title = editor->document().displayName();
    if (editor->document().isModified()) {
        title.append(QStringLiteral(" •"));
    }
    host->setTabText(index, title);
    if (auto* button =
            qobject_cast<QToolButton*>(host->tabBar()->tabButton(index, QTabBar::RightSide))) {
        button->setAccessibleName(QStringLiteral("Close %1").arg(editor->document().displayName()));
    }
}

void MainWindow::updateEditorActions() {
    if (enableAgentEditActions())
        return;
    const auto* editor = currentEditor();
    const bool hasEditor = editor != nullptr;
    const bool hasSelection = hasEditor && editor->hasSelection();

    undoAction_->setEnabled(hasEditor && editor->canUndoEdit());
    redoAction_->setEnabled(hasEditor && editor->canRedoEdit());
    cutAction_->setEnabled(hasSelection);
    copyAction_->setEnabled(hasSelection);
    pasteAction_->setEnabled(hasEditor && editor->canPasteEdit());
    deleteAction_->setEnabled(hasSelection);
    selectAllAction_->setEnabled(hasEditor && !editor->isEmpty());
}

void MainWindow::updateDocumentState() {
    format_->updateIndicators();
    const auto* editor = documentOwner();
    if (editor == nullptr) {
        documentStateLabel_->setText(qobject_cast<GitDiffView*>(tabs_->currentWidget()) != nullptr
                                         ? QStringLiteral("Git Diff")
                                         : QString());
        return;
    }

    const bool dirty = editor->document().isModified();
    documentStateLabel_->setText(dirty                             ? QStringLiteral("● Modified")
                                 : editor->document().isUntitled() ? QStringLiteral("New")
                                                                   : QStringLiteral("Saved"));
    documentStateLabel_->setToolTip(
        dirty ? QStringLiteral("This tab has unsaved changes and will remain in memory")
              : QStringLiteral("This tab has no unsaved changes"));
}

void MainWindow::activateCurrentTab() {
    auto* editor = currentEditor();
    if (editor == nullptr) {
        return;
    }

    if (editor->isHibernated()) {
        const auto result = editor->restoreFromDisk();
        if (!result.ok) {
            QMessageBox::critical(
                this, QStringLiteral("Unable to restore tab"),
                QStringLiteral("%1\n\nThe tab stays read-only until the file can be read.")
                    .arg(result.error));
            return;
        }
        editor->applyTheme(theme_.palette());
    } else if (editor->hasPendingTheme()) {
        editor->applyTheme(theme_.palette());
    }

    editor->markActivated(++tabAccessSequence_);
    editor->setFocus();
    // The Explorer follows the tab, so the file being edited is always the selected one.
    if (explorer_ != nullptr && !editor->document().isUntitled()) {
        explorer_->revealPath(editor->document().filePath());
    }
    enforceTabResourcePolicy();
}

void MainWindow::enforceTabResourcePolicy() {
    int residentCount = 0;
    qsizetype residentBytes = 0;
    QVector<EditorWidget*> candidates;
    auto* activeEditor = currentEditor();

    for (int index = 0; index < tabs_->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (editor == nullptr || editor->isHibernated()) {
            continue;
        }
        ++residentCount;
        residentBytes += editor->residentBytes();
        // Hibernating a document shown in the split group would clear the text both views
        // share. The current tab stays resident even while the split view holds focus.
        const bool shownInSplit = splitPane_ != nullptr && splitPane_->containsSource(editor);
        if (editor != activeEditor && !shownInSplit && editor != tabs_->currentWidget() &&
            !editor->document().isUntitled() && !editor->document().isModified()) {
            candidates.append(editor);
        }
    }

    std::ranges::sort(candidates, {}, &EditorWidget::lastActivated);
    for (auto* editor : candidates) {
        if (residentCount <= maximumResidentTabCount && residentBytes <= residentTextBudgetBytes) {
            break;
        }
        const qsizetype releasedBytes = editor->residentBytes();
        if (editor->hibernate()) {
            --residentCount;
            residentBytes -= releasedBytes;
        }
    }
}

void MainWindow::rememberRecentFolder(const QString& path) {
    QSettings settings;
    QStringList folders = settings.value(QStringLiteral("workspace/recentFolders")).toStringList();
    folders.removeAll(path);
    folders.prepend(path);
    while (folders.size() > maximumRecentFolderCount) {
        folders.removeLast();
    }
    settings.setValue(QStringLiteral("workspace/recentFolders"), folders);
}

void MainWindow::rebuildRecentFoldersMenu() {
    recentFoldersMenu_->clear();
    QSettings settings;
    const QStringList storedFolders =
        settings.value(QStringLiteral("workspace/recentFolders")).toStringList();
    QStringList availableFolders;
    for (const QString& path : storedFolders) {
        if (QFileInfo(path).isDir() && !availableFolders.contains(path)) {
            availableFolders.append(path);
            auto* action = recentFoldersMenu_->addAction(path);
            connect(action, &QAction::triggered, this, [this, path] { openFolder(path); });
        }
    }
    settings.setValue(QStringLiteral("workspace/recentFolders"), availableFolders);

    if (availableFolders.isEmpty()) {
        auto* emptyAction = recentFoldersMenu_->addAction(QStringLiteral("No Recent Folders"));
        emptyAction->setEnabled(false);
        return;
    }

    recentFoldersMenu_->addSeparator();
    auto* clearAction = recentFoldersMenu_->addAction(QStringLiteral("Clear Recent Folders"));
    connect(clearAction, &QAction::triggered, this, [this] {
        QSettings().remove(QStringLiteral("workspace/recentFolders"));
        rebuildRecentFoldersMenu();
    });
}

AgentWorkspaceView* MainWindow::transcriptFindTarget() const {
    // Focus decides: a focused agent workspace (the main one or an embedded chat) takes Find;
    // a focused editor or the editor find bar keeps it.
    for (QWidget* widget = QApplication::focusWidget(); widget != nullptr;
         widget = widget->parentWidget()) {
        if (auto* workspace = qobject_cast<AgentWorkspaceView*>(widget))
            return workspace->isVisible() ? workspace : nullptr;
        if (widget == findBar_ || qobject_cast<EditorWidget*>(widget) != nullptr)
            return nullptr;
    }
    if (agentWorkspace_ == nullptr || !agentWorkspace_->isVisible())
        return nullptr;
    // Otherwise the visible transcript takes Find when its Quick scene holds focus, when no
    // editor is in front, or while this window is active and no editor is focused.
    if (agentWorkspace_->hasFocusWithin() || currentEditor() == nullptr || isActiveWindow())
        return agentWorkspace_;
    return nullptr;
}

void MainWindow::openFindBar(const bool replaceMode) {
    const auto* editor = currentEditor();
    if (auto* transcript = replaceMode ? nullptr : transcriptFindTarget()) {
        transcript->openTranscriptFind();
        return;
    }
    placeFindBar();
    findBar_->open(replaceMode, editor == nullptr ? QString() : editor->selectedText());
    highlightTimer_->start();
}

void MainWindow::findNext(const bool backwards) {
    if (auto* transcript = transcriptFindTarget()) {
        transcript->findInTranscript(backwards);
        return;
    }
    auto* editor = currentEditor();
    if (editor == nullptr) {
        return;
    }
    if (findBar_->query().isEmpty()) {
        openFindBar(false);
        return;
    }

    const auto result = editor->findText(findBar_->query(), backwards, searchOptions());
    findBar_->showSearchResult(result.found, result.wrapped);
}

void MainWindow::replaceCurrentMatch() {
    auto* editor = currentEditor();
    if (editor == nullptr || findBar_->query().isEmpty()) {
        return;
    }

    const auto options = searchOptions();
    const bool replaced =
        editor->replaceSelection(findBar_->query(), findBar_->replacement(), options);
    const auto result = editor->findText(findBar_->query(), false, options);
    findBar_->showSearchResult(result.found, replaced ? result.wrapped : false);
    refreshMatchHighlights(false);
}

void MainWindow::replaceAllMatches() {
    auto* editor = currentEditor();
    if (editor == nullptr || findBar_->query().isEmpty()) {
        return;
    }

    const int replacements =
        editor->replaceAll(findBar_->query(), findBar_->replacement(), searchOptions());
    findBar_->showReplacementCount(replacements);
    refreshMatchHighlights(false);
}

SearchOptions MainWindow::searchOptions() const {
    return {.matchCase = findBar_->matchCase(),
            .wholeWord = findBar_->wholeWord(),
            .regex = findBar_->regex(),
            .inSelection = findBar_->inSelection()};
}

void MainWindow::refreshMatchHighlights(const bool updateCount) {
    auto* editor = currentEditor();
    if (editor == nullptr || !findBar_->isVisible()) {
        return;
    }
    const int count = editor->highlightMatches(findBar_->query(), searchOptions());
    if (updateCount && !findBar_->query().isEmpty()) {
        findBar_->showMatchCount(count, count >= EditorWidget::highlightMatchLimit);
    }
}

void MainWindow::clearMatchHighlights() {
    for (auto* editor : openEditors()) {
        editor->clearMatchHighlights();
        editor->clearSearchScope();
    }
}

QList<EditorWidget*> MainWindow::openEditors() const {
    QList<EditorWidget*> editors;
    for (const QTabWidget* host : {tabs_, agentDetailTabs_}) {
        if (host == nullptr) {
            continue;
        }
        for (int index = 0; index < host->count(); ++index) {
            if (auto* editor = qobject_cast<EditorWidget*>(host->widget(index))) {
                editors.append(editor);
            }
        }
    }
    if (splitEditor_ != nullptr) {
        editors.append(splitEditor_);
    }
    return editors;
}

void MainWindow::applyViewOptions(const EditorViewOptions& options) {
    viewOptions_ = options.normalized();
    viewOptions_.save();
    for (auto* editor : openEditors()) {
        editor->setViewOptions(viewOptions_);
    }
    const auto* editor = currentEditor();
    if (editor != nullptr && editor->isLargeFileMode() &&
        (viewOptions_.wordWrap || viewOptions_.codeFolding)) {
        statusBar()->showMessage(
            QStringLiteral("Large file mode: word wrap and code folding stay off for this file"),
            4000);
    }
}

void MainWindow::goToLine() {
    auto* editor = currentEditor();
    if (editor == nullptr || editor->isHibernated()) {
        return;
    }
    bool accepted = false;
    const int line =
        QInputDialog::getInt(this, QStringLiteral("Go to Line"),
                             QStringLiteral("Line number (1–%1):").arg(editor->lineCount()),
                             editor->currentLine(), 1, editor->lineCount(), 1, &accepted);
    if (accepted) {
        pushNavigationLocation(locationOf(editor));
        editor->goToLine(line);
        editor->setFocus();
    }
}

bool MainWindow::saveAllDocuments() {
    int savedCount = 0;
    for (auto* editor : openEditors()) {
        if (!editor->document().isModified()) {
            continue;
        }
        if (editor->document().isUntitled()) {
            // Show which tab the save dialog belongs to.
            for (QTabWidget* host : {tabs_, agentDetailTabs_}) {
                if (host != nullptr && host->indexOf(editor) >= 0) {
                    host->setCurrentWidget(editor);
                }
            }
        }
        if (!saveEditor(editor, false)) {
            return false;
        }
        ++savedCount;
    }
    statusBar()->showMessage(savedCount == 0 ? QStringLiteral("No unsaved changes")
                                             : QStringLiteral("Saved %1 file(s)").arg(savedCount),
                             2500);
    return true;
}

void MainWindow::reopenClosedTab() {
    while (!closedFilePaths_.isEmpty()) {
        const QString path = closedFilePaths_.takeLast();
        if (QFileInfo(path).isFile()) {
            openFile(path);
            break;
        }
    }
    reopenClosedTabAction_->setEnabled(!closedFilePaths_.isEmpty());
}

void MainWindow::rememberRecentFile(const QString& path) {
    QSettings settings;
    QStringList files = settings.value(QStringLiteral("workspace/recentFiles")).toStringList();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > maximumRecentFileCount) {
        files.removeLast();
    }
    settings.setValue(QStringLiteral("workspace/recentFiles"), files);
}

void MainWindow::rebuildRecentFilesMenu() {
    recentFilesMenu_->clear();
    QSettings settings;
    const QStringList storedFiles =
        settings.value(QStringLiteral("workspace/recentFiles")).toStringList();
    QStringList availableFiles;
    for (const QString& path : storedFiles) {
        if (QFileInfo(path).isFile() && !availableFiles.contains(path)) {
            availableFiles.append(path);
            auto* action = recentFilesMenu_->addAction(path);
            connect(action, &QAction::triggered, this, [this, path] { openFile(path); });
        }
    }
    settings.setValue(QStringLiteral("workspace/recentFiles"), availableFiles);

    if (availableFiles.isEmpty()) {
        auto* emptyAction = recentFilesMenu_->addAction(QStringLiteral("No Recent Files"));
        emptyAction->setEnabled(false);
        return;
    }

    recentFilesMenu_->addSeparator();
    auto* clearAction = recentFilesMenu_->addAction(QStringLiteral("Clear Recent Files"));
    connect(clearAction, &QAction::triggered, this, [this] {
        QSettings().remove(QStringLiteral("workspace/recentFiles"));
        rebuildRecentFilesMenu();
    });
}

void MainWindow::showSettings() { setSettingsWorkspaceVisible(true); }

void MainWindow::setSettingsWorkspaceVisible(const bool visible) {
    if (!visible) {
        if (settingsWorkspace_ != nullptr) {
            settingsWorkspace_->hide();
            settingsWorkspace_->deleteLater();
            settingsWorkspace_ = nullptr;
            settingsDialog_ = nullptr;
        }
        if (settingsWorkspaceMode_)
            theme_.cancelPreview();
        settingsWorkspaceMode_ = false;
        return;
    }

    hideUtilityWorkspaces();
    setRemoteWorkspaceVisible(false);
    setAtlasWorkspaceVisible(false);
    setAgentWorkspaceVisible(false);

    settingsDialog_ = new SettingsDialog(appearanceSettings_, theme_);
    QVariantList archived =
        agentManager_ == nullptr ? QVariantList{} : agentManager_->archivedConversations();
    archived.append(archivedWorktrees());
    auto* archivedPage = new ArchivedSettingsPage(
        archived,
        [this](const QString& path, const QString& id) {
            return id.isEmpty()
                       ? restoreArchivedWorktree(path)
                       : agentManager_ != nullptr && agentManager_->restoreConversation(path, id);
        },
        [this](const QString& path, const QString& id) {
            if (agentManager_ == nullptr)
                return false;
            return id.isEmpty() ? agentManager_->cancelWorktree(path)
                                : agentManager_->cancelConversation(path, id);
        },
        [this](const QString& path, const QString& id) { deleteAgentConversation(path, id); });
    settingsDialog_->addPage(archivedPage);
    // A delete confirmed in its dialog, or an archive from another client, updates the cards.
    if (agentManager_ != nullptr)
        connect(agentManager_, &AgentSessionManager::conversationsChanged, archivedPage,
                [this, archivedPage] {
                    QVariantList current = agentManager_->archivedConversations();
                    current.append(archivedWorktrees());
                    archivedPage->setConversations(std::move(current));
                });
    // Another window may save while this form is open. Each change applies at once, so only
    // the fields this emission changed overwrite; the rest keep the other window's newer values.
    auto lastRequested = std::make_shared<AppearanceSettings>(appearanceSettings_);
    connect(settingsDialog_, &SettingsDialog::appearanceSettingsSaved, this,
            [this, lastRequested](const AppearanceSettings& requested) {
                const AppearanceSettings& previous = *lastRequested;
                auto merged = appearanceSettings_;
                if (requested.interfaceZoomPercent != previous.interfaceZoomPercent)
                    merged.interfaceZoomPercent = requested.interfaceZoomPercent;
                if (requested.interfaceFontSizePixels != previous.interfaceFontSizePixels)
                    merged.interfaceFontSizePixels = requested.interfaceFontSizePixels;
                if (requested.editor.fontFamily != previous.editor.fontFamily)
                    merged.editor.fontFamily = requested.editor.fontFamily;
                if (requested.editor.fontSizePixels != previous.editor.fontSizePixels)
                    merged.editor.fontSizePixels = requested.editor.fontSizePixels;
                if (requested.editor.lineHeightPixels != previous.editor.lineHeightPixels)
                    merged.editor.lineHeightPixels = requested.editor.lineHeightPixels;
                // Terminal and Markdown sizes follow from these when normalized.
                *lastRequested = requested;
                applyAppearanceSettings(merged);
            });

    const QPointer<AgentSessionManager> agents(agentManager_);
    const auto providerRuntimes = [agents](const bool recheck, SettingsProvidersPage::Reply reply) {
        if (agents.isNull() || agents->appServer() == nullptr) {
            reply({}, SettingsProvidersPage::tr("Agents are not available."));
            return;
        }
        agents->appServer()->call(
            QStringLiteral("provider/runtimeStatus"), {{QStringLiteral("recheck"), recheck}},
            [reply = std::move(reply)](const QJsonObject& result, const QString& error) {
                reply(result.value(QStringLiteral("providers")).toArray(), error);
            });
    };
    settingsWorkspace_ = new SettingsWorkspacePage(
        settingsDialog_, providerRuntimes, workspaceSplit_,
        [agents](const QString& operation, const QJsonObject& payload,
                 SettingsClaudeCredentials::Reply reply) {
            if (!agents || !agents->appServer()) {
                reply({}, SettingsProvidersPage::tr("Agents are not available."));
                return;
            }
            agents->appServer()->call(QStringLiteral("provider/claudeCredentials/") + operation,
                                      payload, std::move(reply));
        },
        [agents, sidebar = QPointer<ActivityBar>(activityBar_)](const QString& operation,
                                                                const QJsonObject& payload,
                                                                SettingsStoragePage::Reply reply) {
            if (!agents || !agents->appServer()) {
                reply({}, SettingsStoragePage::tr("Agents are not available."));
                return;
            }
            // The sidebar banner follows what the Storage page reads, so it clears as soon as
            // a compaction or a new limit brings the journal back under it.
            if (operation.startsWith(QStringLiteral("size.")))
                reply = [sidebar, reply = std::move(reply)](const QJsonObject& result,
                                                            const QString& error) {
                    if (sidebar && error.isEmpty() && result.value("ok").toBool())
                        sidebar->setStorageStatus(result.toVariantMap());
                    reply(result, error);
                };
            // "read" is the detail policy; "size.read", "compaction.start" and the like are
            // the other storage areas (ADR 0108).
            const QString area = operation.contains('.') ? operation.section('.', 0, 0)
                                                         : QStringLiteral("retention");
            agents->appServer()->call(
                QStringLiteral("daemon/storage%1%2/%3")
                    .arg(area.left(1).toUpper(), area.mid(1), operation.section('.', -1)),
                payload, std::move(reply));
        },
        // Settings → Teams: the daemon keeps the automatic turns a new team starts with.
        [window = QPointer<MainWindow>(this)](const QString& type, const QJsonObject& payload,
                                              SettingsTeamsPage::Reply reply) {
            if (!window || window->teams_ == nullptr) {
                reply({}, SettingsTeamsPage::tr("Teams need a KetPlus Daemon that supports them."));
                return;
            }
            window->teams_->request(type, payload, std::move(reply));
        });
    workspaceSplit_->addWidget(settingsWorkspace_);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(settingsWorkspace_), 1);
    connect(settingsWorkspace_, &SettingsWorkspacePage::saved, this, [this] {
        auto* previous = settingsWorkspace_;
        settingsWorkspace_ = nullptr;
        settingsDialog_ = nullptr;
        if (previous != nullptr)
            previous->deleteLater();
        QTimer::singleShot(0, this, [this] {
            if (!settingsWorkspaceMode_)
                return;
            settingsWorkspaceMode_ = false;
            setSettingsWorkspaceVisible(true);
        });
    });
    connect(settingsWorkspace_, &SettingsWorkspacePage::dismissed, this,
            [this] { setExplorerVisible(true); });
    connect(settingsWorkspace_, &SettingsWorkspacePage::accountDevicesRequested, this, [this] {
        setSettingsWorkspaceVisible(false);
        setRemoteWorkspaceVisible(true);
    });
    // Account & devices reads the remote.status Connections reads, through its own client;
    // signing out happens in Connections, which asks first.
    auto* accountDaemon = this->accountDaemon(); // shared with the sidebar identity
    settingsWorkspace_->setAccountStatusReader(
        [accountDaemon](std::function<void(const QJsonObject&)> reply) {
            if (!accountDaemon->isReady())
                return; // readyChanged reads it
            accountDaemon->call(
                QStringLiteral("remote.status"), {}, {}, -1,
                [reply = std::move(reply)](const QJsonObject& status, const QString& error) {
                    if (error.isEmpty())
                        reply(status);
                });
        });
    connect(accountDaemon, &remote::DaemonClient::readyChanged, settingsWorkspace_,
            [page = settingsWorkspace_](const bool ready) {
                if (ready)
                    page->refreshAccount();
            });
    connect(settingsWorkspace_, &SettingsWorkspacePage::accountSignOutRequested, this, [this] {
        setSettingsWorkspaceVisible(false);
        setRemoteWorkspaceVisible(true);
        if (remoteControl_ != nullptr)
            remoteControl_->requestSignOut();
    });
    connect(settingsWorkspace_, &SettingsWorkspacePage::typeInTerminalRequested, this,
            &MainWindow::typeInTerminal);
    connect(settingsWorkspace_, &SettingsWorkspacePage::agentModelsRequested, this, [this] {
        setSettingsWorkspaceVisible(false);
        setAgentWorkspaceVisible(true);
        QTimer::singleShot(0, this, [this] {
            if (agentWorkspace_ != nullptr)
                agentWorkspace_->openModelPicker();
        });
    });

    if (explorer_ != nullptr)
        explorer_->hide();
    if (gitChanges_ != nullptr)
        gitChanges_->hide();
    if (terminal_ != nullptr)
        terminal_->hide();
    if (agentWorkspace_ != nullptr)
        agentWorkspace_->hide();
    if (agentDetailPane_ != nullptr)
        agentDetailPane_->hide();
    editorSplit_->hide();
    settingsWorkspaceMode_ = true;
    settingsWorkspace_->show();
    setShellMode(ActivityBar::Mode::Settings);
    workspaceSplit_->show();
    mainSplit_->show();
    mainSplit_->setMaximumHeight(QWIDGETSIZE_MAX);
    mainSplit_->parentWidget()->layout()->activate();
    QList<int> shellSizes(mainSplit_->count(), 0);
    shellSizes[mainSplit_->indexOf(workspaceSplit_)] = qMax(480, mainSplit_->height());
    mainSplit_->setSizes(shellSizes);
    QList<int> sizes(workspaceSplit_->count(), 0);
    sizes[workspaceSplit_->indexOf(settingsWorkspace_)] = qMax(800, workspaceSplit_->width());
    workspaceSplit_->setSizes(sizes);
}

void MainWindow::applyThemeToEditors() {
    applySearchPanelColors();
    auto* activeEditor = currentEditor();
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (editor != nullptr && editor == activeEditor && !editor->isHibernated()) {
            editor->applyTheme(theme_.palette());
        } else if (editor != nullptr) {
            editor->markThemePending();
        }
    }
    for (int index = 0; index < tabs_->count(); ++index) {
        if (auto* diffView = qobject_cast<GitDiffView*>(tabs_->widget(index))) {
            diffView->applyTheme(theme_.palette());
        }
    }
    if (splitEditor_ != nullptr) {
        splitEditor_->applyTheme(theme_.palette());
    }
    if (terminal_ != nullptr) {
        terminal_->applyTheme(theme_.palette());
        applyTerminalTypography(terminal_);
    }
    if (agentDetailTabs_ != nullptr) {
        for (int index = 0; index < agentDetailTabs_->count(); ++index) {
            if (auto* editor = qobject_cast<EditorWidget*>(agentDetailTabs_->widget(index))) {
                if (editor == activeEditor && !editor->isHibernated()) {
                    editor->applyTheme(theme_.palette());
                } else {
                    editor->markThemePending();
                }
            } else if (auto* terminal =
                           qobject_cast<TerminalPanel*>(agentDetailTabs_->widget(index))) {
                terminal->applyTheme(theme_.palette());
                applyTerminalTypography(terminal);
            } else if (auto* diffView =
                           qobject_cast<GitDiffView*>(agentDetailTabs_->widget(index))) {
                diffView->applyTheme(theme_.palette());
            }
        }
    }
    if (agentWorkspace_ != nullptr) {
        agentWorkspace_->applyTheme(theme_.palette(), appearanceSettings_.interfaceZoomPercent);
    }
    if (MachineWorkspaceView* machine = machines_ != nullptr ? machines_->existingView() : nullptr)
        machine->transcript()->view()->applyTheme(theme_.palette(),
                                                  appearanceSettings_.interfaceZoomPercent);
    if (conversationAtlasPanel_ != nullptr)
        conversationAtlasPanel_->applyPalette(QApplication::palette());
    if (atlasTabs_ != nullptr) {
        for (int index = 0; index < atlasTabs_->count(); ++index) {
            if (auto* tab = qobject_cast<AtlasTab*>(atlasTabs_->widget(index))) {
                tab->applyPalette(QApplication::palette(),
                                  appearanceSettings_.interfaceZoomPercent);
            } else if (auto* builder = qobject_cast<AtlasBuilderTab*>(atlasTabs_->widget(index))) {
                builder->applyPalette(QApplication::palette(),
                                      appearanceSettings_.interfaceZoomPercent);
                builder->chatView()->applyTheme(theme_.palette(),
                                                appearanceSettings_.interfaceZoomPercent);
            }
        }
    }
    updateMarkdownPreview();
}

} // namespace ketplus
