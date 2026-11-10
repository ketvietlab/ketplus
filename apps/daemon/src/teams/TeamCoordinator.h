#pragma once

#include "teams/Team.h"
#include "teams/TeamRules.h"

#include <QHash>
#include <QObject>

#include <optional>

namespace ketplus::daemon {

class LocalJournal;
struct TeamUpdateRecord;

namespace teams {

// What the coordinator reads about a conversation (ADR 0112).
struct TeamConversation final {
    QString sessionId;
    qint64 generation{0};
    QString number;
    QString title;
    QString provider;
    QString model;
    QString effort;
    QString worktree;
    // The repository's main worktree, the key a team belongs to.
    QString repository;
    bool idle{false};
};

// The session runtime as the coordinator sees it. SessionHost implements it; tests fake it.
class TeamHostPort {
  public:
    virtual ~TeamHostPort() = default;
    [[nodiscard]] virtual std::optional<TeamConversation>
    teamConversation(const QString& sessionId) const = 0;
};

struct TeamResult final {
    std::optional<TeamError> error;
    qint64 teamId{0};

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Owns every team of the daemon: the rules, the journal and the change events. Every change
// goes through commit(), which raises the revision, stores the document and the updates and
// emits teamChanged.
class TeamCoordinator : public QObject {
    Q_OBJECT

  public:
    TeamCoordinator(LocalJournal* journal, TeamHostPort* host, QObject* parent = nullptr);
    ~TeamCoordinator() override;

    // Reads the stored teams. Called once at start.
    void load();

    [[nodiscard]] QList<const Team*> teams(const QString& repository = {}) const;
    [[nodiscard]] const Team* team(qint64 teamId) const;
    [[nodiscard]] const Team* teamOf(const QString& sessionId) const;
    [[nodiscard]] bool isMember(const QString& sessionId) const { return teamOf(sessionId); }
    // The team as clients receive it: the document plus each member's number, title and agent.
    [[nodiscard]] QJsonObject teamJson(const Team& team) const;
    [[nodiscard]] QJsonArray updatesJson(qint64 teamId, int limit = 200) const;

    // `draft` carries the settings and the members with their session ids. Nothing is stored
    // when a rule refuses it.
    TeamResult create(Team draft, const QString& actor);
    // name, goal, integration_branch, merge_policy, leader_writes_requirements, paused,
    // shared_paths and max_rounds; other keys are ignored.
    TeamResult update(qint64 teamId, const QJsonObject& changes, const QString& actor);
    std::optional<TeamError> remove(qint64 teamId);
    TeamResult addMember(qint64 teamId, Member member, const QString& actor);
    TeamResult updateMember(qint64 teamId, const QString& sessionId, const QString& lane,
                            const QStringList& paths, const QString& actor,
                            const QString& reason = {});
    TeamResult removeMember(qint64 teamId, const QString& sessionId, const QString& actor);

    // A deleted conversation leaves its team (ADR 0112, open point 4).
    void conversationDeleted(const QString& sessionId);
    // Archiving a member is refused; a reason, or nothing when it may be archived.
    [[nodiscard]] std::optional<TeamError> archiveRefusal(const QString& sessionId) const;

  signals:
    void teamChanged(const QJsonObject& team);
    void teamRemoved(const QString& teamId, const QString& repository);

  protected:
    // One Updates entry to store with the next commit.
    struct Update final {
        QString kind;
        QString text;
        QString taskId;
        bool actionable{false};
        QJsonObject data;
    };

    [[nodiscard]] Team* mutableTeam(qint64 teamId);
    [[nodiscard]] Team* mutableTeamOf(const QString& sessionId);
    // Stores `team` with revision + 1 and the updates, then emits teamChanged. On a journal
    // failure the in-memory team is restored from `before`.
    std::optional<TeamError> commit(Team& team, const Team& before, const QString& actor,
                                    const QList<Update>& updates);
    // Leaves the developer's open tasks unassigned, with an entry in their history.
    void unassignTasksOf(Team& team, const QString& sessionId, const QString& actor,
                         const QString& reason, QList<Update>& updates);
    Decision& raiseDecision(Team& team, const QString& kind, const QString& question,
                            const QString& context, const QStringList& choices,
                            const QString& recommended, const QString& taskId,
                            const QString& raisedBy);
    static void addHistory(Task& task, const QString& actor, const QString& kind,
                           const QString& text, const QJsonObject& data = {});
    [[nodiscard]] std::optional<Candidate> candidate(const QString& sessionId) const;

    LocalJournal* journal_{nullptr};
    TeamHostPort* host_{nullptr};
    QHash<qint64, Team> teams_;
    QHash<QString, qint64> teamBySession_;
};

} // namespace teams
} // namespace ketplus::daemon
