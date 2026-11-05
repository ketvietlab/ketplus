#include "app/MainWindow.h"

#include "editor/EditorSplitPane.h"
#include "editor/EditorWidget.h"
#include "git/GitChangesPanel.h"
#include "preview/MarkdownPreviewPane.h"
#include "terminal/TerminalPanel.h"
#include "ui/Theme.h"
#include "workspace/ActivityBar.h"
#include "workspace/ExplorerPanel.h"
#include "workspace/SearchPanel.h"

#include <QAction>
#include <QEvent>
#include <QLabel>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QWindowStateChangeEvent>

#include <initializer_list>

namespace ketplus {

EditorWidget* MainWindow::documentOwner() const {
    // The split view shares its source tab's document, file and encoding.
    auto* editor = currentEditor();
    return editor != nullptr && editor == splitEditor_ && splitSource_ != nullptr ? splitSource_
                                                                                  : editor;
}

void MainWindow::openSplit(EditorWidget* source, const Qt::Orientation orientation) {
    if (source == nullptr || agentWorkspaceMode_ || atlasWorkspaceMode_ ||
        tabs_->indexOf(source) < 0) {
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
                    updateDocumentState();
                });
        connect(splitPane_, &EditorSplitPane::emptied, this, &MainWindow::closeSplit);
        connect(splitEditor_, &EditorWidget::contextMenuRequested, this,
                [this](const QPoint& position) { showEditorContextMenu(splitEditor_, position); });
        connect(splitEditor_, &EditorWidget::definitionRequested, this,
                [this](const QString& symbol, const QString& fileToken,
                       const QString& lineText) {
                    resolveDefinition(splitEditor_, symbol, fileToken, lineText);
                });
        connect(splitEditor_, &EditorWidget::editorStateChanged, this, [this] {
            if (currentEditor() == splitEditor_) {
                updateEditorActions();
            }
        });
        connect(splitEditor_, &EditorWidget::cursorPositionChanged, this,
                [this](const int line, const int column) {
                    if (currentEditor() == splitEditor_) {
                        cursorPositionLabel_->setText(
                            QStringLiteral("Ln %1, Col %2").arg(line).arg(column));
                        trackNavigation(splitEditor_);
                    }
                });
        connect(splitEditor_, &ScintillaEditBase::zoom, this, [this](const int zoom) {
            if (zoom != viewOptions_.zoom) {
                auto options = viewOptions_;
                options.zoom = zoom;
                applyViewOptions(options);
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
    splitFocused_ = true;
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
    updateDocumentState();
}

void MainWindow::focusOtherView() {
    if (splitEditor_ == nullptr || agentWorkspaceMode_ || atlasWorkspaceMode_) {
        return;
    }
    if (currentEditor() == splitEditor_) {
        splitFocused_ = false;
        if (auto* editor = currentEditor()) {
            editor->setFocus();
        }
    } else {
        splitFocused_ = true;
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
        const auto isShown = [](const QWidget* widget) {
            return widget != nullptr && widget->isVisible();
        };
        const std::initializer_list<QWidget*> panels{explorer_, searchPanel_, gitChanges_,
                                                     terminal_, markdownPreview_};
        if (enabled) {
            distractionFreeRestore_ = {
                .explorer = isShown(explorer_),
                .search = isShown(searchPanel_),
                .sourceControl = isShown(gitChanges_),
                .terminal = isShown(terminal_),
                .markdownPreview = isShown(markdownPreview_),
                .fullScreen = isFullScreen(),
                .statusBar = statusBar()->isVisible(),
            };
            distractionFree_ = true;
            for (QWidget* panel : panels) {
                if (panel != nullptr) {
                    panel->hide();
                }
            }
            activityBar_->hide();
            statusBar()->hide();
            tabs_->tabBar()->hide();
            setFullScreen(true);
        } else {
            distractionFree_ = false;
            activityBar_->show();
            // The shell only shows the status bar in some modes; restore what was there.
            statusBar()->setVisible(distractionFreeRestore_.statusBar);
            tabs_->tabBar()->show();
            // Show the panels directly so the workspace mode stays exactly as it was.
            if (distractionFreeRestore_.explorer && explorer_ != nullptr) {
                explorer_->show();
            }
            if (distractionFreeRestore_.search && searchPanel_ != nullptr) {
                searchPanel_->show();
            }
            if (distractionFreeRestore_.sourceControl && gitChanges_ != nullptr) {
                gitChanges_->show();
            }
            if (distractionFreeRestore_.terminal && terminal_ != nullptr) {
                terminal_->show();
            }
            if (distractionFreeRestore_.markdownPreview && markdownPreview_ != nullptr) {
                markdownPreview_->show();
            }
            if (!distractionFreeRestore_.fullScreen) {
                setFullScreen(false);
            }
        }
    }

    const QSignalBlocker blocker(distractionFreeAction_);
    distractionFreeAction_->setChecked(distractionFree_);
    if (auto* editor = currentEditor()) {
        editor->setFocus();
    }
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

} // namespace ketplus
