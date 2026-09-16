#include "app/MainWindow.h"

#include "app/SettingsDialog.h"
#include "editor/EditorSplitPane.h"
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
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace ketplus {
namespace {
constexpr int maximumResidentTabCount = 12;
constexpr qsizetype residentTextBudgetBytes = 64 * 1024 * 1024;
constexpr int maximumRecentFolderCount = 10;
constexpr int maximumRecentFileCount = 15;
constexpr int maximumClosedTabCount = 20;

void rememberRecentPath(const QString& key, const QString& path, const int maximumCount) {
    QSettings settings;
    QStringList paths = settings.value(key).toStringList();
    paths.removeAll(path);
    paths.prepend(path);
    while (paths.size() > maximumCount) {
        paths.removeLast();
    }
    settings.setValue(key, paths);
}
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

bool MainWindow::saveAllDocuments() {
    int savedCount = 0;
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (editor == nullptr || !editor->document().isModified()) {
            continue;
        }
        if (editor->document().isUntitled()) {
            // Show which tab the save dialog belongs to.
            tabs_->setCurrentIndex(index);
        }
        if (!saveEditor(editor, false)) {
            return false;
        }
        ++savedCount;
    }
    statusBar()->showMessage(savedCount == 0
                                 ? QStringLiteral("No unsaved changes")
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

bool MainWindow::saveEditor(EditorWidget* editor, const bool choosePath) {
    auto& document = editor->document();
    QString path = document.filePath();
    if (choosePath || document.isUntitled()) {
        path = QFileDialog::getSaveFileName(this, QStringLiteral("Save file"), path);
        if (path.isEmpty()) {
            return false;
        }
    }

    const QByteArray content = editor->text();
    const auto result = document.saveAs(path, content);
    if (!result.ok) {
        QMessageBox::critical(this, QStringLiteral("Unable to save file"), result.error);
        return false;
    }

    editor->markSaved();
    rememberRecentFile(path);
    editor->configureLexerForPath(path);
    editor->applyTheme(theme_.palette());
    if (splitPane_ != nullptr && editor == splitSource_) {
        // Save As may pick a new lexer, so refresh the split view's styles.
        splitPane_->refreshCurrentSource();
    }
    updateTabTitle(editor);
    updateMarkdownPreview();
    updateFormatIndicators();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 2500);
    git_->scheduleRefresh();
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
        if (closedFilePaths_.size() > maximumClosedTabCount) {
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
    menu.addSeparator();
    // The split view edits its source tab's document, so split from that tab.
    auto* source = editor == splitEditor_ && splitSource_ != nullptr ? splitSource_ : editor;
    auto* splitRightAction = menu.addAction(QStringLiteral("Split Right"));
    splitRightAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+\\")));
    auto* splitDownAction = menu.addAction(QStringLiteral("Split Down"));
    splitDownAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+K, Ctrl+\\")));
    if (splitPane_ != nullptr) {
        menu.addAction(closeSplitAction_);
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
    action->setChecked(markdownPreview_ != nullptr && markdownPreview_->isVisible());
    action->setShortcut(markdownPreviewAction_->shortcut());
    return action;
}

void MainWindow::activateAdjacentTab(const int offset) {
    if (tabs_->count() < 2) {
        return;
    }
    const int nextIndex = (tabs_->currentIndex() + offset + tabs_->count()) % tabs_->count();
    tabs_->setCurrentIndex(nextIndex);
}

EditorWidget* MainWindow::currentEditor() const {
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
    editor->setEditorSettings(appearanceSettings_.editor);
    editor->setViewOptions(viewOptions_);
    editor->applyTheme(theme_.palette());
    syncEditorZoom(editor);
    connect(editor, &EditorWidget::contextMenuRequested, this,
            [this, editor](const QPoint& position) { showEditorContextMenu(editor, position); });
    connect(editor, &EditorWidget::definitionRequested, this,
            [this, editor](const QString& symbol, const QString& fileToken,
                           const QString& lineText) {
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
                if (activeEditor() == editor) {
                    cursorPositionLabel_->setText(
                        QStringLiteral("Ln %1, Col %2").arg(line).arg(column));
                    trackNavigation(editor);
                }
            });
    connect(editor, &EditorWidget::editorStateChanged, this, [this, editor] {
        if (currentEditor() == editor) {
            updateEditorActions();
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
    const int index = tabs_->indexOf(editor);
    if (index < 0) {
        return;
    }

    auto title = editor->document().displayName();
    if (editor->document().isModified()) {
        title.append(QStringLiteral(" •"));
    }
    tabs_->setTabText(index, title);
    if (auto* button =
            qobject_cast<QToolButton*>(tabs_->tabBar()->tabButton(index, QTabBar::RightSide))) {
        button->setAccessibleName(QStringLiteral("Close %1").arg(editor->document().displayName()));
    }
}

void MainWindow::updateEditorActions() {
    const auto* editor = activeEditor();
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
    updateFormatIndicators();
    const auto* editor = currentEditor();
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
            QMessageBox::critical(this, QStringLiteral("Unable to restore tab"), result.error);
            return;
        }
        editor->applyTheme(theme_.palette());
    } else if (editor->hasPendingTheme()) {
        editor->applyTheme(theme_.palette());
    }

    editor->markActivated(++tabAccessSequence_);
    editor->setFocus();
    enforceTabResourcePolicy();
    // The Explorer follows the tab, so the file being edited is always the selected one.
    if (explorer_ != nullptr && !editor->document().isUntitled()) {
        explorer_->revealPath(editor->document().filePath());
    }
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
        const bool shownInSplit = splitPane_ != nullptr && splitPane_->containsSource(editor);
        if (editor != activeEditor && !shownInSplit && !editor->document().isUntitled() &&
            !editor->document().isModified()) {
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
    rememberRecentPath(QStringLiteral("workspace/recentFolders"), path, maximumRecentFolderCount);
}

void MainWindow::rememberRecentFile(const QString& path) {
    rememberRecentPath(QStringLiteral("workspace/recentFiles"), path, maximumRecentFileCount);
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

void MainWindow::openFindBar(const bool replaceMode) {
    const auto* editor = activeEditor();
    findBar_->open(replaceMode, editor == nullptr ? QString() : editor->selectedText());
    highlightTimer_->start();
}

void MainWindow::findNext(const bool backwards) {
    auto* editor = activeEditor();
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
    auto* editor = activeEditor();
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
    auto* editor = activeEditor();
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
    auto* editor = activeEditor();
    if (editor == nullptr || !findBar_->isVisible()) {
        return;
    }
    const int count = editor->highlightMatches(findBar_->query(), searchOptions());
    if (updateCount && !findBar_->query().isEmpty()) {
        findBar_->showMatchCount(count, count >= EditorWidget::highlightMatchLimit);
    }
}

void MainWindow::clearMatchHighlights() {
    for (int index = 0; index < tabs_->count(); ++index) {
        if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index))) {
            editor->clearMatchHighlights();
            editor->clearSearchScope();
        }
    }
    if (splitEditor_ != nullptr) {
        splitEditor_->clearMatchHighlights();
        splitEditor_->clearSearchScope();
    }
}

void MainWindow::applyViewOptions(const EditorViewOptions& options) {
    viewOptions_ = options.normalized();
    viewOptions_.save();
    for (int index = 0; index < tabs_->count(); ++index) {
        if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index))) {
            editor->setViewOptions(viewOptions_);
        }
    }
    if (splitEditor_ != nullptr) {
        splitEditor_->setViewOptions(viewOptions_);
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
    auto* editor = activeEditor();
    if (editor == nullptr || editor->isHibernated()) {
        return;
    }
    bool accepted = false;
    const int line = QInputDialog::getInt(
        this, QStringLiteral("Go to Line"),
        QStringLiteral("Line number (1–%1):").arg(editor->lineCount()), editor->currentLine(), 1,
        editor->lineCount(), 1, &accepted);
    if (accepted) {
        pushNavigationLocation(locationOf(editor));
        editor->goToLine(line);
        editor->setFocus();
    }
}

EditorWidget* MainWindow::activeEditor() const {
    if (splitEditor_ != nullptr && splitFocused_ && splitEditor_->isVisible()) {
        return splitEditor_;
    }
    return currentEditor();
}

void MainWindow::openSplit(EditorWidget* source, const Qt::Orientation orientation) {
    if (source == nullptr) {
        return;
    }
    // The active tab is never hibernated, so its document is loaded before sharing.
    tabs_->setCurrentWidget(source);
    if (source->isHibernated()) {
        return;
    }

    const bool created = splitPane_ == nullptr;
    if (created) {
        splitPane_ = new EditorSplitPane(documentSplit_);
        splitEditor_ = splitPane_->editor();
        splitEditor_->setEditorSettings(appearanceSettings_.editor);
        connect(splitPane_, &EditorSplitPane::currentSourceChanged, this,
                [this](EditorWidget* current) {
                    splitSource_ = current;
                    splitEditor_->applyTheme(theme_.palette());
                    updateEditorActions();
                });
        connect(splitPane_, &EditorSplitPane::emptied, this, &MainWindow::closeSplit);
        syncEditorZoom(splitEditor_);
        connect(splitEditor_, &EditorWidget::contextMenuRequested, this,
                [this](const QPoint& position) { showEditorContextMenu(splitEditor_, position); });
        connect(splitEditor_, &EditorWidget::definitionRequested, this,
                [this](const QString& symbol, const QString& fileToken,
                       const QString& lineText) {
                    resolveDefinition(splitEditor_, symbol, fileToken, lineText);
                });
        connect(splitEditor_, &EditorWidget::editorStateChanged, this, [this] {
            if (activeEditor() == splitEditor_) {
                updateEditorActions();
            }
        });
        connect(splitEditor_, &EditorWidget::cursorPositionChanged, this,
                [this](const int line, const int column) {
                    if (activeEditor() == splitEditor_) {
                        cursorPositionLabel_->setText(
                            QStringLiteral("Ln %1, Col %2").arg(line).arg(column));
                        trackNavigation(splitEditor_);
                    }
                });
        documentSplit_->addWidget(splitPane_);
    }

    splitEditor_->setViewOptions(viewOptions_);
    splitPane_->showSource(source);
    splitPane_->show();
    if (created || documentSplit_->orientation() != orientation) {
        documentSplit_->setOrientation(orientation);
        const int extent =
            orientation == Qt::Horizontal ? documentSplit_->width() : documentSplit_->height();
        documentSplit_->setSizes({extent / 2, extent - extent / 2});
    }
    closeSplitAction_->setEnabled(true);
    splitEditor_->setFocus();
}

void MainWindow::closeSplit() {
    if (splitPane_ == nullptr) {
        return;
    }
    // Deleting the view releases its reference to the shared Scintilla document.
    splitPane_->hide();
    splitPane_->deleteLater();
    splitPane_ = nullptr;
    splitEditor_ = nullptr;
    splitSource_ = nullptr;
    splitFocused_ = false;
    closeSplitAction_->setEnabled(false);
    if (auto* editor = currentEditor()) {
        editor->setFocus();
    }
    updateEditorActions();
}

void MainWindow::focusOtherView() {
    if (splitEditor_ == nullptr) {
        return;
    }
    if (activeEditor() == splitEditor_) {
        if (auto* editor = currentEditor()) {
            editor->setFocus();
        }
    } else {
        splitEditor_->setFocus();
    }
}

void MainWindow::setFullScreen(const bool fullScreen) {
    if (fullScreen == isFullScreen()) {
        return;
    }
    if (fullScreen) {
        showFullScreen();
    } else if (maximizedBeforeFullScreen_) {
        showMaximized();
    } else {
        showNormal();
    }
}

void MainWindow::setDistractionFree(const bool enabled) {
    if (enabled != distractionFree_) {
        if (enabled) {
            distractionFreeRestore_ = {
                .explorer = explorerAction_->isChecked(),
                .sourceControl = sourceControlAction_->isChecked(),
                .terminal = terminalAction_->isChecked(),
                .markdownPreview = markdownPreviewAction_->isChecked(),
                .fullScreen = isFullScreen(),
            };
            distractionFree_ = true;
            setExplorerVisible(false);
            setSourceControlVisible(false);
            setTerminalVisible(false);
            setMarkdownPreviewVisible(false);
            statusBar()->hide();
            tabs_->tabBar()->hide();
            setFullScreen(true);
        } else {
            distractionFree_ = false;
            statusBar()->show();
            tabs_->tabBar()->show();
            if (distractionFreeRestore_.explorer) {
                setExplorerVisible(true);
            }
            if (distractionFreeRestore_.sourceControl) {
                setSourceControlVisible(true);
            }
            if (distractionFreeRestore_.terminal) {
                setTerminalVisible(true);
            }
            if (distractionFreeRestore_.markdownPreview) {
                setMarkdownPreviewVisible(true);
            }
            if (!distractionFreeRestore_.fullScreen) {
                setFullScreen(false);
            }
        }
    }

    const QSignalBlocker blocker(distractionFreeAction_);
    distractionFreeAction_->setChecked(distractionFree_);
    if (auto* editor = activeEditor()) {
        editor->setFocus();
    }
}

void MainWindow::changeZoom(const int delta) {
    auto options = viewOptions_;
    options.zoom += delta;
    applyViewOptions(options);
}

void MainWindow::syncEditorZoom(EditorWidget* editor) {
    // Ctrl+wheel zooms one Scintilla view; mirror it to every editor and persist it.
    connect(editor, &ScintillaEditBase::zoom, this, [this](const int zoom) {
        if (zoom != viewOptions_.zoom) {
            auto options = viewOptions_;
            options.zoom = zoom;
            applyViewOptions(options);
        }
    });
}

void MainWindow::showSettings() {
    SettingsDialog dialog(appearanceSettings_, theme_, this);
    connect(&dialog, &SettingsDialog::appearanceSettingsSaved, this,
            &MainWindow::applyAppearanceSettings);
    dialog.exec();
}

void MainWindow::applyAppearanceSettings(const AppearanceSettings& settings) {
    appearanceSettings_ = settings.normalized();
    appearanceSettings_.save();
    theme_.setInterfaceFontSizePixels(appearanceSettings_.interfaceFontSizePixels);

    auto* activeEditor = currentEditor();
    for (int index = 0; index < tabs_->count(); ++index) {
        if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index))) {
            editor->setEditorSettings(appearanceSettings_.editor);
            if (editor == activeEditor && !editor->isHibernated()) {
                editor->applyTheme(theme_.palette());
            } else {
                editor->markThemePending();
            }
        } else if (auto* diffView = qobject_cast<GitDiffView*>(tabs_->widget(index))) {
            diffView->applyEditorSettings(appearanceSettings_.editor);
        }
    }
    if (splitEditor_ != nullptr) {
        splitEditor_->setEditorSettings(appearanceSettings_.editor);
        splitEditor_->applyTheme(theme_.palette());
    }
    if (terminal_ != nullptr) {
        terminal_->setTypography(appearanceSettings_.terminal.fontSizePixels,
                                 appearanceSettings_.terminal.lineHeightPixels);
    }
    if (markdownPreview_ != nullptr) {
        markdownPreview_->setTypography(appearanceSettings_.preview.fontSizePixels,
                                        appearanceSettings_.preview.lineHeightPixels);
    }
    statusBar()->showMessage(QStringLiteral("Appearance settings updated"), 2500);
}

void MainWindow::applyThemeToEditors() {
    auto* activeEditor = currentEditor();
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (editor == activeEditor && !editor->isHibernated()) {
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
    }
    applySearchPanelColors();
    updateMarkdownPreview();
}

} // namespace ketplus
