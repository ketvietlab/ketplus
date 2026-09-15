#pragma once

#include "app/AppearanceSettings.h"
#include "editor/EditorViewOptions.h"
#include "git/GitTypes.h"
#include "workspace/WorkspaceFileIndex.h"

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QStringList>

#include <cstdint>

class QCloseEvent;
class QEvent;
class QAction;
class QLabel;
class QMenu;
class QPoint;
class QSplitter;
class QString;
class QTabWidget;
class QShortcut;
class QTimer;
class QToolButton;
class QVariant;

namespace ketplus {

class QuickOpenPopup;

struct SearchOptions;

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
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

  private:
    void createActions();
    void createNewDocument();
    void openDocument();
    void chooseFolder();
    bool saveCurrentDocument();
    bool saveCurrentDocumentAs();
    bool saveAllDocuments();
    bool saveEditor(EditorWidget* editor, bool choosePath);
    void reopenClosedTab();
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
    void rememberRecentFile(const QString& path);
    void rebuildRecentFilesMenu();
    void openFindBar(bool replaceMode);
    void findNext(bool backwards = false);
    void replaceCurrentMatch();
    void replaceAllMatches();
    [[nodiscard]] SearchOptions searchOptions() const;
    void refreshMatchHighlights(bool updateCount = true);
    void clearMatchHighlights();
    void applyViewOptions(const EditorViewOptions& options);
    void goToLine();
    [[nodiscard]] EditorWidget* activeEditor() const;
    void openSplit(EditorWidget* source, Qt::Orientation orientation);
    void closeSplit();
    void focusOtherView();
    void setFullScreen(bool fullScreen);
    void setDistractionFree(bool enabled);
    void changeZoom(int delta);
    void syncEditorZoom(EditorWidget* editor);
    void showCommandPalette();
    void showGoToFile();
    void showGoToSymbol();
    QuickOpenPopup* ensureQuickOpen();
    [[nodiscard]] QString quickOpenStyleSheet() const;
    void activateQuickOpenItem(const QVariant& data);
    void refreshWorkspaceFileIndex(bool force);
    void finishWorkspaceFileIndex(const QString& root, const WorkspaceFileList& files);
    void updateGoToFileItems();
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

    struct NavigationLocation final {
        QPointer<EditorWidget> editor;
        QString filePath;
        qint64 position{0};
        int line{0};
    };
    enum class QuickOpenMode { Commands, Files, Symbols };

    [[nodiscard]] NavigationLocation locationOf(EditorWidget* editor) const;
    [[nodiscard]] bool isLocationAvailable(const NavigationLocation& location) const;
    void trackNavigation(EditorWidget* editor);
    void pushNavigationLocation(const NavigationLocation& location);
    void navigateHistory(bool back);
    void restoreLocation(const NavigationLocation& location);
    void updateNavigationActions();

    ThemeManager& theme_;
    AppearanceSettings appearanceSettings_;
    EditorViewOptions viewOptions_;
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
    QTimer* highlightTimer_{nullptr};
    QSplitter* documentSplit_{nullptr};
    EditorWidget* splitEditor_{nullptr};
    EditorWidget* splitSource_{nullptr};
    QAction* closeSplitAction_{nullptr};
    QAction* fullScreenAction_{nullptr};
    QAction* distractionFreeAction_{nullptr};
    QShortcut* exitFullScreenShortcut_{nullptr};
    QuickOpenPopup* quickOpen_{nullptr};
    QuickOpenMode quickOpenMode_{QuickOpenMode::Commands};
    QAction* commandPaletteAction_{nullptr};
    QAction* backAction_{nullptr};
    QAction* forwardAction_{nullptr};
    QList<QPointer<QAction>> paletteActions_;
    QPointer<EditorWidget> symbolEditor_;
    QStringList workspaceFiles_;
    QString workspaceFilesRoot_;
    qint64 workspaceFilesIndexedAt_{0};
    bool workspaceFilesTruncated_{false};
    bool workspaceIndexing_{false};
    QList<NavigationLocation> backLocations_;
    QList<NavigationLocation> forwardLocations_;
    NavigationLocation lastLocation_;
    bool restoringNavigation_{false};
    struct DistractionFreeRestore final {
        bool explorer{false};
        bool sourceControl{false};
        bool terminal{false};
        bool markdownPreview{false};
        bool fullScreen{false};
    } distractionFreeRestore_;
    bool splitFocused_{false};
    bool distractionFree_{false};
    bool maximizedBeforeFullScreen_{false};
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
    QMenu* recentFilesMenu_{nullptr};
    QAction* reopenClosedTabAction_{nullptr};
    QStringList closedFilePaths_;
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
