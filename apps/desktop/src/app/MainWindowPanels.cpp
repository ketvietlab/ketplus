#include "app/MainWindow.h"
#include "app/RepositoryRouter.h"
#include "app/SpotlightDialog.h"
#include "atlas/AtlasTab.h"
#include "workspace/AtlasIndex.h"

#include "agent/AgentSessionManager.h"
#include "agent/AgentWorkspaceView.h"
#include "agent/CodexRateLimits.h"
#include "app/SettingsDialog.h"
#include "editor/EditorWidget.h"
#include "git/GitChangesPanel.h"
#include "git/GitDiffView.h"
#include "git/GitService.h"
#include "preview/MarkdownPreviewPane.h"
#include "terminal/TerminalPanel.h"
#include "ui/FindReplaceBar.h"
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
#include <QUrl>
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

MarkdownPreviewPane* MainWindow::ensureMarkdownPreview() {
    if (markdownPreview_ != nullptr) {
        return markdownPreview_;
    }

    markdownPreview_ = new MarkdownPreviewPane(editorSplit_);
    markdownPreview_->setTypography(appearanceSettings_.preview.fontSizePixels,
                                    appearanceSettings_.preview.lineHeightPixels);
    editorSplit_->addWidget(markdownPreview_);
    editorSplit_->setStretchFactor(0, 1);
    editorSplit_->setStretchFactor(1, 1);
    connect(markdownPreview_, &MarkdownPreviewPane::closeRequested, this,
            [this] { setMarkdownPreviewVisible(false); });
    connect(markdownPreview_, &MarkdownPreviewPane::fileOpenRequested, this, &MainWindow::openFile);
    connect(markdownPreview_, &MarkdownPreviewPane::statusMessageRequested, this,
            [this](const QString& message) { statusBar()->showMessage(message, 7000); });
    return markdownPreview_;
}

void MainWindow::setMarkdownPreviewVisible(const bool visible) {
    if (auto* editor = currentEditor())
        editor->setMarkdownPreviewVisible(visible);
    updateMarkdownPreview();
}

void MainWindow::updateMarkdownPreview() {
    auto* editor = currentEditor();
    const QSignalBlocker blocker(markdownPreviewAction_);
    markdownPreviewAction_->setChecked(editor && editor->isMarkdownPreviewVisible());
    if (editor)
        editor->setPreviewTypography(appearanceSettings_.preview.fontSizePixels,
                                     appearanceSettings_.preview.lineHeightPixels);
}

void MainWindow::applyTerminalTypography(TerminalPanel* terminal) {
    if (terminal == nullptr)
        return;
    const int fontSize = appearanceSettings_.terminal.fontSizePixels;
    terminal->setTypography(fontSize, appearanceSettings_.terminal.lineHeightPixels,
                            appearanceSettings_.terminalFontFamily);
    // The product theme owns the global interface font. A local rule keeps the
    // independently configurable terminal font from inheriting that size.
    terminal->setStyleSheet(
        QStringLiteral("QAbstractScrollArea[kvRole=\"terminalView\"] { font-size: %1px; }")
            .arg(fontSize));
}

TerminalPanel* MainWindow::ensureTerminal() {
    if (terminal_ != nullptr) {
        return terminal_;
    }

    terminal_ = new TerminalPanel(mainSplit_);
    terminal_->applyTheme(theme_.palette());
    applyTerminalTypography(terminal_);
    mainSplit_->addWidget(terminal_);
    mainSplit_->setStretchFactor(mainSplit_->indexOf(workspaceSplit_), 1);
    mainSplit_->setStretchFactor(mainSplit_->indexOf(terminal_), 0);
    connect(terminal_, &TerminalPanel::closeRequested, this, [this] { setTerminalVisible(false); });
    connect(terminal_, &TerminalPanel::statusMessageRequested, this,
            [this](const QString& message) { statusBar()->showMessage(message, 5000); });
    return terminal_;
}

void MainWindow::placeFindBar() {
    QWidget* host =
        agentWorkspaceMode_ && agentDetailPane_ != nullptr ? agentDetailPane_ : editorPane_;
    if (findBar_->parentWidget() == host)
        return;
    const bool visible = findBar_->isVisibleTo(findBar_->parentWidget());
    host->layout()->addWidget(findBar_);
    findBar_->setVisible(visible);
}

