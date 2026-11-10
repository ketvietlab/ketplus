#pragma once

#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QWidget>

namespace ket {
class ConversationRow;
}
class QVBoxLayout;

namespace ketplus {

// A team's folder inside its repository (ADR 0112, #464). `members` are conversation maps in the
// folder's order, leader first; each carries "teamRole" and its role tag first in "tags".
struct SidebarTeam final {
    QString id;
    QString name;
    int openDecisions{0};
    bool full{false};
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
    bool promoted{false};
    // Listed before the repository's other conversations; none without teams/1.
    QVector<SidebarTeam> teams;

    friend bool operator==(const SidebarRepository& left, const SidebarRepository& right) {
        return left.path == right.path && left.name == right.name && left.active == right.active &&
               left.conversations == right.conversations && left.promoted == right.promoted &&
               left.teams == right.teams;
    }
};

// The order the phones use too (ketplus-app MobileWorkspaceData): conversations by
// "lastTurnStartedAt", newest first, then title and id; repositories by their newest
// conversation, then name and path. An explicit conversation repository switch promotes its target.
void sortSidebarRepositories(QVector<SidebarRepository>& repositories);

class SidebarRepositoryList final : public QWidget {
    Q_OBJECT

  public:
    explicit SidebarRepositoryList(QWidget* parent = nullptr);

    void applyZoom(int zoomPercent);
    void setTagFilter(const QString& tag);
    void setRepositories(const QVector<SidebarRepository>& repositories);
    // With teams/1 the repository's + offers New team… beside New conversation.
    void setTeamsAvailable(bool available) { teamsAvailable_ = available; }
    // A conversation of `repositoryPath` dragged onto the folder of `teamId` joins as a developer:
    // same repository, a team that is not full, and not on a team already.
    [[nodiscard]] bool acceptsDrop(const QString& teamId, const QString& repositoryPath,
                                   const QString& sessionId) const;

    static constexpr char kConversationMimeType[] = "application/x-ketplus-conversation";

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
    void teamJoinRequested(const QString& teamId, const QString& sessionId);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    ket::ConversationRow* createRow(QWidget* group, const QVariantMap& conversation);
    QWidget* createTeamFolder(QWidget* group, const SidebarRepository& repository,
                              const SidebarTeam& team);
    void updateTeamFolders(const QVector<SidebarRepository>& repositories);
    // The folders' height inside `group`, laid out for the current tag filter and collapse.
    int presentTeamFolders(QWidget* group, bool repositoryCollapsed, int rowHeight);
    bool handleDrag(QObject* watched, QEvent* event);
    void onRepositoryAction(const QString& path, QWidget* anchor);
    [[nodiscard]] bool rowMatchesFilter(const ket::ConversationRow* row) const;
    void markConversationSeen(const QString& conversationId);
    void updateRows(const QVector<SidebarRepository>& repositories);
    void applyPresentation();
    void onRepositoryHeaderClicked(const QString& path);
    void persistCollapsedRepositories() const;

    QVBoxLayout* layout_{nullptr};
    QVector<SidebarRepository> repositories_;
    QStringList structure_;
    QVector<ket::ConversationRow*> rows_;
    QVector<QWidget*> headers_;
    QVector<QWidget*> groups_;
    QVector<QWidget*> folders_;
    QSet<QString> collapsedPaths_;
    QSet<QString> collapsedTeams_;
    QPointer<ket::ConversationRow> pressedRow_;
    QPoint pressPosition_;
    bool teamsAvailable_{false};
    QSet<QString> showAllPaths_;
    // Completion is new until the user opens that conversation during this application session.
    QSet<QString> seenConversationIds_;
    QString tagFilter_;
    bool built_{false};
};

} // namespace ketplus
