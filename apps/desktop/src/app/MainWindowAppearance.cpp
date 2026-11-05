#include "agent/AgentWorkspaceView.h"
#include "app/AtlasBuilderTab.h"
#include "app/MainWindow.h"
#include "editor/EditorWidget.h"
#include "git/GitDiffView.h"
#include "preview/MarkdownPreviewPane.h"
#include "terminal/TerminalPanel.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QStatusBar>
#include <QTabWidget>

namespace ketplus {
void MainWindow::applyAppearanceSettings(const AppearanceSettings& settings) {
    auto normalized = settings.normalized();
    normalized.interfaceFontSizePixels =
        InterfaceZoomMetrics::snap(normalized.interfaceFontSizePixels);
    normalized.save();
    QList<MainWindow*> windows;
    for (auto* widget : QApplication::topLevelWidgets()) {
        auto* window = qobject_cast<MainWindow*>(widget);
        if (window && &window->theme_ == &theme_) {
            window->appearanceSettings_ = normalized;
            windows.append(window);
        }
    }
    // ThemeChanged is synchronous: every window must already have the snapshot.
    theme_.setInterfaceFontSizePixels(normalized.interfaceFontSizePixels);
    for (auto* window : windows)
        window->applyAppearanceToSurfaces();
}

void MainWindow::applyAppearanceToSurfaces() {
    auto* activeEditor = currentEditor();
    for (int index = 0; index < tabs_->count(); ++index) {
        if (auto* editor = qobject_cast<EditorWidget*>(tabs_->widget(index))) {
            editor->setEditorSettings(appearanceSettings_.editor);
            editor->setPreviewTypography(appearanceSettings_.preview.fontSizePixels,
                                         appearanceSettings_.preview.lineHeightPixels);
            if (editor == activeEditor && !editor->isHibernated()) {
                editor->applyTheme(theme_.palette());
            } else {
                editor->markThemePending();
            }
        } else if (auto* diffView = qobject_cast<GitDiffView*>(tabs_->widget(index))) {
            diffView->applyEditorSettings(appearanceSettings_.editor);
        }
    }
    if (agentDetailTabs_ != nullptr) {
        for (int index = 0; index < agentDetailTabs_->count(); ++index) {
            if (auto* editor = qobject_cast<EditorWidget*>(agentDetailTabs_->widget(index))) {
                editor->setEditorSettings(appearanceSettings_.editor);
                editor->setPreviewTypography(appearanceSettings_.preview.fontSizePixels,
                                             appearanceSettings_.preview.lineHeightPixels);
                if (editor == activeEditor && !editor->isHibernated()) {
                    editor->applyTheme(theme_.palette());
                } else {
                    editor->markThemePending();
                }
            } else if (auto* diffView =
                           qobject_cast<GitDiffView*>(agentDetailTabs_->widget(index))) {
                diffView->applyEditorSettings(appearanceSettings_.editor);
            } else if (auto* terminal =
                           qobject_cast<TerminalPanel*>(agentDetailTabs_->widget(index))) {
                applyTerminalTypography(terminal);
            }
        }
    }
    if (splitEditor_ != nullptr) {
        splitEditor_->setEditorSettings(appearanceSettings_.editor);
        splitEditor_->setPreviewTypography(appearanceSettings_.preview.fontSizePixels,
                                           appearanceSettings_.preview.lineHeightPixels);
        splitEditor_->applyTheme(theme_.palette());
    }
    if (terminal_ != nullptr) {
        applyTerminalTypography(terminal_);
    }
    if (markdownPreview_ != nullptr) {
        markdownPreview_->setTypography(appearanceSettings_.preview.fontSizePixels,
                                        appearanceSettings_.preview.lineHeightPixels);
    }
    statusBar()->showMessage(QStringLiteral("Appearance settings updated"), 2500);
    // Font-family-only saves do not emit ThemeManager::themeChanged.
    applyThemeToEditors();
}
} // namespace ketplus
