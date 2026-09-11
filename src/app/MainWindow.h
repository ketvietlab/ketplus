#pragma once

#include "app/AppearanceSettings.h"
#include "git/GitTypes.h"

#include <QHash>
#include <QMainWindow>

#include <cstdint>

class QCloseEvent;
class QAction;
class QLabel;
class QMenu;
class QPoint;
class QSplitter;
class QString;
class QTabWidget;
class QToolButton;

namespace ketplus {

class EditorWidget;
class FindReplaceBar;
class ExplorerPanel;
class GitChangesPanel;
class GitService;
class MarkdownPreviewPane;
class TerminalPanel;
class ThemeManager;

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(ThemeManager& theme, QWidget* parent = nullptr);

    void openFile(const QString& filePath);
    void openFolder(const QString& folderPath);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void createActions();
    void createNewDocument();
    void openDocument();
    void chooseFolder();
    bool saveCurrentDocument();
    bool saveCurrentDocumentAs();
    bool saveEditor(EditorWidget* editor, bool choosePath);
    bool maybeCloseEditor(EditorWidget* editor);
    bool closeTab(int index, bool createReplacement = true);
    void closeOtherTabs(int keepIndex);
    void closeAllTabs();
    void closeCurrentTab();
    void showTabContextMenu(const QPoint& position);
    void showEditorContextMenu(EditorWidget* editor, const QPoint& position);
    QAction* addMarkdownPreviewContextAction(QMenu& menu, EditorWidget* editor);
    void activateAdjacentTab(int offset);
    EditorWidget* createEditor();
    EditorWidget* currentEditor() const;
    void configureTabCloseButton(int index, EditorWidget* editor);
    void configureTabCloseButton(int index, QWidget* tab, const QString& accessibleName);
    void updateTabTitle(EditorWidget* editor);
    void updateEditorActions();
    void updateDocumentState();
    void activateCurrentTab();
    void enforceTabResourcePolicy();
    void rememberRecentFolder(const QString& path);
    void rebuildRecentFoldersMenu();
    void openFindBar(bool replaceMode);
    void findNext(bool backwards = false);
    void replaceCurrentMatch();
    void replaceAllMatches();
    void showSettings();
    void applyAppearanceSettings(const AppearanceSettings& settings);
    void applyThemeToEditors();
    MarkdownPreviewPane* ensureMarkdownPreview();
    void setMarkdownPreviewVisible(bool visible);
    void updateMarkdownPreview();
    TerminalPanel* ensureTerminal();
    void setTerminalVisible(bool visible);
    void focusTerminal();
    ExplorerPanel* ensureExplorer();
    void setExplorerVisible(bool visible);
    void focusExplorer();
    GitChangesPanel* ensureGitChanges();
    void setSourceControlVisible(bool visible);
    void requestCurrentFileDiff();
    void requestFileDiff(const QString& filePath, GitDiffMode mode = GitDiffMode::Combined);
    void showDiff(const QString& filePath, GitDiffMode mode, const QString& diff);
    void updateGitSnapshot(const GitSnapshot& snapshot);
    void updateWorktrees(const QVector<GitWorktree>& worktrees);
    void rebuildWorktreeMenu();
    void updateGitActions();
    void switchToWorktree(const QString& path);
    void rememberActiveFileForWorktree();
    [[nodiscard]] QString currentWorkspaceRelativeFile() const;

    ThemeManager& theme_;
    AppearanceSettings appearanceSettings_;
    QSplitter* mainSplit_{nullptr};
    QSplitter* workspaceSplit_{nullptr};
    QSplitter* editorSplit_{nullptr};
    QTabWidget* tabs_{nullptr};
    FindReplaceBar* findBar_{nullptr};
    QLabel* cursorPositionLabel_{nullptr};
    QLabel* encodingLabel_{nullptr};
    QLabel* lineEndingLabel_{nullptr};
    QLabel* documentStateLabel_{nullptr};
    QToolButton* gitButton_{nullptr};
    QAction* undoAction_{nullptr};
    QAction* redoAction_{nullptr};
    QAction* cutAction_{nullptr};
    QAction* copyAction_{nullptr};
    QAction* pasteAction_{nullptr};
    QAction* deleteAction_{nullptr};
    QAction* selectAllAction_{nullptr};
    QAction* explorerAction_{nullptr};
    QAction* sourceControlAction_{nullptr};
    QAction* markdownPreviewAction_{nullptr};
    QAction* terminalAction_{nullptr};
    QAction* viewDiffAction_{nullptr};
    QMenu* gitMenu_{nullptr};
    QMenu* worktreeMenu_{nullptr};
    QMenu* recentFoldersMenu_{nullptr};
    ExplorerPanel* explorer_{nullptr};
    GitChangesPanel* gitChanges_{nullptr};
    MarkdownPreviewPane* markdownPreview_{nullptr};
    TerminalPanel* terminal_{nullptr};
    GitService* git_{nullptr};
    GitSnapshot gitSnapshot_;
    QVector<GitWorktree> worktrees_;
    QHash<QString, QString> activeFileByWorktree_;
    QString workspaceRoot_;
    std::uint64_t tabAccessSequence_{0};
};

} // namespace ketplus