QTabWidget* MainWindow::ensureAgentDetailTabs() {
    if (agentDetailTabs_ != nullptr) {
        return agentDetailTabs_;
    }

    agentDetailPane_ = new QWidget(workspaceSplit_);
    agentDetailPane_->setObjectName(QStringLiteral("agentDetailPane"));
    auto* paneLayout = new QVBoxLayout(agentDetailPane_);
    paneLayout->setContentsMargins(0, 0, 0, 0);
    paneLayout->setSpacing(0);
    agentDetailTabs_ = new QTabWidget(agentDetailPane_);
    paneLayout->addWidget(agentDetailTabs_, 1);
    agentDetailTabs_->setObjectName(QStringLiteral("agentDetailTabs"));
    agentDetailTabs_->setProperty("kvRole", QStringLiteral("agentDetailTabs"));
    agentDetailTabs_->setDocumentMode(true);
    agentDetailTabs_->setMovable(true);
    agentDetailTabs_->setTabsClosable(true);
    agentDetailTabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    agentDetailPane_->hide();
    workspaceSplit_->addWidget(agentDetailPane_);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(agentWorkspace_), 1);
    workspaceSplit_->setStretchFactor(workspaceSplit_->indexOf(agentDetailPane_), 1);
    connect(agentDetailTabs_, &QTabWidget::tabCloseRequested, this,
            &MainWindow::closeAgentDetailTab);
    connect(agentDetailTabs_->tabBar(), &QTabBar::customContextMenuRequested, this,
            &MainWindow::showAgentDetailTabContextMenu);
    connect(agentDetailTabs_, &QTabWidget::currentChanged, this,
            [this] { updateAgentDetailTabs(); });
    return agentDetailTabs_;
}

TerminalPanel* MainWindow::openAgentTerminalTab() {
    auto* detailTabs = ensureAgentDetailTabs();
    auto* terminal = new TerminalPanel(detailTabs);
    terminal->applyTheme(theme_.palette());
    applyTerminalTypography(terminal);
    terminal->start(workspaceRoot_.isEmpty() ? QDir::homePath() : workspaceRoot_);
    connect(terminal, &TerminalPanel::closeRequested, this, [this, terminal] {
        if (agentDetailTabs_ == nullptr) {
            return;
        }
        const int index = agentDetailTabs_->indexOf(terminal);
        if (index >= 0) {
            closeAgentDetailTab(index);
        }
    });
    connect(terminal, &TerminalPanel::statusMessageRequested, this,
            [this](const QString& message) { statusBar()->showMessage(message, 5000); });

    const int index =
        detailTabs->addTab(terminal, QStringLiteral("Terminal %1").arg(++agentTerminalSerial_));
    detailTabs->setTabToolTip(index, workspaceRoot_.isEmpty() ? QDir::homePath() : workspaceRoot_);
    detailTabs->setCurrentIndex(index);
    updateAgentDetailTabs();
    terminal->focusTerminal();
    return terminal;
}

void MainWindow::closeAgentDetailTab(const int index) {
    if (agentDetailTabs_ == nullptr || index < 0 || index >= agentDetailTabs_->count()) {
        return;
    }
    auto* tab = agentDetailTabs_->widget(index);
    if (auto* editor = qobject_cast<EditorWidget*>(tab);
        editor != nullptr && !maybeCloseEditor(editor)) {
        return;
    }
    agentDetailTabs_->removeTab(index);
    tab->deleteLater();
    updateAgentDetailTabs();
    if (agentDetailTabs_->count() == 0 && agentWorkspace_ != nullptr) {
        agentWorkspace_->focusPrompt();
    }
}

void MainWindow::closeOtherAgentDetailTabs(const int keepIndex) {
    if (agentDetailTabs_ == nullptr) {
        return;
    }
    auto* keepTab = agentDetailTabs_->widget(keepIndex);
    if (keepTab == nullptr) {
        return;
    }

    agentDetailTabs_->setCurrentWidget(keepTab);
    for (int index = agentDetailTabs_->count() - 1; index >= 0; --index) {
        auto* tab = agentDetailTabs_->widget(index);
        if (tab == keepTab) {
            continue;
        }
        closeAgentDetailTab(index);
        if (agentDetailTabs_->indexOf(tab) >= 0) {
            return;
        }
    }
}

