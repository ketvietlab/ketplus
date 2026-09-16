#include "app/MainWindow.h"

#include "editor/DefinitionPattern.h"
#include "index/SymbolIndex.h"
#include "editor/EditorWidget.h"
#include "editor/SymbolExtractor.h"
#include "workspace/SearchPanel.h"
#include "ui/Theme.h"
#include "workspace/QuickOpenPopup.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>
#include <QTimer>

#include <functional>

namespace ketplus {
namespace {

constexpr int navigationJumpLines = 10;
constexpr auto fullScanRootsKey = "workspace/fullScanRoots";
constexpr int maximumNavigationHistory = 50;

QString strippedMenuText(QString text) {
    text.remove(QLatin1Char('&'));
    text.remove(QChar(0x2026));
    return text.trimmed();
}

} // namespace

QuickOpenPopup* MainWindow::ensureQuickOpen() {
    if (quickOpen_ == nullptr) {
        quickOpen_ = new QuickOpenPopup(this);
        connect(quickOpen_, &QuickOpenPopup::itemActivated, this,
                &MainWindow::activateQuickOpenItem);
        connect(quickOpen_, &QuickOpenPopup::dismissed, this, [this] {
            if (auto* editor = activeEditor()) {
                editor->setFocus();
            }
        });
    }
    quickOpen_->setStyleSheet(quickOpenStyleSheet());
    return quickOpen_;
}

QString MainWindow::quickOpenStyleSheet() const {
    const auto& palette = theme_.palette();
    return QStringLiteral(
               "QFrame[kvRole=\"quickOpen\"] { background: %1; border: 1px solid %2;"
               " border-radius: 8px; }"
               "QFrame[kvRole=\"quickOpen\"] QLineEdit { background: %3; color: %4;"
               " border: 1px solid %2; border-radius: 6px; padding: 6px 8px; }"
               "QFrame[kvRole=\"quickOpen\"] QListWidget { background: transparent; color: %4;"
               " border: none; outline: 0; }"
               "QFrame[kvRole=\"quickOpen\"] QListWidget::item { padding: 4px 6px;"
               " border-radius: 4px; }"
               "QFrame[kvRole=\"quickOpen\"] QListWidget::item:selected { background: %5;"
               " color: %4; }"
               "QLabel[kvRole=\"quickOpenStatus\"] { color: %6; }")
        .arg(palette.panelBackground, palette.borderStrong, palette.panelSubtle, palette.textMain,
             palette.accentMuted, palette.textMuted);
}

void MainWindow::showCommandPalette() {
    // Refresh dynamic menus and enabled states so the palette mirrors the menu bar.
    rebuildRecentFilesMenu();
    rebuildRecentFoldersMenu();
    rebuildWorktreeMenu();
    updateEditorActions();
    updateGitActions();

    paletteActions_.clear();
    QList<QuickOpenItem> items;
    const std::function<void(QMenu*, const QString&)> collect = [&](QMenu* menu,
                                                                   const QString& path) {
        for (QAction* action : menu->actions()) {
            const QString text = strippedMenuText(action->text());
            if (action->isSeparator() || text.isEmpty() || !action->isVisible() ||
                action == commandPaletteAction_) {
                continue;
            }
            if (QMenu* submenu = action->menu()) {
                collect(submenu, path + text + QStringLiteral(" › "));
                continue;
            }
            if (!action->isEnabled()) {
                continue;
            }
            QString detail = action->shortcut().toString(QKeySequence::NativeText);
            if (action->isCheckable()) {
                const QString state = action->isChecked() ? QStringLiteral("On") : QStringLiteral("Off");
                detail = detail.isEmpty() ? state : QStringLiteral("%1 · %2").arg(state, detail);
            }
            items.append({path + text, detail, static_cast<int>(paletteActions_.size())});
            paletteActions_.append(action);
        }
    };
    for (QAction* topLevel : menuBar()->actions()) {
        if (QMenu* menu = topLevel->menu()) {
            collect(menu, strippedMenuText(topLevel->text()) + QStringLiteral(" › "));
        }
    }

    quickOpenMode_ = QuickOpenMode::Commands;
    ensureQuickOpen()->open(QStringLiteral("Type a command"), items);
}

void MainWindow::showGoToFile() {
    quickOpenMode_ = QuickOpenMode::Files;
    ensureQuickOpen()->open(workspaceRoot_.isEmpty()
                                ? QStringLiteral("Go to an open file")
                                : QStringLiteral("Go to file in %1")
                                      .arg(QFileInfo(workspaceRoot_).fileName()),
                            {});
    updateGoToFileItems();
    refreshWorkspaceFileIndex(false);
}

void MainWindow::updateGoToFileItems() {
    if (quickOpen_ == nullptr || !quickOpen_->isVisible() ||
        quickOpenMode_ != QuickOpenMode::Files) {
        return;
    }

    const QDir root(workspaceRoot_);
    QList<QuickOpenItem> items;
    QStringList openPaths;
    for (int index = 0; index < tabs_->count(); ++index) {
        const auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (editor == nullptr || editor->document().isUntitled()) {
            continue;
        }
        const QString path = editor->document().filePath();
        const bool insideWorkspace =
            !workspaceRoot_.isEmpty() && !root.relativeFilePath(path).startsWith(QStringLiteral(".."));
        items.append({insideWorkspace ? root.relativeFilePath(path) : path,
                      QStringLiteral("open"), path});
        openPaths.append(QDir::cleanPath(path));
    }

    if (!workspaceRoot_.isEmpty() && workspaceFilesRoot_ == workspaceRoot_) {
        for (const QString& relativePath : workspaceFiles_) {
            const QString path = QDir::cleanPath(root.absoluteFilePath(relativePath));
            if (!openPaths.contains(path)) {
                items.append({relativePath, {}, path});
            }
        }
    }
    quickOpen_->setItems(items);

    if (workspaceIndexing_) {
        quickOpen_->setStatusText(QStringLiteral("Indexing workspace files…"));
    } else if (workspaceFilesTruncated_ && workspaceFilesRoot_ == workspaceRoot_) {
        quickOpen_->setStatusText(
            QStringLiteral("Showing the first %L1 files").arg(workspaceFiles_.size()));
    } else {
        quickOpen_->setStatusText({});
    }
}

void MainWindow::refreshWorkspaceFileIndex(const bool force) {
    if (workspaceRoot_.isEmpty() || workspaceIndexing_) {
        return;
    }
    const bool fresh = workspaceFilesRoot_ == workspaceRoot_ &&
                       QDateTime::currentMSecsSinceEpoch() - workspaceFilesIndexedAt_ <
                           workspaceIndexMaxAgeMs;
    if (fresh && !force) {
        return;
    }

    workspaceIndexing_ = true;
    updateGoToFileItems();
    const QString root = workspaceRoot_;
    workspaceIndexCancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = workspaceIndexCancelled_;
    // Walk the tree off the UI thread; large workspaces can take a moment. The destructor
    // cancels and waits, so `this` outlives the worker, and a queued call to a destroyed
    // receiver is discarded.
    const int limit = workspaceFileLimit(root);
    auto* thread = QThread::create([this, root, limit, cancelled] {
        const WorkspaceFileList files = collectWorkspaceFiles(root, limit, cancelled.get());
        if (cancelled->load()) {
            return;
        }
        QMetaObject::invokeMethod(
            this, [this, root, files] { finishWorkspaceFileIndex(root, files); },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    workspaceIndexThread_ = thread;
    thread->start(QThread::LowPriority);
}

MainWindow::~MainWindow() {
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
}

void MainWindow::finishWorkspaceFileIndex(const QString& root, const WorkspaceFileList& files) {
    workspaceIndexing_ = false;
    if (root != workspaceRoot_) {
        refreshWorkspaceFileIndex(true);
        return;
    }
    workspaceFiles_ = files.relativePaths;
    workspaceFilesTruncated_ = files.truncated;
    workspaceFilesRoot_ = root;
    workspaceFilesIndexedAt_ = QDateTime::currentMSecsSinceEpoch();
    updateGoToFileItems();
    if (files.truncated) {
        askAboutFullWorkspaceScan(root);
    }
}

WorkspaceFileList MainWindow::cachedWorkspaceFiles(const QString& root) const {
    const bool fresh = workspaceFilesRoot_ == root && !workspaceIndexing_ &&
                       !workspaceFiles_.isEmpty() &&
                       QDateTime::currentMSecsSinceEpoch() - workspaceFilesIndexedAt_ <
                           workspaceIndexMaxAgeMs;
    if (!fresh) {
        return {};
    }
    return {workspaceFiles_, workspaceFilesTruncated_};
}

int MainWindow::workspaceFileLimit(const QString& root) const {
    const QSettings settings;
    return settings.value(fullScanRootsKey).toStringList().contains(root)
               ? maximumWorkspaceFileLimit
               : defaultWorkspaceFileLimit;
}

void MainWindow::askAboutFullWorkspaceScan(const QString& root) {
    // Asked once per folder per session; the answer to scan it all is remembered for good.
    if (workspaceFileLimit(root) == maximumWorkspaceFileLimit ||
        declinedFullScanRoots_.contains(root)) {
        return;
    }

    const auto answer = QMessageBox::question(
        this, QStringLiteral("Large folder"),
        QStringLiteral("%1 holds more than %L2 files, so Go to File and Search in Files only "
                       "cover the first %L2.\n\nScan the whole folder? It uses more memory and "
                       "takes longer to open, and is remembered for this folder.")
            .arg(QFileInfo(root).fileName())
            .arg(defaultWorkspaceFileLimit),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        declinedFullScanRoots_.append(root);
        return;
    }

    QSettings settings;
    QStringList roots = settings.value(fullScanRootsKey).toStringList();
    roots.removeAll(root);
    roots.append(root);
    settings.setValue(fullScanRootsKey, roots);
    refreshWorkspaceFileIndex(true);
}

void MainWindow::showGoToSymbol() {
    auto* editor = activeEditor();
    if (editor == nullptr || editor->isHibernated()) {
        return;
    }
    if (editor->isLargeFileMode()) {
        statusBar()->showMessage(QStringLiteral("Symbols are not available in large file mode"),
                                 3000);
        return;
    }

    const auto symbols = extractDocumentSymbols(editor->text(), editor->syntaxName());
    if (symbols.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No symbols found in this file"), 3000);
        return;
    }

    QList<QuickOpenItem> items;
    items.reserve(symbols.size());
    for (const auto& symbol : symbols) {
        items.append({QString(symbol.depth * 2, QLatin1Char(' ')) + symbol.name,
                      QStringLiteral("%1 · line %2").arg(symbol.kind).arg(symbol.line),
                      symbol.line});
    }
    symbolEditor_ = editor;
    quickOpenMode_ = QuickOpenMode::Symbols;
    ensureQuickOpen()->open(QStringLiteral("Go to symbol in %1")
                                .arg(editor->document().displayName()),
                            items);
}

void MainWindow::activateQuickOpenItem(const QVariant& data) {
    switch (quickOpenMode_) {
    case QuickOpenMode::Commands: {
        const int index = data.toInt();
        if (auto* editor = activeEditor()) {
            editor->setFocus();
        }
        if (index >= 0 && index < paletteActions_.size()) {
            const QPointer<QAction> action = paletteActions_.at(index);
            if (action != nullptr && action->isEnabled()) {
                action->trigger();
            }
        }
        break;
    }
    case QuickOpenMode::Files: {
        const QString path = data.toString();
        if (auto* editor = activeEditor()) {
            pushNavigationLocation(locationOf(editor));
        }
        restoringNavigation_ = true;
        openFile(path);
        restoringNavigation_ = false;
        if (auto* editor = currentEditor()) {
            lastLocation_ = locationOf(editor);
            editor->setFocus();
        }
        break;
    }
    case QuickOpenMode::Definitions: {
        const QStringList parts = data.toString().split(QLatin1Char('\n'));
        if (parts.size() == 2) {
            openWorkspaceFile(parts.at(0), parts.at(1).toInt());
        }
        break;
    }
    case QuickOpenMode::Symbols: {
        EditorWidget* editor = symbolEditor_;
        if (editor == nullptr || editor->isHibernated()) {
            break;
        }
        pushNavigationLocation(locationOf(editor));
        editor->goToLine(data.toInt());
        editor->setFocus();
        lastLocation_ = locationOf(editor);
        break;
    }
    }
}

void MainWindow::goToDefinition() {
    auto* editor = activeEditor();
    if (editor == nullptr) {
        return;
    }
    const auto position = editor->caretWordPosition();
    resolveDefinition(editor, editor->wordAtPosition(position),
                      editor->fileTokenAtPosition(position));
}

void MainWindow::resolveDefinition(EditorWidget* editor, const QString& symbol,
                                   const QString& fileToken) {
    if (editor == nullptr || editor->isHibernated()) {
        return;
    }
    if (openFileReference(editor, fileToken)) {
        return;
    }
    if (symbol.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Put the cursor on a symbol first"), 3000);
        return;
    }

    // An indexed workspace answers without touching the disk.
    if (resolveDefinitionFromIndex(editor, symbol)) {
        return;
    }

    // A declaration in this file wins: it needs no scan and is the common case.
    if (!editor->isLargeFileMode()) {
        const int currentLine = editor->currentLine();
        for (const auto& candidate : extractDocumentSymbols(editor->text(), editor->syntaxName())) {
            const bool sameName = candidate.name == symbol ||
                                  candidate.name.endsWith(QStringLiteral(".") + symbol) ||
                                  candidate.name.endsWith(QStringLiteral("::") + symbol);
            if (sameName && candidate.line != currentLine) {
                pushNavigationLocation(locationOf(editor));
                editor->goToLine(candidate.line);
                editor->setFocus();
                lastLocation_ = locationOf(editor);
                return;
            }
        }
    }

    if (workspaceRoot_.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("No definition of %1 in this file. Open a folder to search further.")
                .arg(symbol),
            4000);
        return;
    }

    // The scan answers this click; the index makes the next ones instant.
    startSymbolIndex(workspaceRoot_);

    const QString expression = definitionExpression(symbol, editor->syntaxName());
    definitionSymbol_ = symbol;
    definitionFallbackUsed_ = expression.isEmpty();
    WorkspaceSearchOptions options;
    options.query = expression.isEmpty() ? symbol : expression;
    options.regex = !expression.isEmpty();
    options.wholeWord = expression.isEmpty();
    options.matchCase = true;
    startWorkspaceSearch(options, true);
}

void MainWindow::clearSymbolIndex() {
    if (symbolIndex_ == nullptr) {
        statusBar()->showMessage(QStringLiteral("No symbols are indexed"), 3000);
        return;
    }
    const int workspaces = symbolIndex_->workspaceCount();
    const qint64 bytes = symbolIndex_->memoryBytes();
    symbolIndex_->clear();
    statusBar()->showMessage(QStringLiteral("Cleared the index of %L1 %2 (%L3 MB)")
                                 .arg(workspaces)
                                 .arg(workspaces == 1 ? QStringLiteral("folder")
                                                      : QStringLiteral("folders"))
                                 .arg(bytes / (1024 * 1024)),
                             3000);
}

bool MainWindow::resolveDefinitionFromIndex(EditorWidget* editor, const QString& symbol) {
    if (symbolIndex_ == nullptr || workspaceRoot_.isEmpty() ||
        !symbolIndex_->hasWorkspace(workspaceRoot_)) {
        return false;
    }
    const auto hits = symbolIndex_->lookup(workspaceRoot_, symbol);
    if (hits.isEmpty()) {
        return false;
    }

    const QDir root(workspaceRoot_);
    const QString currentPath =
        editor->document().isUntitled() ? QString() : editor->document().filePath();
    if (hits.size() == 1) {
        openWorkspaceFile(hits.constFirst().relativePath, hits.constFirst().line);
        return true;
    }

    // Several declarations: list them, with the ones in this file first.
    QList<QuickOpenItem> items;
    items.reserve(hits.size());
    for (const auto& hit : hits) {
        const QString path = QDir::cleanPath(root.absoluteFilePath(hit.relativePath));
        items.append({hit.relativePath,
                      QStringLiteral("%1 · line %2")
                          .arg(hit.kind.isEmpty() ? QStringLiteral("symbol") : hit.kind)
                          .arg(hit.line),
                      QStringLiteral("%1\n%2").arg(hit.relativePath).arg(hit.line)});
        if (path == currentPath) {
            items.move(items.size() - 1, 0);
        }
    }
    quickOpenMode_ = QuickOpenMode::Definitions;
    ensureQuickOpen()->open(QStringLiteral("Definitions of %1").arg(symbol), items);
    return true;
}

void MainWindow::openWorkspaceFile(const QString& relativePath, const int line) {
    const QString path = QDir::cleanPath(QDir(workspaceRoot_).absoluteFilePath(relativePath));
    if (auto* editor = activeEditor(); editor != nullptr && !editor->isHibernated()) {
        pushNavigationLocation(locationOf(editor));
    }
    restoringNavigation_ = true;
    openFile(path);
    restoringNavigation_ = false;
    if (auto* opened = currentEditor(); opened != nullptr && !opened->isHibernated()) {
        opened->goToLine(line);
        opened->setFocus();
        lastLocation_ = locationOf(opened);
    }
}

void MainWindow::startSymbolIndex(const QString& root) {
    if (root.isEmpty() || symbolIndexThread_ != nullptr) {
        return;
    }
    if (symbolIndex_ == nullptr) {
        symbolIndex_ = std::make_unique<SymbolIndex>();
    }
    if (symbolIndex_->hasWorkspace(root)) {
        return;
    }
    const WorkspaceFileList files = cachedWorkspaceFiles(root);
    if (files.relativePaths.isEmpty()) {
        // The file list is stale or still being built; the next lookup tries again.
        refreshWorkspaceFileIndex(false);
        return;
    }

    symbolIndexRoot_ = root;
    symbolIndexCancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = symbolIndexCancelled_;
    auto* index = symbolIndex_.get();
    // Reading and extracting happen off the UI thread; the destructor cancels and waits.
    auto* thread = QThread::create([this, index, root, files, cancelled] {
        index->indexWorkspace(root, files, cancelled.get());
        QMetaObject::invokeMethod(
            this,
            [this, root, cancelled] {
                symbolIndexThread_ = nullptr;
                symbolIndexRoot_.clear();
                if (!cancelled->load() && symbolIndex_ != nullptr) {
                    statusBar()->showMessage(
                        QStringLiteral("Indexed %L1 files in %2")
                            .arg(symbolIndex_->fileCount(root))
                            .arg(QFileInfo(root).fileName()),
                        3000);
                }
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    symbolIndexThread_ = thread;
    thread->start(QThread::LowPriority);

    if (symbolIndexIdleTimer_ == nullptr) {
        // Workspaces left untouched are dropped even while the budget has room.
        symbolIndexIdleTimer_ = new QTimer(this);
        symbolIndexIdleTimer_->setInterval(60 * 1000);
        connect(symbolIndexIdleTimer_, &QTimer::timeout, this, [this] {
            if (symbolIndex_ != nullptr) {
                symbolIndex_->evictIdle(defaultSymbolIndexIdleMs, workspaceRoot_);
            }
        });
        symbolIndexIdleTimer_->start();
    }
}

bool MainWindow::openFileReference(EditorWidget* editor, const QString& fileToken) {
    if (fileToken.isEmpty() || !looksLikeFileReference(fileToken)) {
        return false;
    }
    QStringList bases;
    if (!editor->document().isUntitled()) {
        bases.append(QFileInfo(editor->document().filePath()).absolutePath());
    }
    if (!workspaceRoot_.isEmpty()) {
        bases.append(workspaceRoot_);
    }
    const QStringList candidates = fileReferenceCandidates(fileToken);
    for (const QString& base : bases) {
        for (const QString& candidate : candidates) {
            const QFileInfo target(QDir(base).absoluteFilePath(candidate));
            if (!target.isFile()) {
                continue;
            }
            pushNavigationLocation(locationOf(editor));
            restoringNavigation_ = true;
            openFile(target.absoluteFilePath());
            restoringNavigation_ = false;
            if (auto* opened = currentEditor()) {
                lastLocation_ = locationOf(opened);
                opened->setFocus();
            }
            return true;
        }
    }
    return false;
}

void MainWindow::finishDefinitionSearch() {
    int matchCount = 0;
    for (const auto& file : definitionResults_) {
        matchCount += static_cast<int>(file.matches.size());
    }
    if (matchCount == 0) {
        // The declaration pattern found nothing, so fall back to every use of the name.
        if (!definitionFallbackUsed_) {
            definitionFallbackUsed_ = true;
            WorkspaceSearchOptions options;
            options.query = definitionSymbol_;
            options.wholeWord = true;
            options.matchCase = true;
            startWorkspaceSearch(options, true);
            return;
        }
        statusBar()->showMessage(
            QStringLiteral("No definition found for %1").arg(definitionSymbol_), 4000);
        return;
    }
    if (matchCount == 1) {
        const auto& file = definitionResults_.constFirst();
        const auto& match = file.matches.constFirst();
        openSearchMatch(file.path, match.line, match.column, match.length);
        return;
    }

    // Several candidates: the user picks one in the search panel.
    setSearchPanelVisible(true);
    auto* panel = ensureSearchPanel();
    panel->clearResults();
    for (const auto& file : definitionResults_) {
        panel->addFileResult(file);
    }
    panel->setSearching(false);
    panel->setStatusText(
        QStringLiteral("%L1 %2 of %3")
            .arg(matchCount)
            .arg(definitionFallbackUsed_ ? QStringLiteral("uses") : QStringLiteral("declarations"))
            .arg(definitionSymbol_));
}

MainWindow::NavigationLocation MainWindow::locationOf(EditorWidget* editor) const {
    if (editor == nullptr) {
        return {};
    }
    // The split view shares its source tab's text but has no file path of its own.
    const EditorWidget* fileOwner =
        editor == splitEditor_ && splitSource_ != nullptr ? splitSource_ : editor;
    return {.editor = editor,
            .filePath = fileOwner->document().isUntitled() ? QString()
                                                           : fileOwner->document().filePath(),
            .position = static_cast<qint64>(editor->caretPosition()),
            .line = editor->currentLine()};
}

bool MainWindow::isLocationAvailable(const NavigationLocation& location) const {
    const EditorWidget* editor = location.editor;
    if (editor != nullptr && tabs_->indexOf(location.editor) >= 0) {
        return true;
    }
    // The split view is reused for other files, so it only matches while it still shows
    // the file the location was recorded in.
    if (editor != nullptr && editor == splitEditor_ && splitSource_ != nullptr &&
        !location.filePath.isEmpty() && splitSource_->document().filePath() == location.filePath) {
        return true;
    }
    return !location.filePath.isEmpty() && QFileInfo(location.filePath).isFile();
}

void MainWindow::trackNavigation(EditorWidget* editor) {
    if (restoringNavigation_ || editor == nullptr || editor->isHibernated()) {
        return;
    }
    const NavigationLocation current = locationOf(editor);
    const bool hasPrevious = lastLocation_.editor != nullptr || !lastLocation_.filePath.isEmpty();
    if (hasPrevious) {
        const bool switchedEditor = lastLocation_.editor != editor;
        // Small caret moves while typing are not history; large jumps and tab switches are.
        const bool jumped = !switchedEditor &&
                            qAbs(current.line - lastLocation_.line) >= navigationJumpLines;
        if (switchedEditor || jumped) {
            pushNavigationLocation(lastLocation_);
        }
    }
    lastLocation_ = current;
}

void MainWindow::pushNavigationLocation(const NavigationLocation& location) {
    if (location.editor == nullptr && location.filePath.isEmpty()) {
        return;
    }
    if (!backLocations_.isEmpty()) {
        const auto& last = backLocations_.constLast();
        if (last.editor == location.editor && last.filePath == location.filePath &&
            last.line == location.line) {
            return;
        }
    }
    backLocations_.append(location);
    while (backLocations_.size() > maximumNavigationHistory) {
        backLocations_.removeFirst();
    }
    forwardLocations_.clear();
    updateNavigationActions();
}

void MainWindow::navigateHistory(const bool back) {
    auto& source = back ? backLocations_ : forwardLocations_;
    auto& destination = back ? forwardLocations_ : backLocations_;
    while (!source.isEmpty()) {
        const NavigationLocation target = source.takeLast();
        if (!isLocationAvailable(target)) {
            continue;
        }
        if (auto* editor = activeEditor(); editor != nullptr && !editor->isHibernated()) {
            destination.append(locationOf(editor));
        }
        restoreLocation(target);
        break;
    }
    updateNavigationActions();
}

void MainWindow::restoreLocation(const NavigationLocation& location) {
    restoringNavigation_ = true;
    EditorWidget* editor = location.editor;
    const bool splitShowsLocation = editor != nullptr && editor == splitEditor_ &&
                                    splitSource_ != nullptr &&
                                    splitSource_->document().filePath() == location.filePath;
    if (splitShowsLocation) {
        splitEditor_->setFocus();
    } else if (editor != nullptr && tabs_->indexOf(editor) >= 0) {
        tabs_->setCurrentWidget(editor);
    } else if (location.filePath.isEmpty()) {
        restoringNavigation_ = false;
        return;
    } else {
        openFile(location.filePath);
        editor = currentEditor();
        if (editor != nullptr &&
            QDir::cleanPath(editor->document().filePath()) != QDir::cleanPath(location.filePath)) {
            editor = nullptr;
        }
    }

    if (editor != nullptr && !editor->isHibernated()) {
        editor->setCaretPosition(static_cast<sptr_t>(location.position));
        editor->setFocus();
        lastLocation_ = locationOf(editor);
    }
    restoringNavigation_ = false;
}

void MainWindow::updateNavigationActions() {
    if (backAction_ != nullptr) {
        backAction_->setEnabled(!backLocations_.isEmpty());
    }
    if (forwardAction_ != nullptr) {
        forwardAction_->setEnabled(!forwardLocations_.isEmpty());
    }
}

} // namespace ketplus
