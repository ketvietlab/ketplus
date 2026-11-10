#include "app/MainWindow.h"
#include "KetAppearance.h"
#include "KetChrome.h"
#include "KetControlRecipes.h"
#include "KetTokens.h"

#include "editor/DiskChangePrompt.h"
#include "editor/DocumentFormatControls.h"

#include "app/SettingsDialog.h"
#include "editor/EditorSplitPane.h"
#include "editor/EditorWidget.h"
#include "git/GitChangesPanel.h"
#include "git/GitDiffView.h"
#include "git/GitService.h"
#include "index/SymbolIndex.h"
#include "preview/MarkdownPreviewPane.h"
#include "terminal/TerminalPanel.h"
#include "ui/FindReplaceBar.h"
#include "ui/Theme.h"
#include "workspace/ExplorerPanel.h"

#include "ui/MessageDialog.h"
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
#include <QPainter>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindowStateChangeEvent>

#include <algorithm>

namespace ketplus {
namespace {
// Keep QMainWindow/QStatusBar message and visibility APIs while the kit owns
// the painted bar. Temporary messages update the left item; permanent actions
// never disappear behind the QStatusBar message overlay.
class StatusBarHost final : public QStatusBar {
  public:
    explicit StatusBarHost(QWidget* parent)
        : QStatusBar(parent), content(new ket::StatusBar(this)) {
        setSizeGripEnabled(false);
        setObjectName(QStringLiteral("kitStatusBarHost"));
        setStyleSheet(
            QStringLiteral("QStatusBar#kitStatusBarHost { min-height:0; border:0; padding:0; }"));
        auto* message = content->addItem(tr("Ready"));
        connect(this, &QStatusBar::messageChanged, message, [message](const QString& text) {
            message->setText(text.isEmpty() ? tr("Ready") : text);
        });
        // QStatusBar's own layout is taller than the kit bar; keep the host at the bar's
        // height so the bar is not drawn at the top of a taller strip.
        const auto fit = [this] { setFixedHeight(content->maximumHeight()); };
        connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this, fit);
        fit();
    }
    QSize sizeHint() const override { return content->sizeHint(); }
    QSize minimumSizeHint() const override { return content->minimumSizeHint(); }
    ket::StatusBar* content;

