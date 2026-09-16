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
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QShortcut>
#include <QWindowStateChangeEvent>
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
QString normalizedPath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
}
} // namespace

MainWindow::MainWindow(ThemeManager& theme, QWidget* parent)
    : QMainWindow(parent), theme_(theme), appearanceSettings_(AppearanceSettings::load()),
      viewOptions_(EditorViewOptions::load()),
      mainSplit_(new QSplitter(Qt::Vertical, this)),
      workspaceSplit_(new QSplitter(Qt::Horizontal, mainSplit_)),
      editorSplit_(new QSplitter(Qt::Horizontal, workspaceSplit_)), tabs_(new QTabWidget),
      findBar_(new FindReplaceBar),
      cursorPositionLabel_(new QLabel(QStringLiteral("Ln 1, Col 1"), this)),
      documentStateLabel_(new QLabel(QStringLiteral("New"), this)),
      gitButton_(new QToolButton(this)) {
    theme_.setInterfaceFontSizePixels(appearanceSettings_.interfaceFontSizePixels);
    setWindowTitle(QStringLiteral("KetPlus CM"));
    resize(1100, 720);
    setMinimumSize(680, 420);
    setUnifiedTitleAndToolBarOnMac(true);

    tabs_->setDocumentMode(true);
    tabs_->setMovable(true);
    tabs_->setTabsClosable(true);
    tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* editorPane = new QWidget(editorSplit_);
    auto* centralLayout = new QVBoxLayout(editorPane);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    documentSplit_ = new QSplitter(Qt::Horizontal, editorPane);
    documentSplit_->setChildrenCollapsible(false);
    documentSplit_->setHandleWidth(1);
    documentSplit_->addWidget(tabs_);
    centralLayout->addWidget(documentSplit_, 1);
    centralLayout->addWidget(findBar_);
    editorSplit_->setChildrenCollapsible(false);
    editorSplit_->setHandleWidth(1);
    editorSplit_->addWidget(editorPane);
    workspaceSplit_->setChildrenCollapsible(false);
    workspaceSplit_->setHandleWidth(1);
    workspaceSplit_->addWidget(editorSplit_);
    mainSplit_->setChildrenCollapsible(false);
    mainSplit_->setHandleWidth(1);
    mainSplit_->addWidget(workspaceSplit_);
    mainSplit_->setStretchFactor(0, 1);
    setCentralWidget(mainSplit_);

    git_ = new GitService(this);
    createActions();
    statusBar()->setSizeGripEnabled(false);
    statusBar()->showMessage(QStringLiteral("Ready"));
    gitButton_->setProperty("kvRole", QStringLiteral("statusGit"));
    gitButton_->setPopupMode(QToolButton::InstantPopup);
    // The status bar keeps Git status actions; switching worktrees stays in the Git menu.
    auto* statusGitMenu = new QMenu(this);
    connect(statusGitMenu, &QMenu::aboutToShow, this, [this, statusGitMenu] {
        updateGitActions();
        git_->scheduleRefresh(0);
        statusGitMenu->clear();
        for (QAction* action : gitMenu_->actions()) {
            if (action != worktreeMenu_->menuAction()) {
                statusGitMenu->addAction(action);
            }
        }
        const auto actions = statusGitMenu->actions();
        if (!actions.isEmpty() && actions.last()->isSeparator()) {
            statusGitMenu->removeAction(actions.last());
        }
    });
    gitButton_->setMenu(statusGitMenu);
    gitButton_->setToolTip(QStringLiteral("Git repository"));
    gitButton_->setAccessibleName(QStringLiteral("Git repository"));
    gitButton_->hide();
    statusBar()->addPermanentWidget(gitButton_);
    for (QLabel* label : {documentStateLabel_, cursorPositionLabel_}) {
        label->setProperty("kvRole", QStringLiteral("status"));
        statusBar()->addPermanentWidget(label);
    }
    const auto addStatusButton = [this](QMenu* menu, const QString& accessibleName) {
        auto* button = new QToolButton(this);
        button->setProperty("kvRole", QStringLiteral("statusButton"));
        button->setPopupMode(QToolButton::InstantPopup);
        button->setMenu(menu);
        button->setToolTip(accessibleName);
        button->setAccessibleName(accessibleName);
        statusBar()->addPermanentWidget(button);
        return button;
    };
    encodingButton_ = addStatusButton(encodingMenu_, QStringLiteral("File encoding"));
    lineEndingButton_ = addStatusButton(lineEndingMenu_, QStringLiteral("Line endings"));
    languageButton_ = addStatusButton(languageMenu_, QStringLiteral("Language"));

    connect(tabs_, &QTabWidget::tabCloseRequested, this,
            [this](const int index) { closeTab(index); });
    connect(tabs_->tabBar(), &QTabBar::customContextMenuRequested, this,
            &MainWindow::showTabContextMenu);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        activateCurrentTab();
        rememberActiveFileForWorktree();
        updateEditorActions();
        updateDocumentState();
        updateGitActions();
        updateMarkdownPreview();
        trackNavigation(currentEditor());
        if (findBar_->isVisible() && highlightTimer_ != nullptr) {
            highlightTimer_->start();
        }
    });
    connect(findBar_, &FindReplaceBar::findRequested, this, &MainWindow::findNext);
    connect(findBar_, &FindReplaceBar::replaceRequested, this, &MainWindow::replaceCurrentMatch);
    connect(findBar_, &FindReplaceBar::replaceAllRequested, this, &MainWindow::replaceAllMatches);
    connect(findBar_, &FindReplaceBar::closeRequested, this, [this] {
        highlightTimer_->stop();
        clearMatchHighlights();
        if (auto* editor = currentEditor()) {
            editor->setFocus();
        }
    });

    // Debounce match highlighting so typing a query never rescans on every key.
    highlightTimer_ = new QTimer(this);
    highlightTimer_->setSingleShot(true);
    highlightTimer_->setInterval(200);
    connect(highlightTimer_, &QTimer::timeout, this, [this] { refreshMatchHighlights(); });
    connect(findBar_, &FindReplaceBar::queryChanged, highlightTimer_,
            qOverload<>(&QTimer::start));
    connect(findBar_, &FindReplaceBar::searchOptionsChanged, this, [this] {
        if (auto* editor = activeEditor()) {
            if (findBar_->inSelection()) {
                if (!editor->hasSearchScope()) {
                    editor->setSearchScopeToSelection();
                }
            } else {
                editor->clearSearchScope();
            }
        }
        highlightTimer_->start();
    });

    // Editing commands follow whichever editor view the user focused last.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (now == nullptr) {
            return;
        }
        if (splitPane_ != nullptr && (now == splitPane_ || splitPane_->isAncestorOf(now))) {
            splitFocused_ = true;
            updateEditorActions();
        } else if (now == tabs_ || tabs_->isAncestorOf(now)) {
            splitFocused_ = false;
            updateEditorActions();
        }
    });
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this,
            &MainWindow::updateEditorActions);

    connect(&theme_, &ThemeManager::themeChanged, this, &MainWindow::applyThemeToEditors);
    connect(git_, &GitService::snapshotChanged, this, &MainWindow::updateGitSnapshot);
    connect(git_, &GitService::worktreesChanged, this, &MainWindow::updateWorktrees);
    connect(git_, &GitService::diffReady, this,
            [this](const QString& path, const GitDiffMode mode, const QString& diff,
                   const QString& error) {
                if (!error.isEmpty()) {
                    QMessageBox::warning(this, QStringLiteral("Unable to show Git diff"), error);
                } else if (diff.isEmpty()) {
                    statusBar()->showMessage(QStringLiteral("No changes for this file."), 3000);
                } else {
                    showDiff(path, mode, diff);
                }
            });
    connect(git_, &GitService::errorOccurred, this, [this](const QString& error) {
        if (!error.isEmpty()) {
            statusBar()->showMessage(error, 4000);
        }
    });
    connect(qApp, &QApplication::applicationStateChanged, this,
            [this](const Qt::ApplicationState state) {
                if (state == Qt::ApplicationActive) {
                    git_->scheduleRefresh(0);
                }
            });

    createNewDocument();
    updateEditorActions();
}

