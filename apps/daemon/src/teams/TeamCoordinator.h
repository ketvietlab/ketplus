#pragma once

#include "teams/Team.h"
#include "teams/TeamRules.h"

#include <QHash>
#include <QObject>

class QTimer;

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

// A task's own branch and worktree, cut from the integration branch (ADR 0112, point 1).
struct TaskWorktree final {
    QString branch;
    QString worktree;
    QString baseCommit;
};

// A message the team sends into a member's conversation through the broker (ADR 0027).
struct TeamMessage final {
    qint64 teamId{0};
    QString teamName;
    QString toSession;
    // The member it comes from, or empty for the team itself (a digest, a decision).
    QString fromSession;
    // "assignment", "digest", "review", "decision" or "team".
    QString kind;
    QString taskId;
    QString body;
    bool expectReply{false};
};

// The session runtime as the coordinator sees it. SessionHost implements it; tests fake it.
// The changes of a task's branch against the integration branch it was cut from.
struct TaskDiff final {
    QString diff;
    QStringList files;
};

class TeamHostPort {
  public:
    virtual ~TeamHostPort() = default;
    [[nodiscard]] virtual std::optional<TeamConversation>
    teamConversation(const QString& sessionId) const = 0;
    // Creates the task's branch and worktree. Nothing, with `error` set, when git failed.
    virtual std::optional<TaskWorktree> createTaskWorktree(const Team& team, const Task& task,
                                                           QString* error) {
        (void)team;
        (void)task;
        *error = QStringLiteral("Worktrees are not available.");
        return std::nullopt;
    }
    // Points the conversation at a worktree before its next turn. Returns an error.
    virtual QString pointConversationAt(const QString& sessionId, const QString& worktree) {
        (void)sessionId;
        (void)worktree;
        return {};
    }
    // Whether the worktree has changes that are not committed; nothing when git failed.
    [[nodiscard]] virtual std::optional<bool> hasUncommittedWork(const QString& worktree) const {
        (void)worktree;
        return false;
    }
    [[nodiscard]] virtual QString headCommit(const QString& worktree) const {
        (void)worktree;
        return {};
    }
    // Queues the message; it starts a turn when the receiver is idle. Returns an error.
    // The task's branch against its base on the integration branch; nothing when git failed.
    [[nodiscard]] virtual std::optional<TaskDiff> taskDiff(const Team& team,
                                                           const Task& task) const {
        (void)team;
        (void)task;
        return std::nullopt;
    }
    // Paths changed in the task's worktree against its base, committed or not (ADR 0112 §4).
    [[nodiscard]] virtual std::optional<QStringList> changedPaths(const Team& team,
                                                                  const Task& task) const {
        (void)team;
        (void)task;
        return std::nullopt;
    }
    virtual QString postTeamMessage(const TeamMessage& message) {
        (void)message;
        return {};
    }
    // Shows a line in the conversation without starting a turn.
    virtual void noteInConversation(const QString& sessionId, const QJsonObject& note) {
        (void)sessionId;
        (void)note;
    }
};

// A new task as the leader or the user writes it.
struct TaskDraft final {
    QString title;
    QString description;
    QString lane;
    QStringList dependsOn;
    QStringList criteria;
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

    // Tasks (ADR 0112 §6), in TeamCoordinatorTasks.cpp. `actor` is "user", "daemon" or the
    // session id of the member that acts; the daemon refuses what its role may not do.
    TeamResult createTask(qint64 teamId, const TaskDraft& draft, const QString& actor,
                          QString* taskId = nullptr);
    // title, description, lane, depends_on (leader, user); pull_request and note (owner,
    // leader, user); met: [{index, met}] (owner, user).
    TeamResult updateTask(qint64 teamId, const QString& taskId, const QJsonObject& changes,
                          const QString& actor);
    TeamResult assignTask(qint64 teamId, const QString& taskId, const QString& owner,
                          const QString& actor);
    TeamResult handoverTask(qint64 teamId, const QString& taskId, const QString& owner,
                            const QString& reason, const QString& actor);
    // `note` is the question of a blocked task.
    TeamResult moveTask(qint64 teamId, const QString& taskId, const QString& to,
                        const QString& actor, const QString& note = {});
    TeamResult setCriteria(qint64 teamId, const QString& taskId, const QStringList& texts,
                           const QString& actor);
    // "user", "daemon", the member's role, or empty for a stranger.
    [[nodiscard]] QString actorRole(const Team& team, const QString& actor) const;
    [[nodiscard]] QString actorName(const Team& team, const QString& actor) const;

    // A member's turn ended (ADR 0112 §7): after the leader's turn the waiting digest is sent.
    void memberTurnEnded(const QString& sessionId);
    // ADR 0112 §2: the team's reviewer answered the review it was sent, or its turn failed
    // (`failure` set). `noFindings` is the verdict line of ADR 0067.
    void reviewFinished(const QString& reviewerSession, const QString& text, bool noFindings,
                        const QString& failure = {});

    // The quiet period after which an idle leader receives the digest (ADR 0112 §7).
    static constexpr int defaultQuietPeriodMs = 20000;
    void setQuietPeriod(int milliseconds) { quietPeriod_ = milliseconds; }

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
    // Moves a task and records it; no permission check. Adds the actionable updates.
    void applyMove(Team& team, Task& task, const QString& to, const QString& actor,
                   const QString& reason, QList<Update>& updates);
    // The leader's first turn, once the team has a goal (ADR 0112, #456/#458).
    void startLeader(qint64 teamId);
    // Starts a turn of `toSession` after the commit that caused it, counted against the
    // budget; held while the team is paused (TeamCoordinatorFlow.cpp).
    virtual void wake(const Team& team, const TeamMessage& message);
    // Sends the messages held while paused, in order, once the team runs again.
    void releaseHeld(qint64 teamId);
    // ADR 0112 §4 (TeamCoordinatorLanes.cpp): after a developer's turn, compares the files its
    // open tasks changed with its lane and the shared paths.
    void checkLanes(qint64 teamId, const QString& developer);
    // After a lane or the shared paths changed: clears the flagged paths they now cover.
    void recheckLanes(Team& team, QList<Update>& updates);
    // Sets a task's outside paths and records the change; false when nothing changed.
    bool flagOutside(Team& team, Task& task, const QStringList& outside, const QString& why,
                     QList<Update>& updates);
    // Sends the next queued task to an idle reviewer (TeamCoordinatorReviews.cpp).
    void pumpReviews(qint64 teamId);
    [[nodiscard]] static QString reviewText(const Team& team, const Task& task,
                                            const std::optional<TaskDiff>& diff);
    // Restarts the quiet period of the team's digest.
    void scheduleDigest(qint64 teamId);
    // One digest of every update since the last one, when one of them is actionable.
    void sendDigest(qint64 teamId);
    // Points the owner at the task's worktree and sends the assignment.
    void deliverAssignment(const Team& team, const Task& task, const QString& from, bool handover,
                           const QString& reason = {});
    [[nodiscard]] static QString assignmentText(const Team& team, const Task& task,
                                                const QString& fromName, bool handover,
                                                const QString& reason);

    LocalJournal* journal_{nullptr};
    TeamHostPort* host_{nullptr};
    QHash<qint64, Team> teams_;
    QHash<QString, qint64> teamBySession_;
    QHash<qint64, QTimer*> digestTimers_;
    int quietPeriod_{defaultQuietPeriodMs};
};

} // namespace teams
} // namespace ketplus::daemon