  protected:
    void paintEvent(QPaintEvent*) override {}
    void resizeEvent(QResizeEvent* event) override {
        QStatusBar::resizeEvent(event);
        content->setGeometry(rect());
    }
};

// A status bar entry with a menu (Git, encoding, line endings, language), drawn as a
// ket::StatusItem that fills on hover.
class StatusButton final : public QToolButton {
  public:
    explicit StatusButton(QWidget* parent) : QToolButton(parent) {
        setPopupMode(QToolButton::InstantPopup);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        // It paints itself; the app's QToolButton rule (min-height: 32px) would overflow the bar.
        setStyleSheet(QStringLiteral("QToolButton { min-height: 0; border: 0; padding: 0; }"));
    }
    QSize sizeHint() const override {
        auto& kit = ket::Appearance::instance();
        const auto recipe = ket::statusBarRecipe(kit.theme());
        // The bar's height less its 1 px top hairline, so the bar centres the button.
        return {QFontMetrics(textFont()).horizontalAdvance(text()) + qRound(kit.scaled(12)),
                qRound(kit.scaled(recipe.height)) - 1};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    void paintEvent(QPaintEvent*) override {
        auto& kit = ket::Appearance::instance();
        const auto recipe = ket::statusBarRecipe(kit.theme());
        QPainter painter(this);
        if (underMouse() || isDown())
            painter.fillRect(rect(), ket::Tokens::color(u"surface-hover", kit.theme()));
        painter.setFont(textFont());
        painter.setPen(isEnabled() ? recipe.text
                                   : ket::Tokens::color(u"text-disabled", kit.theme()));
        painter.drawText(rect(), Qt::AlignCenter, text());
    }

  private:
    QFont textFont() const {
        QFont value = font();
        value.setPixelSize(qRound(ket::Appearance::instance().scaled(
            ket::statusBarRecipe(ket::Appearance::instance().theme()).fontPixels)));
        return value;
    }
};

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
      viewOptions_(EditorViewOptions::load()), mainSplit_(new QSplitter(Qt::Vertical)),
      workspaceSplit_(new QSplitter(Qt::Horizontal, this)),
      editorSplit_(new QSplitter(Qt::Horizontal, mainSplit_)), tabs_(new QTabWidget),
      findBar_(new FindReplaceBar),
      cursorPositionLabel_(new ket::StatusItem(QStringLiteral("Ln 1, Col 1"), {}, this)),
      documentStateLabel_(new ket::StatusItem(QStringLiteral("New"), {}, this)),
      gitButton_(new StatusButton(this)) {
    theme_.setInterfaceScale(appearanceSettings_.interfaceFontSizePixels,
                             appearanceSettings_.interfaceZoomPercent);
    setWindowTitle(QStringLiteral("KétPlus"));
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
    // As the mock: the side panel, then the editor column with the terminal below the
    // editor; one status bar spans the window.
    mainSplit_->setChildrenCollapsible(false);
    mainSplit_->setHandleWidth(1);
    mainSplit_->addWidget(editorSplit_);
    mainSplit_->setStretchFactor(0, 1);
    editorColumn_ = new QWidget(workspaceSplit_);
    editorColumn_->setObjectName(QStringLiteral("editorColumn"));
    auto* columnLayout = new QVBoxLayout(editorColumn_);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(0);
    columnLayout->addWidget(mainSplit_, 1);
    workspaceSplit_->setChildrenCollapsible(false);
    workspaceSplit_->setHandleWidth(1);
    workspaceSplit_->addWidget(editorColumn_);

    git_ = new GitService(this);
    createActions();
    createChrome();
    auto* statusHost = new StatusBarHost(this);
    setStatusBar(statusHost);
    auto* status = statusHost->content;
    statusBar()->showMessage(QStringLiteral("Ready"));
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
    // The Lite mock's single bar: the branch with the messages on the left, then the
    // document's state, position, language, encoding and line endings.
    status->addWidget(gitButton_, ket::StatusBar::Side::Left);
    for (auto* item : {documentStateLabel_, cursorPositionLabel_})
        status->addWidget(item);
    const auto addStatusButton = [this, status](QMenu* menu, const QString& accessibleName) {
        QToolButton* button = new StatusButton(this);
        button->setMenu(menu);
        button->setToolTip(accessibleName);
        button->setAccessibleName(accessibleName);
        status->addWidget(button);
        return button;
    };
    format_->addStatusButtons(addStatusButton);

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
    connect(findBar_, &FindReplaceBar::queryChanged, highlightTimer_, qOverload<>(&QTimer::start));
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
                    // Routine (outside the repository, git stderr): no modal dialog.
                    statusBar()->showMessage(
                        QStringLiteral("Unable to show Git diff: %1").arg(error), 6000);
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
                    disk_change::refreshCleanTabs(tabs_);
                    git_->scheduleRefresh(0);
                }
            });

    updateEditorActions();
}

void MainWindow::openFile(const QString& filePath) {
    const QString absolutePath = normalizedPath(filePath);
    for (int index = 0; index < tabs_->count(); ++index) {
        auto* existing = qobject_cast<EditorWidget*>(tabs_->widget(index));
        if (existing != nullptr && !existing->document().isUntitled() &&
            normalizedPath(existing->document().filePath()) == absolutePath) {
            const auto refreshed = existing->refreshFromDisk();
            if (!refreshed.ok)
                statusBar()->showMessage(
                    QStringLiteral("Unable to refresh file: %1").arg(refreshed.error), 5000);
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
        message_dialog::critical(this, QStringLiteral("Unable to open file"), result.error);
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
    // The breadcrumb still names the untitled document createEditor() started with.
    updateTabTitle(editor);
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
        message_dialog::critical(this, QStringLiteral("Unable to open folder"),
                                 QStringLiteral("The selected path is not a folder."));
        return;
    }

    showEditorSurface();
    rememberActiveFileForWorktree();
    workspaceRoot_ = normalizedPath(folder.absoluteFilePath());
    rememberRecentFolder(workspaceRoot_);
    workspaceFilesIndexedAt_ = 0;
    auto* explorer = ensureExplorer();
    explorer->setRootPath(workspaceRoot_);
    // As the Lite mock: the side panel stays as it was, closed in a new window; the
    // activity bar or Cmd+B opens it.
    if (!explorerAction_->isChecked())
        explorer->hide();
    setWindowTitle(QStringLiteral("%1 — KétPlus").arg(folder.fileName()));
    statusBar()->showMessage(QStringLiteral("Opened %1").arg(workspaceRoot_), 2500);
    git_->setWorkspacePath(workspaceRoot_);
    // Listing the tree now means the first lookup does not wait for it, and a folder too
    // large for the default limit asks about a full scan while the user is still here.
    refreshWorkspaceFileIndex(true);
    if (tabs_->count() == 0)
        createNewDocument();
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