void MainWindow::closeAllAgentDetailTabs() {
    if (agentDetailTabs_ == nullptr) {
        return;
    }
    for (int index = agentDetailTabs_->count() - 1; index >= 0; --index) {
        auto* tab = agentDetailTabs_->widget(index);
        closeAgentDetailTab(index);
        if (agentDetailTabs_->indexOf(tab) >= 0) {
            return;
        }
    }
}

void MainWindow::showAgentDetailTabContextMenu(const QPoint& position) {
    if (agentDetailTabs_ == nullptr) {
        return;
    }
    auto* tab = agentDetailTabs_->widget(agentDetailTabs_->tabBar()->tabAt(position));
    if (tab == nullptr) {
        return;
    }

    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("agentDetailTabContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto* closeAction = menu->addAction(QStringLiteral("Close"));
    auto* closeOthersAction = menu->addAction(QStringLiteral("Close Other Tabs"));
    auto* closeAllAction = menu->addAction(QStringLiteral("Close All Tabs"));
    closeOthersAction->setEnabled(agentDetailTabs_->count() > 1);

    connect(closeAction, &QAction::triggered, this, [this, tab] {
        if (agentDetailTabs_ != nullptr) {
            closeAgentDetailTab(agentDetailTabs_->indexOf(tab));
        }
    });
    connect(closeOthersAction, &QAction::triggered, this, [this, tab] {
        if (agentDetailTabs_ != nullptr) {
            closeOtherAgentDetailTabs(agentDetailTabs_->indexOf(tab));
        }
    });
    connect(closeAllAction, &QAction::triggered, this, &MainWindow::closeAllAgentDetailTabs);
    for (QAction* action : {closeAction, closeOthersAction, closeAllAction}) {
        connect(action, &QAction::triggered, menu, &QMenu::close);
    }
    menu->popup(agentDetailTabs_->tabBar()->mapToGlobal(position));
}

void MainWindow::updateAgentDetailTabs() {
    updateAtlasActivity();
    if (agentDetailTabs_ == nullptr) {
        return;
    }
    const bool hasTabs = agentDetailTabs_->count() > 0 && !compactConversationWindow_;
    agentDetailPane_->setVisible(agentWorkspaceMode_ && hasTabs);
    if (agentWorkspaceMode_ && agentWorkspace_ != nullptr) {
        QList<int> sizes(workspaceSplit_->count(), 0);
        const int width = qMax(800, workspaceSplit_->width());
        sizes[workspaceSplit_->indexOf(agentWorkspace_)] = hasTabs ? width / 2 : width;
        if (hasTabs) {
            sizes[workspaceSplit_->indexOf(agentDetailPane_)] = width - width / 2;
        }
        workspaceSplit_->setSizes(sizes);
    }

    bool terminalSelected = false;
    if (hasTabs) {
        terminalSelected =
            qobject_cast<TerminalPanel*>(agentDetailTabs_->currentWidget()) != nullptr;
    }
    const QSignalBlocker blocker(terminalAction_);
    terminalAction_->setChecked(agentWorkspaceMode_ && terminalSelected);
    activateCurrentTab();
    updateEditorActions();
    updateDocumentState();
    trackNavigation(currentEditor());
}

bool MainWindow::showAgentFile(const QString& filePath) {
    if (!agentWorkspaceMode_) {
        return false;
    }

    if (compactConversationWindow_)
        setCompactConversationWindow(false);
    auto* detailTabs = ensureAgentDetailTabs();
    const QString absolutePath = normalizedPath(filePath);
    for (int index = 0; index < detailTabs->count(); ++index) {
        auto* existing = qobject_cast<EditorWidget*>(detailTabs->widget(index));
        if (existing != nullptr && !existing->document().isUntitled() &&
            normalizedPath(existing->document().filePath()) == absolutePath) {
            const auto refreshed = existing->refreshFromDisk();
            if (!refreshed.ok)
                QMessageBox::warning(this, QStringLiteral("Unable to refresh file"),
                                     refreshed.error);
            detailTabs->setCurrentIndex(index);
            updateAgentDetailTabs();
            return true;
        }
    }

    auto* editor = createEditor();
    const auto result = editor->loadFile(absolutePath);
    if (!result.ok) {
        QMessageBox::critical(this, QStringLiteral("Unable to open file"), result.error);
        editor->deleteLater();
        return true;
    }

    editor->applyTheme(theme_.palette());
    const int index = detailTabs->addTab(editor, editor->document().displayName());
    detailTabs->setTabToolTip(index, absolutePath);
    detailTabs->setCurrentIndex(index);
    updateAgentDetailTabs();
    statusBar()->showMessage(QStringLiteral("Opened %1").arg(absolutePath), 2000);
    return true;
}

bool MainWindow::showAgentDiff(const QString& filePath, const GitDiffMode mode,
                               const QString& diff) {
    if (!agentWorkspaceMode_) {
        return false;
    }
    auto* detailTabs = ensureAgentDetailTabs();
    const QString absolutePath = normalizedPath(filePath);
    for (int index = 0; index < detailTabs->count(); ++index) {
        auto* existing = qobject_cast<GitDiffView*>(detailTabs->widget(index));
        if (existing != nullptr && normalizedPath(existing->filePath()) == absolutePath &&
            existing->mode() == mode) {
            existing->setDiff(absolutePath, mode, diff, theme_.palette());
            detailTabs->setCurrentIndex(index);
            updateAgentDetailTabs();
            statusBar()->showMessage(QStringLiteral("Updated Git diff"), 2000);
            return true;
        }
    }

    auto* diffView = new GitDiffView(detailTabs);
    diffView->applyEditorSettings(appearanceSettings_.editor);
    diffView->setDiff(absolutePath, mode, diff, theme_.palette());
    connect(diffView, &GitDiffView::openFileRequested, this, &MainWindow::openFile);
    connect(diffView, &GitDiffView::refreshRequested, this,
            [this](const QString& path, const GitDiffMode requestedMode) {
                requestFileDiff(path, requestedMode);
            });

    const QString displayName = QStringLiteral("%1 Diff").arg(QFileInfo(absolutePath).fileName());
    const int index = detailTabs->addTab(diffView, displayName);
    detailTabs->setTabToolTip(index, absolutePath);
    detailTabs->setCurrentIndex(index);
    updateAgentDetailTabs();
    statusBar()->showMessage(QStringLiteral("Opened Git diff"), 2000);
    return true;
}

void MainWindow::setTerminalVisible(const bool visible) {
    if (visible && atlasWorkspaceMode_)
        return;
    if (visible) {
        if (agentWorkspaceMode_) {
            openAgentTerminalTab();
            return;
        }
        auto* terminal = ensureTerminal();
        terminal->show();
        const int availableHeight = qMax(420, mainSplit_->height());
        const int terminalHeight = qBound(160, availableHeight / 3, 300);
        mainSplit_->setSizes({availableHeight - terminalHeight, terminalHeight});
        terminal->start(workspaceRoot_.isEmpty() ? QDir::homePath() : workspaceRoot_);
        terminal->focusTerminal();
        const QSignalBlocker blocker(terminalAction_);
        terminalAction_->setChecked(true);
        return;
    }

    if (terminal_ != nullptr) {
        terminal_->hide();
    }
    if (agentWorkspaceMode_ && agentDetailTabs_ != nullptr) {
        for (int index = agentDetailTabs_->count() - 1; index >= 0; --index) {
            if (qobject_cast<TerminalPanel*>(agentDetailTabs_->widget(index)) != nullptr) {
                closeAgentDetailTab(index);
            }
        }
        updateAgentDetailTabs();
    }
    const QSignalBlocker blocker(terminalAction_);
    terminalAction_->setChecked(false);
    if (agentWorkspaceMode_ && agentWorkspace_ != nullptr) {
        if (agentDetailTabs_ == nullptr || agentDetailTabs_->count() == 0) {
            QList<int> sizes(workspaceSplit_->count(), 0);
            sizes[workspaceSplit_->indexOf(agentWorkspace_)] = qMax(800, workspaceSplit_->width());
            workspaceSplit_->setSizes(sizes);
            agentWorkspace_->focusPrompt();
        } else {
            updateAgentDetailTabs();
        }
        return;
    }
    if (auto* editor = currentEditor()) {
        editor->setFocus();
    }
}

void MainWindow::focusTerminal() {
    if (agentWorkspaceMode_) {
        if (agentDetailTabs_ != nullptr) {
            for (int index = agentDetailTabs_->count() - 1; index >= 0; --index) {
                auto* terminal = qobject_cast<TerminalPanel*>(agentDetailTabs_->widget(index));
                if (terminal != nullptr) {
                    agentDetailTabs_->setCurrentIndex(index);
                    updateAgentDetailTabs();
                    terminal->focusTerminal();
                    return;
                }
            }
        }
    }
    setTerminalVisible(true);
}

} // namespace ketplus
