#include "app/MainWindow.h"

#include "editor/DefinitionPattern.h"
#include "editor/EditorWidget.h"
#include "editor/SymbolExtractor.h"
#include "index/SymbolIndex.h"
#include "ui/Theme.h"
#include "workspace/QuickOpenPopup.h"
#include "workspace/SearchPanel.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QStatusBar>
#include <QThread>
#include <QTimer>

namespace ketplus {
namespace {

constexpr auto fullScanRootsKey = "workspace/fullScanRoots";

} // namespace

WorkspaceFileList MainWindow::cachedWorkspaceFiles(const QString& root) const {
    const bool fresh = workspaceFilesRoot_ == root && !workspaceIndexing_ &&
                       !workspaceFiles_.isEmpty() &&
                       QDateTime::currentMSecsSinceEpoch() - workspaceFilesIndexedAt_ <
                           workspaceFileListMaxAgeMs;
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
        QStringLiteral("%1 holds more than %L2 files, so Spotlight and Search in Files only "
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

void MainWindow::applySearchPanelColors() {
    if (searchPanel_ == nullptr) {
        return;
    }
    const auto& palette = theme_.palette();
    // The folder reads quietly, the file name carries the row, and the matched text uses
    // the color the editor highlights matches with.
    searchPanel_->setResultColors(palette.textMuted, palette.textMain, palette.textMuted,
                                  palette.textSecondary, palette.warning);
}

void MainWindow::goToDefinition() {
    auto* editor = currentEditor();
    if (editor == nullptr) {
        return;
    }
    const auto position = editor->caretWordPosition();
    resolveDefinition(editor, editor->wordAtPosition(position),
                      editor->fileTokenAtPosition(position),
                      editor->lineTextAtPosition(position));
}

void MainWindow::resolveDefinition(EditorWidget* editor, const QString& symbol,
                                   const QString& fileToken, const QString& lineText) {
    if (editor == nullptr || editor->isHibernated()) {
        return;
    }
    if (openFileReference(editor, fileToken)) {
        return;
    }
    // A name brought in by an import is declared in that module, not in this folder at large.
    if (openImportedSymbol(editor, symbol, lineText)) {
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

    // One pass looks for the name itself, which is far cheaper than running a declaration
    // pattern over every line; the lines that come back are classified afterwards.
    definitionSymbol_ = symbol;
    definitionPattern_ = definitionExpression(symbol, editor->syntaxName());
    WorkspaceSearchOptions options;
    options.query = symbol;
    options.wholeWord = true;
    options.matchCase = true;
    startWorkspaceSearch(options, true);
}

QString MainWindow::resolveFileReference(EditorWidget* editor, const QString& token) const {
    if (token.isEmpty() || !looksLikeFileReference(token)) {
        return {};
    }
    QStringList bases;
    if (editor != nullptr && !editor->document().isUntitled()) {
        bases.append(QFileInfo(editor->document().filePath()).absolutePath());
    }
    if (!workspaceRoot_.isEmpty()) {
        bases.append(workspaceRoot_);
    }
    for (const QString& base : bases) {
        for (const QString& candidate : fileReferenceCandidates(token)) {
            const QFileInfo target(QDir(base).absoluteFilePath(candidate));
            if (target.isFile()) {
                return target.absoluteFilePath();
            }
        }
    }
    return {};
}

bool MainWindow::openFileReference(EditorWidget* editor, const QString& fileToken) {
    const QString path = resolveFileReference(editor, fileToken);
    if (path.isEmpty()) {
        return false;
    }
    pushNavigationLocation(locationOf(editor));
    restoringNavigation_ = true;
    openFile(path);
    restoringNavigation_ = false;
    if (auto* opened = currentEditor()) {
        lastLocation_ = locationOf(opened);
        opened->setFocus();
    }
    return true;
}

bool MainWindow::openImportedSymbol(EditorWidget* editor, const QString& symbol,
                                    const QString& lineText) {
    if (symbol.isEmpty() || !lineText.contains(symbol)) {
        return false;
    }
    const QString path = resolveFileReference(editor, importedModuleOnLine(lineText));
    if (path.isEmpty() || (!editor->document().isUntitled() &&
                           QFileInfo(path) == QFileInfo(editor->document().filePath()))) {
        return false;
    }

    pushNavigationLocation(locationOf(editor));
    restoringNavigation_ = true;
    openFile(path);
    restoringNavigation_ = false;
    auto* opened = currentEditor();
    if (opened == nullptr || opened->isHibernated()) {
        return true;
    }

    // Land on the declaration inside the module rather than at its first line.
    const QRegularExpression pattern(definitionExpression(symbol, opened->syntaxName()));
    const QStringList lines = QString::fromUtf8(opened->text()).split(QLatin1Char('\n'));
    for (int index = 0; index < lines.size(); ++index) {
        if (pattern.isValid() && !pattern.pattern().isEmpty() &&
            pattern.match(lines.at(index)).hasMatch()) {
            opened->goToLine(index + 1);
            break;
        }
    }
    opened->setFocus();
    lastLocation_ = locationOf(opened);
    return true;
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
    if (auto* editor = currentEditor(); editor != nullptr && !editor->isHibernated()) {
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

void MainWindow::finishDefinitionSearch() {
    int useCount = 0;
    for (const auto& file : definitionResults_) {
        useCount += static_cast<int>(file.matches.size());
    }
    if (useCount == 0) {
        statusBar()->showMessage(
            QStringLiteral("%1 is not used anywhere in this folder").arg(definitionSymbol_), 4000);
        return;
    }

    // Keep the lines that look like a declaration rather than a use of the name.
    QList<WorkspaceSearchFileResult> declarations;
    int declarationCount = 0;
    if (!definitionPattern_.isEmpty()) {
        const QRegularExpression pattern(definitionPattern_);
        if (pattern.isValid()) {
            for (const auto& file : definitionResults_) {
                WorkspaceSearchFileResult kept{file.path, file.relativePath, {}};
                for (const auto& match : file.matches) {
                    if (pattern.match(match.preview).hasMatch()) {
                        kept.matches.append(match);
                    }
                }
                if (!kept.matches.isEmpty()) {
                    declarationCount += static_cast<int>(kept.matches.size());
                    declarations.append(kept);
                }
            }
        }
    }

    if (declarationCount == 1) {
        const auto& file = declarations.constFirst();
        const auto& match = file.matches.constFirst();
        openSearchMatch(file.path, match.line, match.column, match.length);
        return;
    }

    // Several declarations, or none to tell apart: the user picks from the search panel.
    const bool showDeclarations = declarationCount > 1;
    setSearchPanelVisible(true);
    auto* panel = ensureSearchPanel();
    panel->setOptions(lastSearchOptions_);
    panel->clearResults();
    for (const auto& file : showDeclarations ? declarations : definitionResults_) {
        panel->addFileResult(file);
    }
    panel->setSearching(false);
    panel->setStatusText(showDeclarations ? QStringLiteral("%L1 declarations of %2")
                                                .arg(declarationCount)
                                                .arg(definitionSymbol_)
                                          : QStringLiteral("No declaration of %1 here; %L2 uses")
                                                .arg(definitionSymbol_)
                                                .arg(useCount));
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
                if (!cancelled->load() && symbolIndex_ != nullptr) {
                    statusBar()->showMessage(QStringLiteral("Indexed %L1 files in %2")
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

void MainWindow::clearSymbolIndex() {
    if (symbolIndex_ == nullptr) {
        statusBar()->showMessage(QStringLiteral("No symbols are indexed"), 3000);
        return;
    }
    // A build in flight would put its workspace straight back, so it is stopped first.
    if (symbolIndexCancelled_ != nullptr) {
        symbolIndexCancelled_->store(true);
    }
    if (symbolIndexThread_ != nullptr) {
        symbolIndexThread_->wait();
    }
    const int workspaces = symbolIndex_->workspaceCount();
    const qint64 bytes = symbolIndex_->memoryBytes();
    symbolIndex_->clear();
    statusBar()->showMessage(
        QStringLiteral("Cleared the index of %L1 %2 (%L3 MB)")
            .arg(workspaces)
            .arg(workspaces == 1 ? QStringLiteral("folder") : QStringLiteral("folders"))
            .arg(bytes / (1024 * 1024)),
        3000);
}

} // namespace ketplus
