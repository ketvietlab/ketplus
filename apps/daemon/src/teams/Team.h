#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace ketplus::daemon::teams {

// ADR 0114. Role, state and policy names are the protocol's strings.
namespace role {
inline const QString leader = QStringLiteral("leader");
inline const QString developer = QStringLiteral("developer");
inline const QString reviewer = QStringLiteral("reviewer");
} // namespace role

namespace state {
inline const QString todo = QStringLiteral("todo");
inline const QString inProgress = QStringLiteral("in_progress");
inline const QString inReview = QStringLiteral("in_review");
inline const QString ready = QStringLiteral("ready");
inline const QString blocked = QStringLiteral("blocked");
inline const QString done = QStringLiteral("done");
} // namespace state

// The choice of a "Merge T-n?" decision that lets the leader merge that task (ADR 0114 §5).
inline const QString leaderMergesChoice = QStringLiteral("Let the leader merge this one");

// The choices of the "stalled" decision the watchdog raises (ADR 0114 §8).
inline const QString wakeLeaderChoice = QStringLiteral("Wake the leader again");
inline const QString pauseTeamChoice = QStringLiteral("Pause the team");

inline constexpr int maximumDevelopers = 5;
inline constexpr int maximumCriteria = 20;
inline constexpr int defaultReviewRounds = 2;
// Automatic turns a team may start since the user last acted on it (ADR 0114, open point 2):
// the daemon's global value until the user changes it (Settings → Teams), and the range a
// team's own value and the global one keep.
inline constexpr int defaultTurnBudget = 40;
inline constexpr int minimumTurnBudget = 5;
inline constexpr int maximumTurnBudget = 200;

struct Member final {
    QString sessionId;
    QString role;
    QString name;
    // Developers only: the lane's name and its path prefixes relative to the repository root.
    QString lane;
    QStringList paths;
    QString joinedAt;
};

struct Criterion final {
    QString text;
    bool met{false};
};

// One append-only entry of a task's history.
struct HistoryEntry final {
    QString at;
    // A session id, "user" or "daemon".
    QString actor;
    QString kind;
    QString text;
    QJsonObject data;
};

// A team's whole document goes to clients in one daemon frame (1 MiB), so what a task keeps
// grows with its work, never with the size of a diff. A list longer than the bound keeps its
// first entries and records the full count next to it as "<key>_total".
inline constexpr qsizetype maximumOutsidePaths = 200;
inline constexpr qsizetype maximumHistoryText = 2000;
inline constexpr qsizetype maximumHistoryListItems = 50;

// The entry within the bounds above. Entries read from older documents are bounded too, and
// so are the team's Updates (their text and data), when written and when read.
[[nodiscard]] HistoryEntry bounded(HistoryEntry entry);
[[nodiscard]] QString boundedText(QString text);
[[nodiscard]] QJsonObject boundedData(QJsonObject data);

struct Task final {
    QString id;
    QString title;
    QString description;
    QString lane;
    QStringList dependsOn;
    QList<Criterion> criteria;
    QString owner;
    QString state{state::todo};
    // The state a blocked task returns to when its question is answered.
    QString resumeState;
    QString question;
    QString branch;
    QString worktree;
    QString baseCommit;
    QString headCommit;
    QString pullRequest;
    int round{0};
    QStringList outsideLane;
    // ADR 0114, open point 5: an assignment made before the first plan is approved.
    bool assignmentHeld{false};
    // The user allowed the leader to merge this task under mergePolicy "user".
    bool leaderMayMerge{false};
    // ADR 0114 §5: the task was offered for merging (ready, its dependencies done), once.
    bool mergeOffered{false};
    QList<HistoryEntry> history;
    // ADR 0116 §4: the team revision at which this task last changed, the fence of a change
    // a paired phone asks for.
    qint64 changedRevision{0};
};

struct Decision final {
    QString id;
    // "question" (the leader), "first_plan", "budget", "merge", "empty_role", "stalled".
    QString kind{QStringLiteral("question")};
    QString question;
    QString context;
    QStringList choices;
    QString recommended;
    QString taskId;
    QString raisedBy;
    QString raisedAt;
    // "open", "answered" or "withdrawn".
    QString state{QStringLiteral("open")};
    QString answer;
    QString answeredAt;
    QString reason;
    // ADR 0116 §4: the team revision at which this decision last changed.
    qint64 changedRevision{0};
};