void MainWindow::openFile(const QString& filePath) {
    const QString absolutePath = normalizedPath(filePath);
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* existing = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (existing != nullptr && !existing->document().isUntitled() &&
            normalizedPath(existing->document().filePath()) == absolutePath) {
            tabs_->setCurrentIndex(index);
            return;
        }
    }

    EditorWidget* initialEditor = nullptr;
    if (tabs_->count() == 1) {
        auto* candidate = qobject_cast<EditorWidget*>(tabs_->widget(0));
        if (candidate != nullptr && candidate->document().isUntitled() &&
            !candidate->document().isModified()) {
            initialEditor = candidate;
        }
    }

    auto* editor = createEditor();
    const auto result = editor->loadFile(absolutePath);
    if (!result.ok) {
        QMessageBox::critical(this, QStringLiteral("Unable to open file"), result.error);
        editor->deleteLater();
        return;
    }

    if (initialEditor != nullptr) {
        tabs_->removeTab(tabs_->indexOf(initialEditor));
        initialEditor->deleteLater();
    }

    editor->applyTheme(theme_.palette());
    const int index = tabs_->addTab(editor, editor->document().displayName());
    configureTabCloseButton(index, editor);
    tabs_->setCurrentIndex(index);
    rememberRecentFile(absolutePath);
    if (editor->isLargeFileMode()) {
        statusBar()->showMessage(
            QStringLiteral("Large file mode: syntax highlighting and undo are disabled"), 5000);
    }
}

