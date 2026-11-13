#pragma once

#include "workspace/SidebarRepositoryPins.h"

#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QWidget>

namespace ket {
class SidebarRepositoryHeader;
class SidebarRow;
class SidebarSectionLabel;
class SidebarTeamBranch;
class SidebarTreeGuide;
} // namespace ket
class QMenu;
class QVBoxLayout;

namespace ketplus {

// A team's folder inside its repository (ADR 0114, #464). `members` are conversation maps in the
// folder's order, leader first; each carries "teamRole" and its locked role tag in "tags", whose
// label the row says as the member's role.
struct SidebarTeam final {
    QString id;
    QString name;
    int openDecisions{0};
    bool full{false};
    // T-89: why the branch's + takes no member (team_roles::fullReason), or empty when it can.
    QString addRefusal;
    QVariantList members;

    friend bool operator==(const SidebarTeam&, const SidebarTeam&) = default;
};

// One repository in the product sidebar. Conversations are the navigation unit;
// each conversation map carries the worktree it runs in as "path" and whether it
// is the window's current conversation as "selected".
struct SidebarRepository final {
    QString path;
    QString name;
    bool active{false};
    QVariantList conversations;
    // Listed before the repository's other conversations; none without teams/1.
    QVector<SidebarTeam> teams;
    // T-92: whether the user may pin it (this Mac's repositories; not another machine's), and
    // its place among the pinned ones, from 0, or -1. SidebarRepositoryList sets the place of a
    // pinnable repository from its pins.
    bool pinnable{false};
    int pinOrder{-1};

    friend bool operator==(const SidebarRepository& left, const SidebarRepository& right) {
        return left.path == right.path && left.name == right.name && left.active == right.active &&
               left.conversations == right.conversations && left.teams == right.teams &&
               left.pinnable == right.pinnable && left.pinOrder == right.pinOrder;
    }
};

// Conversations by "lastTurnStartedAt", newest first, then title and id. Repositories: the
// pinned ones first, in the order they were pinned (T-92); then by their newest turn start of
// anything they show, their own conversations and their teams' members alike; then name and
// path. The phones (the companion mobile client) order conversations and unpinned repositories
// by recency too.
void sortSidebarRepositories(QVector<SidebarRepository>& repositories);
// The newest turn start among a repository's conversations and its teams' members; 0 for none.
[[nodiscard]] qint64 sidebarRepositoryRecency(const SidebarRepository& repository);

// The repository list of ketPlus Pro is the tree of the Qt mock (mockups/qt, "Sidebar tree"):
// a repository is the root, its teams are branches under it, "Conversations (n)" labels the
// conversations of no team where the repository has teams, and every conversation is one line.
// The parts are ket-qt's sidebar tree (KetSidebarTree.h); they share the columns of their level,
// so the text edges hold at every zoom.
class SidebarRepositoryList final : public QWidget {
    Q_OBJECT

  public:
    explicit SidebarRepositoryList(QWidget* parent = nullptr);

    void applyZoom(int zoomPercent);
    void setRepositories(const QVector<SidebarRepository>& repositories);
    // With teams/1 the repository's + offers New team… beside New conversation.
    void setTeamsAvailable(bool available) { teamsAvailable_ = available; }
    // A conversation of `repositoryPath` dragged onto the folder of `teamId` joins as a developer:
    // same repository, a team that is not full, and not on a team already.
    [[nodiscard]] bool acceptsDrop(const QString& teamId, const QString& repositoryPath,
                                   const QString& sessionId) const;

    static constexpr char kConversationMimeType[] = "application/x-ketplus-conversation";

    // What the tree is built on, as the mock's Sidebar.layoutProblems checks it: a repository's
    // name, its "Conversations" label and its conversations' titles share a left edge; a team's
    // name and its members' titles the same; a team's mark stands over its members' marks; every
    // conversation is one line high. Empty when the visible tree holds all of them.
    [[nodiscard]] QStringList layoutProblems() const;

    // T-92: pins a pinnable repository to the top, after those pinned before, or unpins it.
    void setRepositoryPinned(const QString& path, bool pinned);
    [[nodiscard]] const SidebarRepositoryPins& pins() const { return pins_; }
    // The repository header's context menu: Pin to top or Unpin. Nothing for a repository
    // that cannot be pinned. The caller owns it.
    [[nodiscard]] QMenu* createRepositoryMenu(const QString& path, QWidget* parent);

  signals:
    // A conversation changed visual status without rebuilding its row; the owner redraws its
    // Két Symbol pixmap using the current palette.
    void iconPresentationChanged();
    void repositoryRequested(const QString& path);
    void newConversationRequested(const QString& repositoryPath);
    void conversationRenameRequested(const QString& worktreePath, const QString& conversationId);
    void conversationRequested(const QString& worktreePath, const QString& conversationId);
    void conversationContextMenuRequested(const QString& worktreePath,
                                          const QString& conversationId);
    void newTeamRequested(const QString& repositoryPath);
    void teamRequested(const QString& teamId);
    void teamMenuRequested(const QString& teamId);
    // T-89: the team branch's + was pressed, by mouse or keyboard, on a team with room.
    void teamAddMemberRequested(const QString& teamId);
    void teamJoinRequested(const QString& teamId, const QString& sessionId);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    ket::SidebarRow* createRow(QWidget* parent, const QVariantMap& conversation, int level);
    void updateRow(ket::SidebarRow* row, const QVariantMap& conversation) const;
    QWidget* createTeamFolder(QWidget* group, const SidebarRepository& repository,
                              const SidebarTeam& team);
    void updateTeamFolders(const QVector<SidebarRepository>& repositories);
    void presentTeamFolders(QWidget* group);
    // A team's fold is stored for its repository: team ids are numbered per daemon, so another
    // machine's `teams/1` is a different team.
    static QString teamFoldKey(const QString& repositoryPath, const QString& teamId);
    void loadCollapsedTeams();
    // Forgets the folds of teams that left a repository that lists teams; a repository with no
    // team listed (not loaded yet, or its last team removed) keeps its folds.
    void forgetMissingTeams(const QVector<SidebarRepository>& repositories);
    [[nodiscard]] int waitingCount(const SidebarRepository& repository) const;
    bool handleDrag(QObject* watched, QEvent* event);
    void onRepositoryAction(const QString& path, QWidget* anchor);
    void markConversationSeen(const QString& conversationId);
    void updateRows(const QVector<SidebarRepository>& repositories);
    void applyPresentation();
    void onRepositoryHeaderClicked(const QString& path);
    void persistCollapsedRepositories() const;
    // T-92: a small pin drawn at the end of a pinned repository's name row.
    void presentPinMark(ket::SidebarRepositoryHeader* header, bool pinned) const;

    QVBoxLayout* layout_{nullptr};
    QVector<SidebarRepository> repositories_;
    QStringList structure_;
    QVector<ket::SidebarRow*> rows_;
    QVector<ket::SidebarRepositoryHeader*> headers_;
    QVector<QWidget*> groups_;
    QVector<QWidget*> folders_;
    QSet<QString> collapsedPaths_;
    SidebarRepositoryPins pins_;
    QSet<QString> collapsedTeams_;
    bool collapsedTeamsLoaded_{false};
    QPointer<ket::SidebarRow> pressedRow_;
    QPoint pressPosition_;
    bool teamsAvailable_{false};
    QSet<QString> showAllPaths_;
    // Completion is new until the user opens that conversation during this application session.
    QSet<QString> seenConversationIds_;
    bool built_{false};
};

} // namespace ketplus