// A member that must leave a worktree that was handed to someone else. It is stored with
// the handover and kept until the conversation has left: a conversation cannot always move
// while its turn runs (ADR 0114, point 1).
struct Relocation final {
    QString session;
    QString from;
    bool operator==(const Relocation&) const = default;
};

struct Team final {
    qint64 id{0};
    QString repository;
    QString name;
    QString goal;
    QString integrationBranch;
    QString mergePolicy{role::leader};
    // What the leader and the developers may do without asking: a conversation's execution
    // policy ("read-only", "ask" or "full"), given to every such member the team starts.
    // The reviewer is not in it: a review turn is always read-only (ADR 0067).
    QString executionPolicy{QStringLiteral("ask")};
    bool leaderWritesRequirements{true};
    bool paused{false};
    QString pausedReason;
    QStringList sharedPaths;
    int maxRounds{defaultReviewRounds};
    int turnBudget{defaultTurnBudget};
    int automaticTurns{0};
    bool planApproved{false};
    // The leader has had its first turn with the goal.
    bool leaderStarted{false};
    QList<Member> members;
    QList<Task> tasks;
    QList<Decision> decisions;
    // Automatic turns held while the team is paused, oldest first (ADR 0114, open point 2):
    // {to, from, kind, task_id, body, expect_reply}.
    QJsonArray held;
    // T-28: messages the team decided to send, in the same form, sent after the command that
    // decided them is answered. Stored, so a restart sends what is left, once.
    QJsonArray outbox;
    // ADR 0114 §2: tasks waiting for the reviewer, first in first out, and the one it reviews.
    QStringList reviewQueue;
    QString reviewing;
    // The id of the message that asks for that review, chosen when the review starts and
    // stored with it, before the message is sent. Empty with a review in flight only in a
    // journal written before reviews were bound to their request.
    QString reviewingMessage;
    QList<Relocation> relocations;
    int nextTask{1};
    int nextDecision{1};
    qint64 revision{0};
    // ADR 0116 §4: the team revisions at which its settings (name, goal, branch, merge
    // setting, paused and its reason) and its member list last changed.
    qint64 settingsRevision{0};
    qint64 membersRevision{0};
    QString createdAt;
    QString changedAt;

    [[nodiscard]] const Member* member(const QString& sessionId) const;
    [[nodiscard]] Member* member(const QString& sessionId);
    [[nodiscard]] const Member* memberWithRole(const QString& role) const;
    [[nodiscard]] int count(const QString& role) const;
    [[nodiscard]] const Task* task(const QString& id) const;
    [[nodiscard]] Task* task(const QString& id);
    [[nodiscard]] Decision* decision(const QString& id);
    [[nodiscard]] const Decision* decision(const QString& id) const;
    [[nodiscard]] int openDecisions() const;
    [[nodiscard]] QStringList memberSessions() const;
};

// The document stored in the journal and sent to clients (without the client-only fields
// a SessionHost adds, such as numbers and titles).
[[nodiscard]] QJsonObject toJson(const Team& team);
[[nodiscard]] Team teamFromJson(const QJsonObject& json);
[[nodiscard]] QJsonObject toJson(const Task& task, bool withHistory = true);

// ADR 0115: what clients read is bounded by the size of the team, never by how long it ran.
// The summary lists the first outside paths of a task and the newest answered decisions;
// the rest comes from team.task.get, team.updates and team.decision.list, page by page.
inline constexpr qsizetype summaryOutsidePaths = 20;
inline constexpr qsizetype summaryAnsweredDecisions = 20;
inline constexpr int defaultPage = 50;
inline constexpr int maximumPage = 200;

// The task without history in a summary: at most summaryOutsidePaths outside paths, the
// full count as "outside_lane_total".
[[nodiscard]] QJsonObject summaryJson(const Task& task);
// The task with one page of its history: the newest `limit` entries before index `before`
// (the end when negative), oldest first, with "history_total" and "history_start".
[[nodiscard]] QJsonObject taskPageJson(const Task& task, int limit = defaultPage,
                                       qsizetype before = -1);
[[nodiscard]] QJsonObject toJson(const Decision& decision);
[[nodiscard]] QJsonObject toJson(const Member& member);

[[nodiscard]] QString nowIso();

} // namespace ketplus::daemon::teams