void MainWindow::openFolder(const QString& folderPath) {
    const QFileInfo folder(folderPath);
    if (!folder.isDir()) {
        QMessageBox::critical(this, QStringLiteral("Unable to open folder"),
                              QStringLiteral("The selected path is not a folder."));
        return;
    }

    rememberActiveFileForWorktree();
    workspaceRoot_ = normalizedPath(folder.absoluteFilePath());
    rememberRecentFolder(workspaceRoot_);
    workspaceFilesIndexedAt_ = 0;
    auto* explorer = ensureExplorer();
    explorer->setRootPath(workspaceRoot_);
    setExplorerVisible(true);
    setWindowTitle(QStringLiteral("%1 — KetPlus CM").arg(folder.fileName()));
    statusBar()->showMessage(QStringLiteral("Opened %1").arg(workspaceRoot_), 2500);
    git_->setWorkspacePath(workspaceRoot_);
    // Listing the tree now means the first lookup does not wait for it, and a folder too
    // large for the default limit asks about a full scan while the user is still here.
    refreshWorkspaceFileIndex(true);
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange || fullScreenAction_ == nullptr) {
        return;
    }
    const auto previousState = static_cast<QWindowStateChangeEvent*>(event)->oldState();
    if (isFullScreen() && !previousState.testFlag(Qt::WindowFullScreen)) {
        // Entering full screen clears the maximized flag, so remember it for the way back.
        maximizedBeforeFullScreen_ = previousState.testFlag(Qt::WindowMaximized);
    }
    {
        const QSignalBlocker blocker(fullScreenAction_);
        fullScreenAction_->setChecked(isFullScreen());
    }
    exitFullScreenShortcut_->setEnabled(isFullScreen());
    if (!isFullScreen() && distractionFree_) {
        setDistractionFree(false);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (editor != nullptr && !maybeCloseEditor(editor)) {
            event->ignore();
            return;
        }
    }
    event->accept();
}

} // namespace ketplus
