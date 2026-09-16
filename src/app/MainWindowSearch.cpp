#include "app/MainWindow.h"

#include "editor/EditorWidget.h"
#include "git/GitChangesPanel.h"
#include "git/GitService.h"
#include "workspace/ExplorerPanel.h"
#include "workspace/SearchPanel.h"

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>

namespace ketplus {
namespace {

constexpr int searchPanelWidth = 320;

QString comparablePath(const QString& path) {
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
}

} // namespace

SearchPanel* MainWindow::ensureSearchPanel() {
    if (searchPanel_ != nullptr) {
        return searchPanel_;
    }

    searchPanel_ = new SearchPanel(workspaceSplit_);
    workspaceSplit_->insertWidget(0, searchPanel_);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(searchPanel_), 0);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(editorSplit_), 1);
    connect(searchPanel_, &SearchPanel::searchRequested, this,
            qOverload<>(&MainWindow::startWorkspaceSearch));
    connect(searchPanel_, &SearchPanel::cancelRequested, this, [this] {
        cancelWorkspaceSearch();
        searchPanel_->setSearching(false);
        searchPanel_->setStatusText(QStringLiteral("Search cancelled"));
    });
    connect(searchPanel_, &SearchPanel::replaceAllRequested, this,
            &MainWindow::replaceInWorkspace);
    connect(searchPanel_, &SearchPanel::matchActivated, this, &MainWindow::openSearchMatch);
    connect(searchPanel_, &SearchPanel::hideRequested, this,
            [this] { setSearchPanelVisible(false); });
    return searchPanel_;
}

void MainWindow::setSearchPanelVisible(const bool visible) {
    if (visible) {
        auto* panel = ensureSearchPanel();
        if (!panel->isVisible()) {
            // Closing the search panel returns to whichever panel it replaced.
            panelBeforeSearch_ = explorer_ != nullptr && explorer_->isVisible() ? SidePanel::Explorer
                                 : gitChanges_ != nullptr && gitChanges_->isVisible()
                                     ? SidePanel::SourceControl
                                     : SidePanel::None;
        }
        if (explorer_ != nullptr) {
            explorer_->hide();
        }
        if (gitChanges_ != nullptr) {
            gitChanges_->hide();
        }
        panel->show();
        QList<int> sizes(workspaceSplit_->count(), 0);
        sizes[workspaceSplit_->indexOf(panel)] = searchPanelWidth;
        sizes[workspaceSplit_->indexOf(editorSplit_)] =
            qMax(420, workspaceSplit_->width() - searchPanelWidth);
        workspaceSplit_->setSizes(sizes);
        {
            const QSignalBlocker explorerBlocker(explorerAction_);
            const QSignalBlocker sourceControlBlocker(sourceControlAction_);
            const QSignalBlocker searchBlocker(searchPanelAction_);
            explorerAction_->setChecked(false);
            sourceControlAction_->setChecked(false);
            searchPanelAction_->setChecked(true);
        }
        const auto* editor = activeEditor();
        panel->focusQuery(editor != nullptr ? editor->selectedText() : QString());
        if (workspaceRoot_.isEmpty()) {
            panel->setStatusText(QStringLiteral("Open a folder to search across its files."));
        }
        return;
    }

    if (searchPanel_ != nullptr) {
        searchPanel_->hide();
    }
    {
        const QSignalBlocker blocker(searchPanelAction_);
        searchPanelAction_->setChecked(false);
    }
    const SidePanel restore = panelBeforeSearch_;
    panelBeforeSearch_ = SidePanel::None;
    if (restore == SidePanel::Explorer) {
        setExplorerVisible(true);
        return;
    }
    if (restore == SidePanel::SourceControl) {
        setSourceControlVisible(true);
        return;
    }
    if (auto* editor = activeEditor()) {
        editor->setFocus();
    }
}

void MainWindow::startWorkspaceSearch() {
    startWorkspaceSearch(ensureSearchPanel()->options(), false);
}

