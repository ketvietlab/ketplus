#pragma once

#include "teams/Team.h"
#include "teams/TeamRules.h"
#include "teams/TeamStorage.h"

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
    // An assignment: the worktree its receiver is pointed at before the message is sent.
    QString pointAt;
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
// ADR 0118: what the remote says about one pull request. `state` is "open", "merged" or
// "closed"; empty with `error` set when the remote could not tell.
struct PullRequestFact final {
    QString state;
    QString mergeCommit;
    QString error;
};
using PullRequestsDone = std::function<void(const QHash<QString, PullRequestFact>&)>;
// The error of pointing a conversation at a worktree; empty when it worked.
using PointDone = std::function<void(const QString&)>;

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
    // ADR 0118: asks the remote about the pull requests, by URL, and answers `done` on the
    // caller's thread. Without a way to ask, every one is unknown and the leader closes the
    // task by hand.
    virtual void askPullRequests(const Team& team, const QStringList& urls, PullRequestsDone done) {
        (void)team;
        QHash<QString, PullRequestFact> facts;
        for (const QString& url : urls)
            facts.insert(url, {.state = {},
                               .mergeCommit = {},
                               .error = QStringLiteral("Pull requests cannot be checked here.")});
        done(facts);
    }
    [[nodiscard]] virtual std::optional<TeamConversation>
    teamConversation(const QString& sessionId) const = 0;
    // The numbered conversation on this computer (`@agent/<number>`), for the goal's mentions.
    [[nodiscard]] virtual std::optional<TeamConversation>
    conversationByNumber(const QString& number) const {
        (void)number;
        return std::nullopt;
    }
    // The repository's main worktree the conversation works in: the key a team belongs to.
    // Asked only for a conversation that is to join a team, because the session runtime may
    // have to ask git for it.
    [[nodiscard]] virtual QString conversationRepository(const QString& sessionId) const = 0;
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
    // The same, with git off the caller's thread: `done` runs on it once the switch is
    // stored, so a slow git holds up no command and no conversation's events (T-28).
    virtual void askPointConversationAt(const QString& sessionId, const QString& worktree,
                                        PointDone done) {
        done(pointConversationAt(sessionId, worktree));
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
    // Whether a message with this id was queued already: the outbox sends each entry once,
    // also when the daemon stopped between sending it and forgetting it (T-28).
    [[nodiscard]] virtual bool teamMessagePosted(const QString& id) const {
        (void)id;
        return false;
    }
    // Shows a line in the conversation without starting a turn.
    virtual void noteInConversation(const QString& sessionId, const QJsonObject& note) {
        (void)sessionId;
        (void)note;
    }
    // ADR 0114 §10: the free space of the volume that holds `volumePath` and the size on disk
    // of each worktree, answered on the caller's thread. Without a way to measure, nothing is
    // known.
    virtual void askStorage(const QString& volumePath, const QStringList& worktrees,
                            StorageDone done) {
        (void)volumePath;
        (void)worktrees;
        done({});
    }
    // Removes the worktree the way `git worktree remove` without force and then `git worktree
    // prune` do, from the main checkout `repository`, after refusing a worktree with
    // uncommitted or untracked changes, with commits on no remote branch, or that is the main
    // checkout. Answers the error in words, empty when it was removed.
    virtual void askRemoveWorktree(const QString& repository, const QString& worktree,
                                   RemovalDone done) {
        (void)repository;
        (void)worktree;
        done(QStringLiteral("Worktrees cannot be removed here."));
    }
    // The title of a conversation that runs a turn in `path` or a folder under it; empty when
    // none does.
    [[nodiscard]] virtual QString conversationRunningIn(const QString& path) const {
        (void)path;
        return {};
    }
    // The conversations whose folder is `path` or a folder under it, idle or not.
    [[nodiscard]] virtual QStringList conversationsIn(const QString& path) const {
        (void)path;
        return {};
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
    // T-28: what a command sets off, pointing members at worktrees and sending their messages,
    // is stored in the team's outbox with the change and sent after the command is answered,
    // one entry per event-loop pass. Off, it is sent at once, as the tests of the rules expect.
    void setDeferSending(bool defer) noexcept { deferSending_ = defer; }

    [[nodiscard]] QList<const Team*> teams(const QString& repository = {}) const;
    [[nodiscard]] const Team* team(qint64 teamId) const;
    [[nodiscard]] const Team* teamOf(const QString& sessionId) const;
    [[nodiscard]] bool isMember(const QString& sessionId) const { return teamOf(sessionId); }
    // The repository of a worktree the team made for one of its tasks; nothing for another
    // path. Such a worktree belongs to its team's repository, so git is not asked (T-32).
    [[nodiscard]] QString taskWorktreeRepository(const QString& worktree) const;
    // The team as clients receive it: the document plus each member's number, title and agent.
    [[nodiscard]] QJsonObject teamJson(const Team& team) const;
    // ADR 0115: the team for team.list and team.changed: teamJson without its tasks and
    // decisions, marked "directory". Its size follows the members, so the sidebar always
    // receives every team; a page reads the rest with team.get.
    [[nodiscard]] QJsonObject directoryJson(const Team& team) const;
    [[nodiscard]] QJsonArray updatesJson(qint64 teamId, int limit = defaultPage,
                                         qint64 beforeId = 0) const;
    // T-36: the whole text of a task's history entry whose text in the document was cut;
    // nothing when it was not cut or the journal has no copy.
    [[nodiscard]] std::optional<QString> wholeHistoryText(qint64 teamId, const QString& taskId,
                                                          qsizetype entry) const;
    // The entries of the task that have a whole text in the journal.
    [[nodiscard]] QSet<qsizetype> wholeHistoryEntries(qint64 teamId, const QString& taskId) const;

    // `draft` carries the settings and the members with their session ids. Nothing is stored
    // when a rule refuses it.
    TeamResult create(Team draft, const QString& actor);
    // name, goal, integration_branch, merge_policy, leader_writes_requirements, paused,
    // shared_paths, max_rounds and turn_budget; other keys are ignored.
    TeamResult update(qint64 teamId, const QJsonObject& changes, const QString& actor);
    std::optional<TeamError> remove(qint64 teamId);
    // ADR 0114, open point 2 (TeamCoordinatorSettings.cpp): what a new team starts from,
    // kept in the journal. Only `turn_budget` for now; changing it changes no team.
    [[nodiscard]] QJsonObject teamSettings() const;
    [[nodiscard]] int turnBudgetSetting() const;
    std::optional<TeamError> updateTeamSettings(const QJsonObject& changes);
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
    // ADR 0118 (TeamCoordinatorDone.cpp): every task that records a pull request and is not
    // done is done once the remote says it is merged. Asked when a team is checked for merges,
    // at load and on a timer, at most once per `minimumGap` for a team and for at most
    // maxPullRequestsPerCheck pull requests; an error never moves a task.
    static constexpr int defaultPullRequestPeriodMs = 2 * 60 * 1000;
    static constexpr int defaultPullRequestGapMs = 60 * 1000;
    static constexpr int maxPullRequestsPerCheck = 20;
    void checkPullRequests(qint64 teamId);
    // Whether the task records a pull request URL: then only the pull request says it is done.
    [[nodiscard]] static bool recordsPullRequest(const Task& task);
    void setPullRequestPeriod(int periodMs, int minimumGapMs);
    // The leader closes a task by hand: `evidence` is a pull request URL or a commit.
    TeamResult closeTask(qint64 teamId, const QString& taskId, const QString& evidence,
                         const QString& note, const QString& actor);
    // Whether a merge check, a lane check, a review's diff or a prepared command is still
    // with git.
    [[nodiscard]] bool checksOpen() const {
        return !mergeChecks_.isEmpty() || !pullRequestChecks_.isEmpty() || !laneChecks_.isEmpty() ||
               !reviewDiffs_.isEmpty() || !worktreeWaits_.isEmpty() || handoffsAsked_ > 0;
    }

    // ADR 0114 §2: the team's reviewer answered the review it was sent, or its turn failed
    // (`failure` set). `noFindings` is the verdict line of ADR 0067; a reply without that
    // line has no verdict and is not counted (T-19).
    // `messageId` is the review request that ended. A review is bound to its request from
    // the moment it starts, sent or still held, so a request with another id is ignored.
    // T-23: the reviewer a member's "reviewer" names: the only one, else the one that reviews
    // or last reviewed its tasks; empty when that is not one reviewer.
    [[nodiscard]] static QString reviewerFor(const Team& team, const QString& sessionId);
    void reviewFinished(const QString& reviewerSession, const QString& messageId,
                        const QString& text, bool noFindings, const QString& failure = {});
    // T-19 (TeamCoordinatorReviewTools.cpp), the leader's: void the last recorded verdict of
    // a task, which goes back to In review with its round counted once less; or accept a
    // review made outside the board, which makes a task in review, or blocked by its review,
    // Ready.
    TeamResult voidReview(qint64 teamId, const QString& taskId, const QString& reason,
                          const QString& actor);
    TeamResult acceptReview(qint64 teamId, const QString& taskId, const QString& evidence,
                            const QString& reason, const QString& actor);
    // How many times a reply without a verdict is answered by asking the reviewer again.
    static constexpr int verdictRetries = 1;

    // The quiet period after which an idle leader receives the digest (ADR 0114 §7).
    static constexpr int defaultQuietPeriodMs = 20000;
    void setQuietPeriod(int milliseconds) { quietPeriod_ = milliseconds; }

    // ADR 0114 §10 (TeamCoordinatorStorage.cpp), the user's only, from the desktop: the disk
    // under the team's worktrees with the done tasks whose worktree is still on it, and the
    // removal of one of those worktrees. Both answer `done` once the host measured or removed;
    // a refusal answers at once and changes nothing.
    using StorageAnswer = std::function<void(const std::optional<TeamError>&, const QJsonObject&)>;
    using RemovalAnswer = std::function<void(const TeamResult&)>;
    void storage(qint64 teamId, StorageAnswer done);
    void removeWorktree(qint64 teamId, const QString& taskId, const QString& actor,
                        RemovalAnswer done);
    // The done tasks whose worktree is a folder on disk other than the main checkout.
    [[nodiscard]] static QList<const Task*> doneWorktrees(const Team& team);

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
    // A team's own budget, from team.update; a team paused by its budget runs again when the
    // new one leaves turns. The turns used stay counted.
    std::optional<TeamError> changeTurnBudget(Team& team, const QJsonValue& value,
                                              QList<Update>& updates);
    // Leaves the developer's open tasks unassigned, with an entry in their history.
    void unassignTasksOf(Team& team, const QString& sessionId, const QString& actor,
                         const QString& reason, QList<Update>& updates);
    Decision& raiseDecision(Team& team, const QString& kind, const QString& question,
                            const QString& context, const QStringList& choices,
                            const QString& recommended, const QString& taskId,
                            const QString& raisedBy);
    static void addHistory(Task& task, const QString& actor, const QString& kind,
                           const QString& text, const QJsonObject& data = {});
    // T-36: saves the whole texts of the entries cut since the last save, after the document.
    void saveWholeHistoryTexts(Team& team);
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
    // T-28 (TeamCoordinatorOutbox.cpp): the send step of wake(): settles the receiver's moves,
    // points it at its worktree and queues the message.
    void send(qint64 teamId, const TeamMessage& message);
    void scheduleOutbox();
    void sendFromOutbox();
    // The rest of one outbox entry once its conversation stands where it should: posts it
    // when `deliver`, then forgets it.
    void finishOutboxEntry(qint64 teamId, const QJsonObject& entry, bool deliver);
    // Whether an outbox entry is still for a member that has not received it.
    [[nodiscard]] bool stillDelivers(qint64 teamId, const TeamMessage& message) const;
    void forgetRelocations(qint64 teamId, const QString& sessionId);
    void notePointError(qint64 teamId, const TeamMessage& message, const QString& error);
    // ADR 0114 §4 (TeamCoordinatorLanes.cpp): after a developer's turn, compares the files its
    // open tasks changed with its lane and the shared paths.
    void checkLanes(qint64 teamId, const QString& developer);
    void applyLaneFacts(qint64 teamId, const QString& developer, const QList<Task>& asked,
                        const QHash<QString, TaskFacts>& facts);
    void applyMergeFacts(qint64 teamId, const QList<Task>& asked,
                         const QHash<QString, TaskFacts>& facts);
    void applyPullRequestFacts(qint64 teamId, const QHash<QString, PullRequestFact>& facts);
    // Moves a task that is not done to done (ADR 0118): withdraws its merge decisions and its
    // place with the reviewer, and records `kind` with `data` in its history.
    void finishTask(Team& team, Task& task, const QString& actor, const QString& reason,
                    const QString& kind, const QJsonObject& data, QList<Update>& updates);
    // The goal_reached update, when every task is done.
    static void noteGoalReached(const Team& team, QList<Update>& updates);
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
    // Starts the review the diff was read for, if that task is still the next one of that
    // reviewer as it was.
    void startReview(qint64 teamId, const QString& reviewerSession, const Task& asked,
                     const QString& repository, const QString& integration,
                     const std::optional<TaskDiff>& diff);
    // T-23 (TeamCoordinatorReviewers.cpp): a task entering In review goes to one reviewer's
    // queue, or waits while the team has none; it leaves every queue and review in flight; a
    // reviewer that joins takes what waited; one that leaves hands its reviews over, the one
    // in flight first and not counted.
    static void queueReview(Team& team, const Task& task);
    static void dropReview(Team& team, const QString& taskId);
    static void reviewerJoined(Team& team, const QString& sessionId);
    static void reviewerLeft(Team& team, const QString& sessionId);
    // The questions of a task the review blocked.
    [[nodiscard]] static QString noVerdictQuestion(int round, int replies);
    [[nodiscard]] static QString roundLimitQuestion(int round, int maxRounds);
    [[nodiscard]] static bool blockedByReview(const Task& task);
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
    // ADR 0114 §7 (TeamCoordinatorRelocations.cpp): moves a member out of the worktrees it was
    // handed out of and forgets each move that is settled: done, or no longer needed. Git runs
    // off this thread; `done`, if given, gets why one is still waiting, or nothing.
    void settleRelocations(qint64 teamId, const QString& sessionId, PointDone done);
    void settleStep(qint64 teamId, const QString& sessionId, PointDone done);
    // Where a conversation handed out of `move.from` goes; nothing when it needs no switch.
    [[nodiscard]] std::optional<QString> relocationHome(const Team& team,
                                                        const Relocation& move) const;
    // Moves stored before a restart are settled when the teams are loaded.
    void settleStoredRelocations();
    // One worktree switch of a conversation at a time: `job` runs now, or after the jobs
    // before it, and ends with conversationFree().
    void whenConversationFree(const QString& sessionId, std::function<void()> job);
    void conversationFree(const QString& sessionId);
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
    // Reviewers whose next review waits for git to read its diff, as "<team>/<session>".
    QSet<QString> reviewDiffs_;
    QSet<qint64> mergeChecks_;
    QSet<qint64> mergeChecksAgain_;
    QSet<qint64> pullRequestChecks_;
    QHash<qint64, qint64> pullRequestCheckedAt_;
    // Per team, the pull request the next check starts at: the one after the last asked, so
    // one that never answers cannot keep the others from being asked.
    QHash<qint64, QString> pullRequestCursor_;
    QTimer* pullRequestTimer_{nullptr};
    int pullRequestPeriod_{defaultPullRequestPeriodMs};
    int pullRequestGap_{defaultPullRequestGapMs};
    QSet<QString> laneChecks_;
    QSet<QString> laneChecksAgain_;
    // Prepared commands, by "<team>/<task>": those waiting for the task's worktree, and the
    // answers that are there while the commands they were asked for run.
    QHash<QString, QList<std::function<void()>>> worktreeWaits_;
    // ADR 0114 §10: the worktrees the host is removing now, so one is removed once.
    QSet<QString> worktreeRemovals_;
    // ADR 0114 §10: the size of each done worktree, measured once when it is first listed, so
    // reading the free space again walks no folder.
    QHash<QString, qint64> worktreeSizes_;
    QHash<QString, WorktreeAnswer> worktrees_;
    QHash<QString, HandoffFacts> handoffs_;
    int handoffsAsked_{0};
    QHash<QString, qint64> teamBySession_;
    bool loaded_{false};
    bool deferSending_{false};
    bool outboxScheduled_{false};
    // An outbox entry waits for git: the next one waits for it, so the order is kept.
    bool outboxSending_{false};
    // Conversations with a worktree switch running, and the switches waiting behind it.
    QHash<QString, QList<std::function<void()>>> conversationJobs_;
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
