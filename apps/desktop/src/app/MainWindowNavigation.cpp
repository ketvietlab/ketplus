#include "app/MainWindow.h"
#include "app/SpotlightDialog.h"

#include "editor/EditorWidget.h"
#include "editor/SymbolExtractor.h"
#include "ui/Theme.h"
#include "workspace/QuickOpenPopup.h"

#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>

#include <functional>

namespace ketplus {
namespace {

constexpr int navigationJumpLines = 10;
constexpr int maximumNavigationHistory = 50;
constexpr qint64 workspaceIndexMaxAgeMs = 30 * 1000;

QString strippedMenuText(QString text) {
    text.remove(QLatin1Char('&'));
    text.remove(QChar(0x2026));
    return text.trimmed();
}

} // namespace

void MainWindow::showFileSpotlight() {
    // Spotlight lists files first and has no scope prefixes, so Go to File only has to
    // make sure it is open rather than toggling it closed.
    if (findChild<SpotlightDialog*>(QStringLiteral("spotlightDialog")) == nullptr) {
        showSpotlight();
    }
}

void MainWindow::refreshWorkspaceFileIndex(const bool force) {
    const auto root = fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_;
    if (root.isEmpty() || workspaceIndexing_) {
        return;
    }
    const bool fresh =
        workspaceFilesRoot_ == root &&
        QDateTime::currentMSecsSinceEpoch() - workspaceFilesIndexedAt_ < workspaceIndexMaxAgeMs;
    if (fresh && !force) {
        return;
    }

    workspaceIndexing_ = true;
    workspaceIndexCancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = workspaceIndexCancelled_;
    // Only paths are listed, on demand and off the UI thread; file contents are never
    // indexed. The destructor cancels and joins the worker before `this` goes away.
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

void MainWindow::finishWorkspaceFileIndex(const QString& root, const WorkspaceFileList& files) {
    workspaceIndexing_ = false;
    if (root != (fileContextRoot_.isEmpty() ? workspaceRoot_ : fileContextRoot_)) {
        refreshWorkspaceFileIndex(true);
        return;
    }
    workspaceFiles_ = files.relativePaths;
    workspaceFilesRoot_ = root;
    workspaceFilesTruncated_ = files.truncated;
    workspaceFilesIndexedAt_ = QDateTime::currentMSecsSinceEpoch();
    populateSpotlight();
    if (files.truncated) {
        askAboutFullWorkspaceScan(root);
    }
}

QuickOpenPopup* MainWindow::ensureQuickOpen() {
    if (quickOpen_ == nullptr) {
        quickOpen_ = new QuickOpenPopup(this);
        connect(quickOpen_, &QuickOpenPopup::itemActivated, this,
                &MainWindow::activateQuickOpenItem);
        connect(quickOpen_, &QuickOpenPopup::dismissed, this, [this] {
            if (auto* editor = currentEditor()) {
                editor->setFocus();
            }
        });
    }
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
                const QString state =
                    action->isChecked() ? QStringLiteral("On") : QStringLiteral("Off");
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

void MainWindow::showGoToSymbol() {
    auto* editor = currentEditor();
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
    ensureQuickOpen()->open(
        QStringLiteral("Go to symbol in %1").arg(editor->document().displayName()), items);
}

void MainWindow::activateQuickOpenItem(const QVariant& data) {
    switch (quickOpenMode_) {
    case QuickOpenMode::Commands: {
        const int index = data.toInt();
        if (auto* editor = currentEditor()) {
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

MainWindow::NavigationLocation MainWindow::locationOf(EditorWidget* editor) const {
    if (editor == nullptr) {
        return {};
    }
    // The split view shares its source tab's text but has no file path of its own.
    const EditorWidget* fileOwner =
        editor == splitEditor_ && splitSource_ != nullptr ? splitSource_ : editor;
    return {.editor = editor,
            .filePath =
                fileOwner->document().isUntitled() ? QString() : fileOwner->document().filePath(),
            .position = static_cast<qint64>(editor->caretPosition()),
            .line = editor->currentLine()};
}

bool MainWindow::isLocationAvailable(const NavigationLocation& location) const {
    const EditorWidget* editor = location.editor;
    if (editor != nullptr &&
        (tabs_->indexOf(location.editor) >= 0 ||
         (agentDetailTabs_ != nullptr && agentDetailTabs_->indexOf(location.editor) >= 0))) {
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
        const bool jumped =
            !switchedEditor && qAbs(current.line - lastLocation_.line) >= navigationJumpLines;
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
        if (auto* editor = currentEditor(); editor != nullptr && !editor->isHibernated()) {
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
    const bool inEditorTabs = editor != nullptr && tabs_->indexOf(editor) >= 0;
    const bool inAgentTabs =
        editor != nullptr && agentDetailTabs_ != nullptr && agentDetailTabs_->indexOf(editor) >= 0;
    const bool splitShowsLocation = editor != nullptr && editor == splitEditor_ &&
                                    splitSource_ != nullptr && !agentWorkspaceMode_ &&
                                    !atlasWorkspaceMode_ &&
                                    splitSource_->document().filePath() == location.filePath;
    if (splitShowsLocation) {
        splitEditor_->setFocus();
        splitFocused_ = true;
    } else if (inEditorTabs && !agentWorkspaceMode_ && !atlasWorkspaceMode_) {
        tabs_->setCurrentWidget(editor);
    } else if (inAgentTabs && agentWorkspaceMode_) {
        agentDetailTabs_->setCurrentWidget(editor);
    } else if (!location.filePath.isEmpty()) {
        // The editor is closed or belongs to a hidden host; openFile picks the right one.
        openFile(location.filePath);
        editor = currentEditor();
        if (editor != nullptr &&
            QDir::cleanPath(editor->document().filePath()) != QDir::cleanPath(location.filePath)) {
            editor = nullptr;
        }
    } else {
        editor = nullptr;
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
