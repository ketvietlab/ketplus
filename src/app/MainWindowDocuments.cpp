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
    updateTabTitle(editor);
    updateMarkdownPreview();
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
    menu.addSeparator();
    auto* closeAction = menu.addAction(QStringLiteral("Close"));
    auto* closeOthersAction = menu.addAction(QStringLiteral("Close Other Tabs"));
    auto* closeAllAction = menu.addAction(QStringLiteral("Close All Tabs"));
    closeOthersAction->setEnabled(tabs_->count() > 1);

    QAction* selected = menu.exec(tabs_->tabBar()->mapToGlobal(position));
    if (selected == previewAction) {
        tabs_->setCurrentIndex(index);
        setMarkdownPreviewVisible(previewAction->isChecked());
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
    auto* previewAction = addMarkdownPreviewContextAction(menu, editor);

    QAction* selected = menu.exec(editor->mapToGlobal(position));
    if (selected == previewAction) {
        setMarkdownPreviewVisible(previewAction->isChecked());
    }
}

QAction* MainWindow::addMarkdownPreviewContextAction(QMenu& menu, EditorWidget* editor) {
    auto* action = menu.addAction(QStringLiteral("Markdown Preview"));
    action->setCheckable(true);
    action->setChecked(markdownPreview_ != nullptr && markdownPreview_->isVisible());
    action->setEnabled(editor != nullptr && editor->syntaxName() == QStringLiteral("markdown"));
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
    connect(editor, &EditorWidget::contextMenuRequested, this,
            [this, editor](const QPoint& position) { showEditorContextMenu(editor, position); });
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
        if (editor != activeEditor && !editor->document().isUntitled() &&
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
    const auto* editor = currentEditor();
    findBar_->open(replaceMode, editor == nullptr ? QString() : editor->selectedText());
    highlightTimer_->start();
}

void MainWindow::findNext(const bool backwards) {
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
    for (int index = 0; index < tabs_->count(); ++index) {
        if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index))) {
            editor->clearMatchHighlights();
            editor->clearSearchScope();
        }
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
    const int line = QInputDialog::getInt(
        this, QStringLiteral("Go to Line"),
        QStringLiteral("Line number (1–%1):").arg(editor->lineCount()), editor->currentLine(), 1,
        editor->lineCount(), 1, &accepted);
    if (accepted) {
        editor->goToLine(line);
        editor->setFocus();
    }
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
    if (terminal_ != nullptr) {
        terminal_->applyTheme(theme_.palette());
    }
    updateMarkdownPreview();
}

} // namespace ketplus