void MainWindow::startWorkspaceSearch(const WorkspaceSearchOptions& options,
                                      const bool definitionSearch) {
    auto* panel = ensureSearchPanel();
    if (workspaceRoot_.isEmpty()) {
        panel->setStatusText(QStringLiteral("Open a folder to search across its files."));
        return;
    }

    QString error;
    const QRegularExpression expression = buildSearchExpression(options, &error);
    if (!error.isEmpty()) {
        panel->setStatusText(error);
        return;
    }

    cancelWorkspaceSearch();
    lastSearchOptions_ = options;
    definitionSearch_ = definitionSearch;
    definitionResults_.clear();
    if (definitionSearch) {
        // A definition lookup only opens the panel when the answer is ambiguous.
        statusBar()->showMessage(
            QStringLiteral("Looking for a definition of %1…").arg(definitionSymbol_), 2000);
    } else {
        panel->clearResults();
        panel->setSearching(true);
        panel->setStatusText(QStringLiteral("Searching…"));
    }

    // Unsaved tab contents take precedence over the files on disk.
    QHash<QString, QString> openBuffers;
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (editor != nullptr && !editor->isHibernated() && !editor->document().isUntitled()) {
            openBuffers.insert(comparablePath(editor->document().filePath()),
                               QString::fromUtf8(editor->text()));
        }
    }

    const QString root = workspaceRoot_;
    const quint64 generation = ++searchGeneration_;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    searchCancelled_ = cancelled;
    // The worker reports each file as it is found. cancelWorkspaceSearch() and the
    // destructor wait for it, and stale generations are ignored on arrival.
    const int fileLimit = workspaceFileLimit(root);
    // A recent file list is reused, so a lookup does not walk the tree again.
    const WorkspaceFileList knownFiles = cachedWorkspaceFiles(root);
    auto* thread = QThread::create([this, root, options, expression, openBuffers, cancelled,
                                    generation, fileLimit, knownFiles] {
        const auto onFile = [this, cancelled, generation](const WorkspaceSearchFileResult& file) {
            if (cancelled->load()) {
                return;
            }
            QMetaObject::invokeMethod(
                this,
                [this, generation, file] {
                    if (generation != searchGeneration_) {
                        return;
                    }
                    if (definitionSearch_) {
                        definitionResults_.append(file);
                    } else if (searchPanel_ != nullptr) {
                        searchPanel_->addFileResult(file);
                    }
                },
                Qt::QueuedConnection);
        };
        const WorkspaceSearchSummary summary =
            knownFiles.relativePaths.isEmpty()
                ? searchWorkspace(root, options, expression, openBuffers, cancelled.get(), onFile,
                                  defaultSearchMatchLimit, fileLimit)
                : searchWorkspaceFiles(root, knownFiles, options, expression, openBuffers,
                                       cancelled.get(), onFile);
        const bool wasCancelled = cancelled->load();
        QMetaObject::invokeMethod(
            this,
            [this, generation, summary, wasCancelled] {
                if (generation == searchGeneration_) {
                    finishWorkspaceSearch(summary, wasCancelled);
                }
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    searchThread_ = thread;
    thread->start(QThread::LowPriority);
}

void MainWindow::cancelWorkspaceSearch() {
    if (searchCancelled_ != nullptr) {
        searchCancelled_->store(true);
    }
    if (searchThread_ != nullptr) {
        searchThread_->wait();
    }
    ++searchGeneration_;
}

void MainWindow::finishWorkspaceSearch(const WorkspaceSearchSummary& summary,
                                       const bool cancelled) {
    if (definitionSearch_) {
        definitionSearch_ = false;
        if (!cancelled) {
            finishDefinitionSearch();
        }
        definitionResults_.clear();
        return;
    }
    if (searchPanel_ == nullptr) {
        return;
    }
    searchPanel_->setSearching(false);
    if (cancelled) {
        searchPanel_->setStatusText(QStringLiteral("Search cancelled"));
    } else if (summary.matchCount == 0) {
        searchPanel_->setStatusText(QStringLiteral("No results"));
    } else {
        searchPanel_->setStatusText(
            QStringLiteral("%L1 %2 in %L3 %4%5")
                .arg(summary.matchCount)
                .arg(summary.matchCount == 1 ? QStringLiteral("result") : QStringLiteral("results"))
                .arg(summary.fileCount)
                .arg(summary.fileCount == 1 ? QStringLiteral("file") : QStringLiteral("files"))
                .arg(summary.truncated ? QStringLiteral(" (limit reached)") : QString()));
    }
}

EditorWidget* MainWindow::openEditorForPath(const QString& path) const {
    const QString target = comparablePath(path);
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (editor != nullptr && !editor->document().isUntitled() &&
            comparablePath(editor->document().filePath()) == target) {
            return editor;
        }
    }
    return nullptr;
}

void MainWindow::replaceInWorkspace() {
    auto* panel = ensureSearchPanel();
    const QStringList paths = panel->resultPaths();
    if (paths.isEmpty() || panel->isSearching()) {
        return;
    }
    // Replace with exactly what produced the listed results, never a query edited since.
    const WorkspaceSearchOptions options = lastSearchOptions_;
    if (panel->options() != options) {
        panel->setStatusText(
            QStringLiteral("The search changed. Search again before replacing."));
        return;
    }
    QString error;
    const QRegularExpression expression = buildSearchExpression(options, &error);
    if (!error.isEmpty()) {
        panel->setStatusText(error);
        return;
    }

    const QString replacement = panel->replacement();
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Replace in files"),
        QStringLiteral("Replace %L1 matches in %L2 files with “%3”?\n\n"
                       "Open files change in the editor and can be undone. Other files are "
                       "saved to disk immediately and cannot be undone.")
            .arg(panel->resultMatchCount())
            .arg(paths.size())
            .arg(replacement),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }

    int totalReplacements = 0;
    int changedFiles = 0;
    QStringList failures;
    for (const QString& path : paths) {
        int replacements = 0;
        auto* editor = openEditorForPath(path);
        if (editor != nullptr && !editor->isHibernated()) {
            const QString text = QString::fromUtf8(editor->text());
            const QString replaced =
                replaceInText(text, expression, replacement, options.regex, &replacements);
            if (replacements > 0) {
                editor->replaceAllText(replaced.toUtf8());
            }
        } else {
            // Document keeps the file's encoding and byte order mark when writing back.
            Document document;
            const auto loaded = document.load(path);
            const bool utf8 = document.textEncoding() == TextEncoding::Utf8 ||
                              document.textEncoding() == TextEncoding::Utf8Bom;
            if (!loaded.ok || !utf8) {
                failures.append(QDir(workspaceRoot_).relativeFilePath(path));
                continue;
            }
            const QString replaced = replaceInText(QString::fromUtf8(loaded.content), expression,
                                                   replacement, options.regex, &replacements);
            if (replacements > 0 && !document.save(replaced.toUtf8()).ok) {
                failures.append(QDir(workspaceRoot_).relativeFilePath(path));
                continue;
            }
        }
        if (replacements > 0) {
            totalReplacements += replacements;
            ++changedFiles;
        }
    }

    statusBar()->showMessage(QStringLiteral("Replaced %L1 matches in %L2 files")
                                 .arg(totalReplacements)
                                 .arg(changedFiles),
                             4000);
    if (!failures.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Some files were not changed"),
                             QStringLiteral("These files could not be updated:\n%1")
                                 .arg(failures.join(QLatin1Char('\n'))));
    }
    git_->scheduleRefresh();
    startWorkspaceSearch();
}

void MainWindow::openSearchMatch(const QString& path, const int line, const int column,
                                 const int length) {
    if (auto* editor = activeEditor(); editor != nullptr && !editor->isHibernated()) {
        pushNavigationLocation(locationOf(editor));
    }
    restoringNavigation_ = true;
    openFile(path);
    restoringNavigation_ = false;

    auto* editor = currentEditor();
    if (editor == nullptr || editor->isHibernated() ||
        comparablePath(editor->document().filePath()) != comparablePath(path)) {
        return;
    }
    editor->selectTextRange(line, column, length);
    editor->setFocus();
    lastLocation_ = locationOf(editor);
}

} // namespace ketplus
