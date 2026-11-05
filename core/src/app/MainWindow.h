#pragma once

#include "app/AppearanceSettings.h"
#include "core/Document.h"
#include "editor/EditorViewOptions.h"
#include "git/GitTypes.h"
#include "workspace/WorkspaceFileIndex.h"
#include "workspace/WorkspaceSearch.h"

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <memory>

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
class QThread;
class QTimer;
class QToolButton;
class QVariant;

namespace ketplus {

class QuickOpenPopup;
class SearchPanel;

struct SearchOptions;

class EditorSplitPane;
class EditorWidget;
class SymbolIndex;
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
    ~MainWindow() override;

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
    // How long a collected file list is reused before the tree is walked again.
    static constexpr qint64 workspaceIndexMaxAgeMs = 30 * 1000;
    [[nodiscard]] int workspaceFileLimit(const QString& root) const;
    // The cached file list for `root`, empty when it is missing or too old to trust.
    [[nodiscard]] WorkspaceFileList cachedWorkspaceFiles(const QString& root) const;
    void askAboutFullWorkspaceScan(const QString& root);
    void finishWorkspaceFileIndex(const QString& root, const WorkspaceFileList& files);
    void updateGoToFileItems();
    SearchPanel* ensureSearchPanel();
    void applySearchPanelColors();
    void setSearchPanelVisible(bool visible);
    void startWorkspaceSearch();
    void startWorkspaceSearch(const WorkspaceSearchOptions& options, bool definitionSearch);
    // Ctrl/Cmd+click and F12: opens a file reference, or finds where a symbol is declared.
    void goToDefinition();
    // Builds the symbol index for `root` in the background, if it is not there already.
    void startSymbolIndex(const QString& root);
    void clearSymbolIndex();
    [[nodiscard]] bool resolveDefinitionFromIndex(EditorWidget* editor, const QString& symbol);
    void openWorkspaceFile(const QString& relativePath, int line);
    void resolveDefinition(EditorWidget* editor, const QString& symbol, const QString& fileToken,
                           const QString& lineText);
    [[nodiscard]] QString resolveFileReference(EditorWidget* editor, const QString& token) const;
    bool openFileReference(EditorWidget* editor, const QString& fileToken);
    // Follows `import { name } from "./module"` to the declaration inside that module.
    bool openImportedSymbol(EditorWidget* editor, const QString& symbol, const QString& lineText);
    void finishDefinitionSearch();
    void cancelWorkspaceSearch();
    void finishWorkspaceSearch(const WorkspaceSearchSummary& summary, bool cancelled);
    void replaceInWorkspace();
    void openSearchMatch(const QString& path, int line, int column, int length);
    [[nodiscard]] EditorWidget* documentOwner() const;
    [[nodiscard]] EditorWidget* openEditorForPath(const QString& path) const;
    void populateFormatMenus();
    void syncFormatMenus();
    void updateFormatIndicators();
    void setEditorSyntax(const QString& syntaxName);
    void setDocumentLineEnding(LineEnding lineEnding);
    void reopenWithEncoding(TextEncoding encoding);
    void saveWithEncoding(TextEncoding encoding);
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
    enum class QuickOpenMode { Commands, Files, Symbols, Definitions };
    // The side panel the search panel replaced, restored when search is closed.
    enum class SidePanel { None, Explorer, SourceControl };

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
    QLabel* documentStateLabel_{nullptr};
    QToolButton* encodingButton_{nullptr};
    QToolButton* lineEndingButton_{nullptr};
    QToolButton* languageButton_{nullptr};
    QMenu* languageMenu_{nullptr};
    QMenu* lineEndingMenu_{nullptr};
    QMenu* encodingMenu_{nullptr};
    QMenu* saveEncodingMenu_{nullptr};
    SearchPanel* searchPanel_{nullptr};
    QAction* searchPanelAction_{nullptr};
    QPointer<QThread> searchThread_;
    std::shared_ptr<std::atomic_bool> searchCancelled_;
    quint64 searchGeneration_{0};
    WorkspaceSearchOptions lastSearchOptions_;
    QList<WorkspaceSearchFileResult> definitionResults_;
    QString definitionSymbol_;
    bool definitionSearch_{false};
    QString definitionPattern_;
    SidePanel panelBeforeSearch_{SidePanel::None};
    QToolButton* gitButton_{nullptr};
    QTimer* highlightTimer_{nullptr};
    QSplitter* documentSplit_{nullptr};
    EditorSplitPane* splitPane_{nullptr};
    EditorWidget* splitEditor_{nullptr};
    EditorWidget* splitSource_{nullptr};
    QAction* closeSplitAction_{nullptr};
    QAction* fullScreenAction_{nullptr};
    QAction* distractionFreeAction_{nullptr};
    QShortcut* exitFullScreenShortcut_{nullptr};
    QuickOpenPopup* quickOpen_{nullptr};
    QuickOpenMode quickOpenMode_{QuickOpenMode::Commands};
    QAction* commandPaletteAction_{nullptr};
    QAction* goToDefinitionAction_{nullptr};
    QAction* backAction_{nullptr};
    QAction* forwardAction_{nullptr};
    QList<QPointer<QAction>> paletteActions_;
    QPointer<EditorWidget> symbolEditor_;
    QStringList workspaceFiles_;
    QStringList declinedFullScanRoots_;
    std::unique_ptr<SymbolIndex> symbolIndex_;
    QPointer<QThread> symbolIndexThread_;
    std::shared_ptr<std::atomic_bool> symbolIndexCancelled_;
    QString symbolIndexRoot_;
    QTimer* symbolIndexIdleTimer_{nullptr};
    QString workspaceFilesRoot_;
    qint64 workspaceFilesIndexedAt_{0};
    bool workspaceFilesTruncated_{false};
    bool workspaceIndexing_{false};
    QPointer<QThread> workspaceIndexThread_;
    std::shared_ptr<std::atomic_bool> workspaceIndexCancelled_;
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
