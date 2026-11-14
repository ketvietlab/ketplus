#pragma once

#include "app/AppearanceSettings.h"
#include "core/Document.h"
#include "editor/EditorViewOptions.h"
#include "git/GitTypes.h"
#include "workspace/ActivityBar.h"
#include "workspace/WorkspaceFileIndex.h"
#include "workspace/WorkspaceSearch.h"

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QThreadPool>

#include <atomic>
#include <cstdint>
#include <memory>

class QCloseEvent;
namespace ketplus {
class LinuxMachines;
}
class QEvent;
class QShortcut;
class QUrl;
class QAction;
class QLabel;
class QMenu;
class QPoint;
class QSplitter;
class QString;
class QTabWidget;
class QThread;
class QTimer;
class QToolButton;
class QVariant;

namespace ketplus::remote {
class DaemonClient;
}

namespace ketplus {

class DocumentFormatControls;

class QuickOpenPopup;
class SearchPanel;
struct SearchOptions;

class EditorSplitPane;
class EditorWidget;
class SymbolIndex;
class AgentWorktreeCreator;
class AgentSessionManager;
class AgentWorkspaceView;
class FindReplaceBar;
class MachineComposition;
class ExplorerPanel;
class GitChangesPanel;
class GitService;
class MarkdownPreviewPane;
class TerminalPanel;
class ThemeManager;
class AtlasIndex;
class AtlasTab;
class AtlasBuilderTab;
class AtlasConversationPanel;
class RemoteControlView;
class SettingsDialog;
class SettingsWorkspacePage;
class TeamDirectory;
class TeamPage;
class ScheduleWindows;
class ComposerSchedule;
struct AtlasDescriptor;

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    // Shows the app's one set of Linux machines (ADR 0119) in this window: its picker,
    // MachineSelection and Connections read from `machines`, which main() owns, attaches and
    // starts once, so the picker lists them before Connections is opened. Windows that are not
    // given a set (tests) make their own when Connections opens.
    void useLinuxMachines(LinuxMachines* machines);
    explicit MainWindow(ThemeManager& theme, QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFile(const QString& filePath);
    void openFileDiff(const QString& filePath);
    void openFolder(const QString& folderPath);
    bool restoreLastWorkspace();
    void switchToWorktree(const QString& path);
    void setFileContextWorktree(const QString& path);
    void followConversationCheckout();
    // ADR 0119: the machines Connections registered and the one the picker selected.
    MachineComposition* machineComposition();
    // Opens the one Schedules window, selecting `scheduleId` when it is not empty.
    void showSchedules(const QString& scheduleId = {});
    void editSchedule(const QString& scheduleId);

  signals:
    void newWindowRequested();
    void repositoryRequested(const QString& path);
    void repositoryFileRequested(const QString& filePath);
    void repositoryFileDiffRequested(const QString& filePath);
    void repositoryOpened(const QString& path);

  protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

  private:
    struct NavigationLocation final {
        QPointer<EditorWidget> editor;
        QString filePath;
        qint64 position{0};
        int line{0};
    };
    enum class QuickOpenMode { Commands, Symbols, Definitions };

    void createActions();
    void showFileSpotlight();
    void showCommandPalette();
    void showGoToSymbol();
    QuickOpenPopup* ensureQuickOpen();
    [[nodiscard]] QString quickOpenStyleSheet() const;
    void activateQuickOpenItem(const QVariant& data);
    void refreshWorkspaceFileIndex(bool force);
    void finishWorkspaceFileIndex(const QString& root, const WorkspaceFileList& files);
    // How long a collected file list is reused before the tree is walked again.
    static constexpr qint64 workspaceFileListMaxAgeMs = 30 * 1000;
    [[nodiscard]] int workspaceFileLimit(const QString& root) const;
    // The cached file list for `root`, empty when it is missing or too old to trust.
    [[nodiscard]] WorkspaceFileList cachedWorkspaceFiles(const QString& root) const;
    void askAboutFullWorkspaceScan(const QString& root);
    [[nodiscard]] NavigationLocation locationOf(EditorWidget* editor) const;
    [[nodiscard]] bool isLocationAvailable(const NavigationLocation& location) const;
    void trackNavigation(EditorWidget* editor);
    void pushNavigationLocation(const NavigationLocation& location);
    void navigateHistory(bool back);
    void restoreLocation(const NavigationLocation& location);
    void updateNavigationActions();
    void showSpotlight();
    void refreshAtlasIndex();
    void populateSpotlight();
    void openAtlasPicker();
    void showConversationAtlases();
    void editAtlasInKetPlus(const AtlasDescriptor& atlas);
    void setAtlasWorkspaceVisible(bool visible);
    void setRemoteWorkspaceVisible(bool visible);
    // ADR 0119: a remote machine is composed through its MachineBackend (MainWindowMachine).
    void selectMachine(const QString& machineId);
    void setMachineWorkspaceVisible(bool visible);
    // Creates the Connections page, hidden, with its SSH and Relay backends (ADR 0119).
    void ensureRemoteControl();
    void setSettingsWorkspaceVisible(bool visible);
    void hideUtilityWorkspaces();
    void setShellMode(ActivityBar::Mode mode);
    void closeAtlasTab(int index);
    void openAtlas(const AtlasDescriptor& atlas, AtlasTab* picker = nullptr);
    void updateAtlasActivity();
    AtlasTab* createAtlasTab();
    void openAtlasBuilder(AtlasTab* picker);
    void connectAtlasBuilder(AtlasBuilderTab* builder);
    void connectAtlasResults(AgentWorkspaceView* chat);
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
    // Agent workspace that Edit > Find / Find Next / Find Previous act on, or nullptr for the
    // editor find bar.
    AgentWorkspaceView* transcriptFindTarget() const;
    void configureTabCloseButton(int index, EditorWidget* editor);
    void configureTabCloseButton(int index, QWidget* tab, const QString& accessibleName);
    void updateTabTitle(EditorWidget* editor);
    void updateEditorActions();
    // The agent workspace (the main one or an embedded chat) containing `widget`. While it holds
    // focus, its transcript and composer take the Edit menu's commands (MainWindowAgent.cpp).
    static AgentWorkspaceView* agentWorkspaceContaining(QWidget* widget);
    bool enableAgentEditActions();
    bool forwardEditShortcut(QKeySequence::StandardKey key);
    void updateDocumentState();
    void activateCurrentTab();
    void enforceTabResourcePolicy();
    void rememberRecentFolder(const QString& path);
    void rebuildRecentFoldersMenu();
    void openFindBar(bool replaceMode);
    // Moves the find bar under the editor area that is showing: main or agent detail.
    void placeFindBar();
    void findNext(bool backwards = false);
    void replaceCurrentMatch();
    void replaceAllMatches();
    bool saveAllDocuments();
    void reopenClosedTab();
    void rememberRecentFile(const QString& path);
    void rebuildRecentFilesMenu();
    [[nodiscard]] QList<EditorWidget*> openEditors() const;
    [[nodiscard]] SearchOptions searchOptions() const;
    void refreshMatchHighlights(bool updateCount = true);
    void clearMatchHighlights();
    void applyViewOptions(const EditorViewOptions& options);
    void goToLine();
    [[nodiscard]] EditorWidget* documentOwner() const;
    void openSplit(EditorWidget* source, Qt::Orientation orientation);
    void closeSplit();
    void focusOtherView();
    void setFullScreen(bool fullScreen);
    void setDistractionFree(bool enabled);
    void setCompactConversationWindow(bool enabled);
    SearchPanel* ensureSearchPanel();
    void setSearchPanelVisible(bool visible);
    void hideSearchPanel();
    void startWorkspaceSearch();
    void startWorkspaceSearch(const WorkspaceSearchOptions& options, bool definitionSearch);
    void applySearchPanelColors();
    // Ctrl/Cmd+click and F12: opens a file reference, or finds where a symbol is declared.
    void goToDefinition();
    void resolveDefinition(EditorWidget* editor, const QString& symbol, const QString& fileToken,
                           const QString& lineText);
    [[nodiscard]] QString resolveFileReference(EditorWidget* editor, const QString& token) const;
    bool openFileReference(EditorWidget* editor, const QString& fileToken);
    // Follows `import { name } from "./module"` to the declaration inside that module.
    bool openImportedSymbol(EditorWidget* editor, const QString& symbol, const QString& lineText);
    [[nodiscard]] bool resolveDefinitionFromIndex(EditorWidget* editor, const QString& symbol);
    void openWorkspaceFile(const QString& relativePath, int line);
    void finishDefinitionSearch();
    // Builds the symbol index for `root` in the background, if it is not there already.
    void startSymbolIndex(const QString& root);
    void clearSymbolIndex();
    void cancelWorkspaceSearch();
    void finishWorkspaceSearch(const WorkspaceSearchSummary& summary, bool cancelled);
    void replaceInWorkspace();
    void openSearchMatch(const QString& path, int line, int column, int length);
    [[nodiscard]] EditorWidget* openEditorForPath(const QString& path) const;
    void showSettings();
    void setupSidebarStorage();
    // Names the signed-in account on the sidebar identity button (ADR 0109).
    void setupSidebarAccount();
    [[nodiscard]] remote::DaemonClient* accountDaemon();
    // ADR 0114 (#464): teams in the sidebar while the daemon advertises teams/1.
    void setupTeams();
    void setupSchedules();
    // Takes the members of the repository's teams out of `conversations`, into their folders.
    [[nodiscard]] QVector<SidebarTeam>
    takeSidebarTeams(const QString& repository,
                     QHash<QString, QVariantList>& conversationsByRepository) const;
    // Each member conversation's role for the Agent Workspace header (#467).
    void updateTeamRoles();
    void showTeamMenu(const QString& teamId);
    void openTeam(const QString& teamId);
    // #466: the team page in the workspace, or the empty page of a repository without one.
    void showTeamPage(const QString& teamId, const QString& repository);
    void openTeamMember(const QString& sessionId);
    // A goal's `@agent/<number>` mention, from the team page.
    void openConversationNumber(const QString& number);
    void openTeamTask(const QString& teamId, const QString& taskId);
    void joinTeam(const QString& teamId, const QString& sessionId);
    void leaveTeam(const QString& teamId, const QString& sessionId);
    void deleteTeam(const QString& teamId);
    void showNewTeamDialog(const QString& repository);
    void showAddTeamMemberDialog(const QString& teamId);
    void showAddTeamReviewerDialog(const QString& teamId);
    // The conversation menu's team entries; true when `chosen` was one of them.
    bool handleConversationTeamAction(QAction* chosen, const QString& sessionId);
    [[nodiscard]] QString conversationSessionId(const QString& worktreePath,
                                                const QString& conversationId) const;
    void showUpdateSettings();
    // Opens Settings on a section; an empty id keeps the section it shows.
    void showSettingsSection(const QString& id);
    // Opens Sparkle's check (and any pending update); without a configured feed, Settings →
    // Updates.
    void checkForUpdates();
    void applyAppearanceToSurfaces();
    void applyAppearanceSettings(const AppearanceSettings& settings);
    void applyThemeToEditors();
    MarkdownPreviewPane* ensureMarkdownPreview();
    void setMarkdownPreviewVisible(bool visible);
    void updateMarkdownPreview();
    TerminalPanel* ensureTerminal();
    void applyTerminalTypography(TerminalPanel* terminal);
    QTabWidget* ensureAgentDetailTabs();
    TerminalPanel* openAgentTerminalTab();
    // ADR 0106: one Report tab that follows the active conversation (MainWindowWorkReport.cpp).
    void connectAgentWorkReport();
    void openAgentWorkReportTab();
    void openAgentChangesTab();
    void installAgentDetailActions();
    void hideAgentDetailPane();
    void refreshAgentChanges();
    void refreshAgentWorkReport(bool force);
    void closeAgentDetailTab(int index);
    void closeOtherAgentDetailTabs(int keepIndex);
    void closeAllAgentDetailTabs();
    void showAgentDetailTabContextMenu(const QPoint& position);
    void updateAgentDetailTabs();
    bool showAgentFile(const QString& filePath);
    bool showAgentDiff(const QString& filePath, GitDiffMode mode, const QString& diff);
    void setTerminalVisible(bool visible);
    // Type in Terminal (#170): opens a terminal with `command` typed for the user to run.
    void typeInTerminal(const QString& command);
    void focusTerminal();
    void ensureAgentWorkspace();
    // The conversation sessions a dialog needs, without opening the agent pane outside its mode.
    void ensureAgentSessions();
    void syncAgentWorkspaceContext(bool loadTranscript);
    void createAgentWorktree();
    // Create worktree… in a conversation's picker: creates in `repository` and moves that
    // conversation to it, checked against the picker's context `revision`.
    void createConversationWorktree(const QString& repository, qint64 revision);
    void moveConversationToCreatedWorktree(const QString& worktreePath, const QString& branch);
    void startNewAgentConversation();
    [[nodiscard]] QString primaryWorktreePath() const;
    void showAgentWorktreeContextMenu(const QString& path);
    void showWorktreeMenu(const QString& path, const QPoint& globalPosition);
    void showAgentConversationContextMenu(const QString& worktreePath,
                                          const QString& conversationId);
    void changeAgentConversationWorktree(const QString& worktreePath,
                                         const QString& conversationId);
    void renameAgentConversation(const QString& worktreePath, const QString& conversationId);
    void archiveAgentConversation(const QString& worktreePath, const QString& conversationId);
    void forkAgentConversation(const QString& worktreePath, const QString& conversationId);
    void deleteAgentConversation(const QString& worktreePath, const QString& conversationId);
    void clearPendingWorktreeDeletion();
    void removeNextPendingLinkedWorktree();
    // Empty when nothing in this window blocks removing the linked worktree `target`
    // (canonical); otherwise every reason, separated by "; ". Both deletion paths use it.
    [[nodiscard]] QString worktreeRemovalBlocker(const QString& target) const;
    bool archiveWorktree(const QString& path);
    bool restoreArchivedWorktree(const QString& path);
    [[nodiscard]] QVariantList archivedWorktrees() const;
    void setAgentWorkspaceVisible(bool visible);
    void refreshAgentWorktrees();
    void scheduleSidebarRefresh(bool repositoriesChanged = false);
    void refreshSidebarRepositories();
    void openSidebarConversation(const QString& worktreePath, const QString& conversationId);
    [[nodiscard]] QString sidebarRepositoryForPath(const QString& path);
    [[nodiscard]] QString activeSidebarRepository() const;
    ExplorerPanel* ensureExplorer();
    void setExplorerVisible(bool visible);
    void focusExplorer();
    GitChangesPanel* ensureGitChanges();
    void setSourceControlVisible(bool visible);
    void requestCurrentFileDiff();
    void openTranscriptFile(const QString& path, bool diff);
    // A link clicked in an agent transcript: a path, `path:line`, or a URL.
    void openTranscriptLink(const QString& link);
    void openTranscriptUrl(const QUrl& url);
    void requestFileDiff(const QString& filePath, GitDiffMode mode = GitDiffMode::Combined);
    void showDiff(const QString& filePath, GitDiffMode mode, const QString& diff);
    void updateGitSnapshot(const GitSnapshot& snapshot);
    void updateWorktrees(const QVector<GitWorktree>& worktrees);
    void prioritizeWorktree(QVector<GitWorktree>& worktrees) const;
    void rebuildWorktreeMenu();
    void updateGitActions();
    void rememberActiveFileForWorktree();
    [[nodiscard]] QString currentWorkspaceRelativeFile() const;
    void saveWorkspaceState() const;
    void restoreWorkspaceState();
    [[nodiscard]] QString workspaceStateKey() const;

    ThemeManager& theme_;
    AtlasIndex* atlasIndex_{nullptr};
    AppearanceSettings appearanceSettings_;
    EditorViewOptions viewOptions_;
    ActivityBar* activityBar_{nullptr};
    QSplitter* mainSplit_{nullptr};
    QSplitter* workspaceSplit_{nullptr};
    QSplitter* editorSplit_{nullptr};
    QTabWidget* tabs_{nullptr};
    QWidget* editorPane_{nullptr};
    FindReplaceBar* findBar_{nullptr};
    QLabel* cursorPositionLabel_{nullptr};
    QLabel* documentStateLabel_{nullptr};
    DocumentFormatControls* format_{nullptr};
    SearchPanel* searchPanel_{nullptr};
    QAction* searchPanelAction_{nullptr};
    QPointer<QThread> searchThread_;
    std::shared_ptr<std::atomic_bool> searchCancelled_;
    quint64 searchGeneration_{0};
    WorkspaceSearchOptions lastSearchOptions_;
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
    QAction* agentWorkspaceAction_{nullptr};
    QAction* markdownPreviewAction_{nullptr};
    QAction* terminalAction_{nullptr};
    QAction* remoteControlAction_{nullptr};
    QAction* schedulesAction_{nullptr};
    QAction* viewDiffAction_{nullptr};
    QMenu* gitMenu_{nullptr};
    QMenu* worktreeMenu_{nullptr};
    QMenu* recentFoldersMenu_{nullptr};
    QMenu* recentFilesMenu_{nullptr};
    QAction* reopenClosedTabAction_{nullptr};
    QTimer* highlightTimer_{nullptr};
    QStringList closedFilePaths_;
    QSplitter* documentSplit_{nullptr};
    EditorSplitPane* splitPane_{nullptr};
    EditorWidget* splitEditor_{nullptr};
    EditorWidget* splitSource_{nullptr};
    QAction* closeSplitAction_{nullptr};
    QAction* fullScreenAction_{nullptr};
    QAction* distractionFreeAction_{nullptr};
    QShortcut* exitFullScreenShortcut_{nullptr};
    struct DistractionFreeRestore final {
        bool explorer{false};
        bool search{false};
        bool sourceControl{false};
        bool terminal{false};
        bool markdownPreview{false};
        bool fullScreen{false};
        bool statusBar{false};
    } distractionFreeRestore_;
    bool splitFocused_{false};
    bool distractionFree_{false};
    bool maximizedBeforeFullScreen_{false};
    QuickOpenPopup* quickOpen_{nullptr};
    QuickOpenMode quickOpenMode_{QuickOpenMode::Commands};
    QAction* commandPaletteAction_{nullptr};
    QAction* backAction_{nullptr};
    QAction* forwardAction_{nullptr};
    QList<QPointer<QAction>> paletteActions_;
    QPointer<EditorWidget> symbolEditor_;
    QStringList workspaceFiles_;
    QStringList declinedFullScanRoots_;
    QString workspaceFilesRoot_;
    qint64 workspaceFilesIndexedAt_{0};
    bool workspaceFilesTruncated_{false};
    bool workspaceIndexing_{false};
    std::unique_ptr<SymbolIndex> symbolIndex_;
    QPointer<QThread> symbolIndexThread_;
    std::shared_ptr<std::atomic_bool> symbolIndexCancelled_;
    QTimer* symbolIndexIdleTimer_{nullptr};
    QList<WorkspaceSearchFileResult> definitionResults_;
    QString definitionSymbol_;
    QString definitionPattern_;
    bool definitionSearch_{false};
    QAction* goToDefinitionAction_{nullptr};
    QPointer<QThread> workspaceIndexThread_;
    std::shared_ptr<std::atomic_bool> workspaceIndexCancelled_;
    QList<NavigationLocation> backLocations_;
    QList<NavigationLocation> forwardLocations_;
    NavigationLocation lastLocation_;
    bool restoringNavigation_{false};
    ExplorerPanel* explorer_{nullptr};
    GitChangesPanel* gitChanges_{nullptr};
    MarkdownPreviewPane* markdownPreview_{nullptr};
    TerminalPanel* terminal_{nullptr};
    // Holds agentDetailTabs_ and, while one of its editors is searched, the find bar.
    QWidget* agentDetailPane_{nullptr};
    QTabWidget* agentDetailTabs_{nullptr};
    // Hide content pane keeps its tabs; the next opener shows it again.
    bool agentDetailPaneHidden_{false};
    // Whether the pane was last laid out as shown, and its share of the workspace width; the
    // split is resized only when the pane appears, so a width the user drags to stays.
    bool agentDetailPaneShown_{false};
    double agentDetailPaneShare_{0.45};
    QPointer<AtlasConversationPanel> conversationAtlasPanel_;
    QTabWidget* atlasTabs_{nullptr};
    RemoteControlView* remoteControl_{nullptr};
    QPointer<LinuxMachines> sharedLinuxMachines_;
    remote::DaemonClient* accountDaemon_{nullptr};
    TeamDirectory* teams_{nullptr};
    ScheduleWindows* schedules_{nullptr};
    ComposerSchedule* composerSchedule_{nullptr};
    void attachComposerSchedule();
    QPointer<TeamPage> teamPage_;
    QStringList remoteMachineIds_;
    QString selectedMachineId_{QStringLiteral("local")};
    MachineComposition* machines_{nullptr};
    // The Create worktree card asked to open Agent Workspace on what it creates.
    bool openCreatedWorktree_{false};
    SettingsWorkspacePage* settingsWorkspace_{nullptr};
    SettingsDialog* settingsDialog_{nullptr};
    AgentWorktreeCreator* agentWorktreeCreator_{nullptr};
    AgentWorkspaceView* agentWorkspace_{nullptr};
    AgentSessionManager* agentManager_{nullptr};
    GitService* git_{nullptr};
    GitSnapshot gitSnapshot_;
    QVector<GitWorktree> worktrees_;
    QHash<QString, QString> activeFileByWorktree_;
    QString workspaceRoot_;
    QString fileContextRoot_;
    QString pendingTranscriptDiff_;
    QString pendingTranscriptDiffRoot_;
    QString repositoryDisplayRoot_;
    QString promotedWorktreePath_;
    bool pendingConversationWorktree_{false};
    // The conversation whose picker asked for the worktree being created.
    QString pickerWorktreeAgentPath_;
    QString pickerWorktreeConversation_;
    qint64 pickerWorktreeRevision_{0};
    QString pendingDeleteConversationPath_;
    QStringList pendingDeleteWorktreePaths_;
    QString pendingDeleteWorktreeRepository_;
    bool pendingDeleteWorktreeForce_{false};
    QStringList pendingDeleteWorktreeSkipped_;
    QStringList sidebarRepositoryRoots_;
    QHash<QString, QString> sidebarRepositoryByPath_;
    QSet<QString> sidebarRepositoryLookups_;
    bool sidebarRefreshPending_{false};
    bool sidebarRepositoriesStale_{true};
    std::uint64_t tabAccessSequence_{0};
    int agentTerminalSerial_{0};
    bool agentWorkspaceMode_{false};
    bool compactConversationWindow_{false};
    QByteArray normalConversationGeometry_;
    QSize normalConversationMinimum_;
    QSize normalConversationSize_;
    bool compactSidebarVisible_{true};
    bool compactStatusVisible_{false};
    bool compactDetailsVisible_{false};
    QAction* compactConversationAction_{nullptr};
    // Diffs requested while the agent workspace is shown. Its status bar is hidden, so a
    // file with no uncommitted changes opens instead of reporting that nowhere visible.
    QSet<QString> agentDiffRequests_;
    bool atlasWorkspaceMode_{false};
    bool remoteWorkspaceMode_{false};
    bool settingsWorkspaceMode_{false};
    bool restoringWorkspaceState_{false};
    bool suppressWorkspaceRestore_{false};
    // The sidebar conversation a navigation is opening; activation publishes it directly.
    QString navigatingConversationId_;
    // Repository identity probes may touch Git and SQLite. Keep them owned by
    // the window so shutdown waits before QApplication and its SQL registry.
    QThreadPool repositoryLookupPool_;
};

} // namespace ketplus
