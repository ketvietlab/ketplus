#pragma once

#include "teams/Team.h"
#include "teams/TeamRules.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>

class QTimer;

#include <functional>
#include <optional>

namespace ketplus::daemon {

class LocalJournal;
struct TeamUpdateRecord;

namespace teams {

// What the coordinator reads about a conversation (ADR 0114).
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

// A task's own branch and worktree, cut from the integration branch (ADR 0114, point 1).
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
    // "assignment", "digest", "review", "decision", "nudge", "stall", "wait",
    // "message_failed" or "team".
    QString kind;
    QString taskId;
    QString body;
    bool expectReply{false};
    // The id the queued message gets, when the team must know it before it is sent (a
    // review); empty lets the broker choose one.
    QString id;
};

// The session runtime as the coordinator sees it. SessionHost implements it; tests fake it.
// The changes of a task's branch against the integration branch it was cut from.
struct TaskDiff final {
    QString diff;
    QStringList files;
};

// What git knows about one task, for a check the daemon makes by itself.
struct TaskFacts final {
    // Merge: the commit the task's branch points at, and whether the integration branch
    // contains the branch. Nothing where git could not tell.
    QString head;
    std::optional<bool> merged;
    // Lane: the paths the task's worktree changed.
    std::optional<QStringList> changed;
};
enum class TaskQuestion { Merge, Lane };
using TaskFactsDone = std::function<void(const QHash<QString, TaskFacts>&)>;
// What git answered when a task's worktree was asked for: the worktree, or why there is none.
struct WorktreeAnswer final {
    std::optional<TaskWorktree> worktree;
    QString error;
};
// What a hand to review reads from git, and the worktree and branch it was read for: whether
// work is uncommitted (nothing when git failed) and the commit the branch points at.
struct HandoffFacts final {
    QString worktree;
    QString branch;
    std::optional<bool> dirty;
    QString head;
};
using WorktreeDone = std::function<void(const WorktreeAnswer&)>;
using HandoffDone = std::function<void(const HandoffFacts&)>;
using TaskDiffDone = std::function<void(const std::optional<TaskDiff>&)>;

class TeamHostPort {
  public:
    virtual ~TeamHostPort() = default;
    // Asks git about the tasks for one check and answers `done` on the caller's thread, by task
    // id. Here it answers before it returns. The session runtime asks git off its event loop
    // and answers later, so the caller applies the answer to the team as it then is.
    virtual void askTasks(const Team& team, const QList<Task>& tasks, TaskQuestion question,
                          TaskFactsDone done) {
        QHash<QString, TaskFacts> facts;
        for (const Task& task : tasks)
            facts.insert(task.id, question == TaskQuestion::Merge
                                      ? TaskFacts{.head = taskCommit(team, task),
                                                  .merged = isMerged(team, task),
                                                  .changed = std::nullopt}
                                      : TaskFacts{.head = {},
                                                  .merged = std::nullopt,
                                                  .changed = changedPaths(team, task)});
        done(facts);
    }
    // Reads the task's diff for its review and answers `done` on the caller's thread: before
    // it returns here, later where the session runtime reads it off its event loop.
    virtual void askDiff(const Team& team, const Task& task, TaskDiffDone done) {
        done(taskDiff(team, task));
    }
    // Creates the task's branch and worktree, and reads what a hand to review needs, for a
    // command that was prepared (TeamCoordinator::prepare). Both answer `done` on the caller's
    // thread: before they return here, later where the session runtime asks git off its
    // event loop.
    virtual void askWorktree(const Team& team, const Task& task, WorktreeDone done) {
        WorktreeAnswer answer;
        answer.worktree = createTaskWorktree(team, task, &answer.error);
        done(answer);
    }
    virtual void askHandoff(const Team& team, const Task& task, HandoffDone done) {
        done({.worktree = task.worktree,
              .branch = task.branch,
              .dirty = hasUncommittedWork(task.worktree),
              .head = taskCommit(team, task)});
    }
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
    // The commit the task's branch points at in the team's repository: what is reviewed
    // and merged, whatever a worktree has checked out. Empty when git could not tell.
    [[nodiscard]] virtual QString taskCommit(const Team& team, const Task& task) const {
        (void)team;
        (void)task;
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
    // Paths changed in the task's worktree against its base, committed or not (ADR 0114 §4).
    [[nodiscard]] virtual std::optional<QStringList> changedPaths(const Team& team,
                                                                  const Task& task) const {
        (void)team;
        (void)task;
        return std::nullopt;
    }
    // Whether the integration branch contains the task's branch (ADR 0114, open point 3);
    // nothing when git failed.
    [[nodiscard]] virtual std::optional<bool> isMerged(const Team& team, const Task& task) const {
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

    // Reads the stored teams, at start and again when that failed. A journal that cannot
    // be read keeps the teams in memory as they were.
    void load();
    [[nodiscard]] bool isLoaded() const noexcept { return loaded_; }

    [[nodiscard]] QList<const Team*> teams(const QString& repository = {}) const;
    [[nodiscard]] const Team* team(qint64 teamId) const;
    [[nodiscard]] const Team* teamOf(const QString& sessionId) const;
    [[nodiscard]] bool isMember(const QString& sessionId) const { return teamOf(sessionId); }
    // The team as clients receive it: the document plus each member's number, title and agent.
    [[nodiscard]] QJsonObject teamJson(const Team& team) const;
    // ADR 0115: the team for team.list and team.changed: teamJson without its tasks and
    // decisions, marked "directory". Its size follows the members, so the sidebar always
    // receives every team; a page reads the rest with team.get.
    [[nodiscard]] QJsonObject directoryJson(const Team& team) const;
    [[nodiscard]] QJsonArray updatesJson(qint64 teamId, int limit = defaultPage,
                                         qint64 beforeId = 0) const;

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

    // Tasks (ADR 0114 §6), in TeamCoordinatorTasks.cpp. `actor` is "user", "daemon" or the
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
    // TeamCoordinatorGit.cpp. An assignment and a handover make the task's worktree, and a
    // hand to review reads the worktree, through the host on the caller's thread. prepare()
    // asks the host first, where it may answer later, and then calls `then`: the command is
    // run there as always and finds the answer instead of asking. `then` is called at once
    // for a command that would be refused or asks git nothing. One worktree is asked for a
    // task however many commands wait for it.
    struct TaskRequest final {
        enum class Kind { Assign, Handover, Move };
        Kind kind{Kind::Assign};
        qint64 teamId{0};
        QString taskId;
        QString actor;
        QString owner;
        QString reason;
        QString to;
    };
    void prepare(const TaskRequest& request, std::function<void()> then);
    // "user", "daemon", the member's role, or empty for a stranger.
    [[nodiscard]] QString actorRole(const Team& team, const QString& actor) const;
    [[nodiscard]] QString actorName(const Team& team, const QString& actor) const;

    // A member's turn ended (ADR 0114 §7): after the leader's turn the waiting digest is sent,
    // and the team is watched for a stall (§8). `stoppedByUser` is the user's Stop; nothing
    // keeps what the member's last turn said (its worker exited, no turn ended).
    void memberTurnEnded(const QString& sessionId, std::optional<bool> stoppedByUser = false);
    // ADR 0114 §8 (TeamCoordinatorStall.cpp). A team with work left and nobody working is
    // moved on: a developer that left its task in progress is nudged once, then the leader
    // is woken, then the user is asked.
    static constexpr int defaultStallPeriodMs = 3 * 60 * 1000;
    static constexpr int maxStallWakes = 2;
    void setStallPeriod(int milliseconds) { stallPeriod_ = milliseconds; }
    void checkStall(qint64 teamId);
    // ADR 0114 §8 (TeamCoordinatorWaits.cpp). A leader or developer that waits for something
    // outside the team says so and is woken after `minutes` to check again. Until then the
    // watchdog leaves it, and the team it keeps waiting, alone. Kept in memory: after a
    // restart the watchdog takes over.
    static constexpr int maxWaitMinutes = 60;
    TeamResult wait(qint64 teamId, const QString& sessionId, int minutes, const QString& reason,
                    const QString& taskId = {});
    [[nodiscard]] bool isWaiting(const QString& sessionId) const {
        return waits_.contains(sessionId);
    }
    void setWaitMinuteForTesting(int milliseconds) { waitMinuteMs_ = milliseconds; }
    // A message the member sent to `peer`, a conversation outside its team, failed or was
    // never answered (`reason`, the broker's): the member is woken to decide what to do.
    void outsideMessageFailed(const QString& sessionId, const QString& peer, const QString& reason);
    // ADR 0114 §3 (TeamCoordinatorDecisions.cpp). Only the leader raises a decision; only the
    // user answers one, with one of its choices or free text.
    TeamResult raiseQuestion(qint64 teamId, const QJsonObject& draft, const QString& actor,
                             QString* decisionId = nullptr);
    TeamResult answerDecision(qint64 teamId, const QString& decisionId, const QString& answer,
                              const QString& actor);
    TeamResult withdrawDecision(qint64 teamId, const QString& decisionId, const QString& reason,
                                const QString& actor);

    // ADR 0114 §5 (TeamCoordinatorMerges.cpp): marks every Ready task the integration branch
    // contains as done. Run when a member's turn ends and when a client reads the team. Git
    // is asked through the host, which may answer later; one question is open for a team at a
    // time, and a check asked for meanwhile runs when it is answered.
    void checkMerged(qint64 teamId);
    // Whether a merge check, a lane check, a review's diff or a prepared command is still
    // with git.
    [[nodiscard]] bool checksOpen() const {
        return !mergeChecks_.isEmpty() || !laneChecks_.isEmpty() || !reviewDiffs_.isEmpty() ||
               !worktreeWaits_.isEmpty() || handoffsAsked_ > 0;
    }

    // ADR 0114 §2: the team's reviewer answered the review it was sent, or its turn failed
    // (`failure` set). `noFindings` is the verdict line of ADR 0067.
    // `messageId` is the review request that ended. A review is bound to its request from
    // the moment it starts, sent or still held, so a request with another id is ignored.
    void reviewFinished(const QString& reviewerSession, const QString& messageId,
                        const QString& text, bool noFindings, const QString& failure = {});

    // The quiet period after which an idle leader receives the digest (ADR 0114 §7).
    static constexpr int defaultQuietPeriodMs = 20000;
    void setQuietPeriod(int milliseconds) { quietPeriod_ = milliseconds; }

    // A deleted conversation leaves its team (ADR 0114, open point 4).
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

    // The team's Updates from the journal, within the bounds of Team.h.
    [[nodiscard]] QList<TeamUpdateRecord> updateRecords(qint64 teamId, bool undigestedOnly,
                                                        int limit, qint64 beforeId = 0) const;
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
    // The leader's first turn, once the team has a goal (ADR 0114, #456/#458).
    void startLeader(qint64 teamId);
    // Starts a turn of `toSession` after the commit that caused it, counted against the
    // budget; held while the team is paused (TeamCoordinatorFlow.cpp).
    virtual void wake(const Team& team, const TeamMessage& message);
    // Sends the messages held while paused, in order, once the team runs again.
    void releaseHeld(qint64 teamId);
    // ADR 0114 §4 (TeamCoordinatorLanes.cpp): after a developer's turn, compares the files its
    // open tasks changed with its lane and the shared paths.
    void checkLanes(qint64 teamId, const QString& developer);
    void applyLaneFacts(qint64 teamId, const QString& developer, const QList<Task>& asked,
                        const QHash<QString, TaskFacts>& facts);
    void applyMergeFacts(qint64 teamId, const QList<Task>& asked,
                         const QHash<QString, TaskFacts>& facts);
    // After a lane or the shared paths changed: clears the flagged paths they now cover.
    void recheckLanes(Team& team, QList<Update>& updates);
    // Sets a task's outside paths and records the change; false when nothing changed.
    bool flagOutside(Team& team, Task& task, const QStringList& outside, const QString& why,
                     QList<Update>& updates);
    // Offers each Ready task whose dependencies are done for merging, once: an actionable
    // update for the leader, and under mergePolicy "user" a "Merge T-n?" decision.
    void offerMerges(Team& team, QList<Update>& updates);
    // Sends the next queued task to an idle reviewer (TeamCoordinatorReviews.cpp). The diff is
    // read through the host first, which may answer later; nothing of the team changes until
    // then, so a restart meanwhile leaves the task queued.
    void pumpReviews(qint64 teamId);
    // Starts the review the diff was read for, if that task is still the next one as it was.
    void startReview(qint64 teamId, const Task& asked, const QString& repository,
                     const QString& integration, const std::optional<TaskDiff>& diff);
    [[nodiscard]] static QString reviewText(const Team& team, const Task& task,
                                            const std::optional<TaskDiff>& diff);
    // Restarts the quiet period of the team's digest.
    void scheduleDigest(qint64 teamId);
    // One digest of every update since the last one, when one of them is actionable.
    void sendDigest(qint64 teamId);
    // ADR 0114, open point 5: holds an assignment the leader makes before the first plan is
    // approved, behind one "Start these N tasks?" decision. True when it was held.
    bool holdForFirstPlan(qint64 teamId, const QString& taskId, const QString& actor);
    void askFirstPlan(Team& team);
    void askFirstPlanAgain(qint64 teamId);
    // Points the owner at the task's worktree and sends the assignment.
    // Moves a member out of the worktrees it was handed out of and forgets each move that
    // is settled: done, or no longer needed. Returns why one is still waiting, if any.
    QString settleRelocations(qint64 teamId, const QString& sessionId);
    void deliverAssignment(const Team& team, const Task& task, const QString& from, bool handover,
                           const QString& reason = {});
    [[nodiscard]] static QString assignmentText(const Team& team, const Task& task,
                                                const QString& fromName, bool handover,
                                                const QString& reason);
    // Why the command is refused before git is asked for anything; nothing when it may go on.
    [[nodiscard]] std::optional<TeamError> assignRefusal(const Team& team, const Task& task,
                                                         const QString& owner,
                                                         const QString& actor) const;
    [[nodiscard]] std::optional<TeamError> handoverRefusal(const Team& team, const Task& task,
                                                           const QString& owner,
                                                           const QString& reason,
                                                           const QString& actor) const;
    // The task's worktree and what its hand to review reads: the answer prepare() got, or the
    // host's on this thread.
    [[nodiscard]] WorktreeAnswer worktreeFor(const Team& team, const Task& task);
    [[nodiscard]] HandoffFacts handoffFor(const Team& team, const Task& task) const;
    void worktreeAnswered(qint64 teamId, const QString& taskId, const WorktreeAnswer& answer);

    LocalJournal* journal_{nullptr};
    TeamHostPort* host_{nullptr};
    QHash<qint64, Team> teams_;
    // Checks whose question to git is open, and those asked for again meanwhile: a team id
    // for the merge check, "<team>/<developer>" for the lane check.
    // Teams whose next review waits for git to read its diff.
    QSet<qint64> reviewDiffs_;
    QSet<qint64> mergeChecks_;
    QSet<qint64> mergeChecksAgain_;
    QSet<QString> laneChecks_;
    QSet<QString> laneChecksAgain_;
    // Prepared commands, by "<team>/<task>": those waiting for the task's worktree, and the
    // answers that are there while the commands they were asked for run.
    QHash<QString, QList<std::function<void()>>> worktreeWaits_;
    QHash<QString, WorktreeAnswer> worktrees_;
    QHash<QString, HandoffFacts> handoffs_;
    int handoffsAsked_{0};
    QHash<QString, qint64> teamBySession_;
    bool loaded_{false};
    QHash<qint64, QTimer*> digestTimers_;
    int quietPeriod_{defaultQuietPeriodMs};

    // What the watchdog did since the team's work last changed. Not stored: a restart
    // starts the count again, which only costs one more wake.
    struct Stall final {
        QString signature;
        int wakes{0};
        QSet<QString> nudged;
        // Members whose last turn the user stopped; they are not woken on their own.
        QSet<QString> stopped;
    };
    void watchForStall(qint64 teamId);
    QHash<qint64, QTimer*> stallTimers_;
    QHash<qint64, Stall> stalls_;
    int stallPeriod_{defaultStallPeriodMs};

    struct Wait final {
        QTimer* timer{nullptr};
        QString reason;
        QString taskId;
    };
    void cancelWait(const QString& sessionId);
    void waitEnded(const QString& sessionId);
    // By member session.
    QHash<QString, Wait> waits_;
    int waitMinuteMs_{60 * 1000};
};

} // namespace teams
} // namespace ketplus::daemon
