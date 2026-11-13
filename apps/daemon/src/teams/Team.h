#pragma once

#include "TeamLanguage.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace ketplus::daemon::teams {

// T-56: a fixed text the daemon writes for a team, in the team's language. The source is
// marked QT_TRANSLATE_NOOP("Team", ...), so lupdate finds it (modules/remote/translations).
[[nodiscard]] inline QString say(const QString& language, const char* source) {
    return ::ketplus::remote::team_language::text(language, "Team", source);
}

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
// T-56: the daemon's fixed choices are shown in the team's language (say()); an answer is
// matched by its place among the decision's choices, so either language answers it.
inline constexpr const char* leaderMergesSource =
    QT_TRANSLATE_NOOP("Team", "Let the leader merge this one");
inline const QString leaderMergesChoice = QString::fromLatin1(leaderMergesSource);

// The choices of the "stalled" decision the watchdog raises (ADR 0114 §8).
inline constexpr const char* wakeLeaderSource = QT_TRANSLATE_NOOP("Team", "Wake the leader again");
inline constexpr const char* pauseTeamSource = QT_TRANSLATE_NOOP("Team", "Pause the team");
inline const QString wakeLeaderChoice = QString::fromLatin1(wakeLeaderSource);
inline const QString pauseTeamChoice = QString::fromLatin1(pauseTeamSource);

// T-56: a task state or a role as a person reads it, in the team's language.
// English keeps the stored word ("in_progress"), as before T-56.
[[nodiscard]] inline QString stateName(const QString& language, const QString& value) {
    if (::ketplus::remote::team_language::orEnglish(language) ==
        ::ketplus::remote::team_language::english)
        return value;
    if (value == state::todo)
        return say(language, QT_TRANSLATE_NOOP("Team", "todo"));
    if (value == state::inProgress)
        return say(language, QT_TRANSLATE_NOOP("Team", "in progress"));
    if (value == state::inReview)
        return say(language, QT_TRANSLATE_NOOP("Team", "in review"));
    if (value == state::ready)
        return say(language, QT_TRANSLATE_NOOP("Team", "ready"));
    if (value == state::blocked)
        return say(language, QT_TRANSLATE_NOOP("Team", "blocked"));
    if (value == state::done)
        return say(language, QT_TRANSLATE_NOOP("Team", "done"));
    return value;
}

[[nodiscard]] inline QString roleName(const QString& language, const QString& value) {
    if (value == role::leader)
        return say(language, QT_TRANSLATE_NOOP("Team", "leader"));
    if (value == role::developer)
        return say(language, QT_TRANSLATE_NOOP("Team", "developer"));
    if (value == role::reviewer)
        return say(language, QT_TRANSLATE_NOOP("Team", "reviewer"));
    return value;
}

// T-40: where a team's reviews happen: on the board (the reviewer's reply is the record) or
// on the task's pull request (the findings become a comment there, and the board keeps the
// verdict and the comment's link).
namespace place {
inline const QString board = QStringLiteral("board");
inline const QString pullRequest = QStringLiteral("pull_request");
} // namespace place

inline constexpr int maximumDevelopers = 5;
// T-23: a team has one or two reviewers.
inline constexpr int maximumReviewers = 2;
inline constexpr int maximumCriteria = 20;
// T-40, T-53: the review passes a team gives a task before the leader decides, one to five
// (Max review passes per PR). The default stays at the user's rule of two.
inline constexpr int defaultReviewRounds = 2;
inline constexpr int maximumReviewRounds = 5;
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
    // T-36: the text before the cut, when `text` was cut. Never in the document: commit()
    // saves it to the journal, where team_tasks reads it by entry, and clears it.
    QString wholeText;
};

// A team's whole document goes to clients in one daemon frame (1 MiB), so what a task keeps
// grows with its work, never with the size of a diff. A list longer than the bound keeps its
// first entries and records the full count next to it as "<key>_total".
inline constexpr qsizetype maximumOutsidePaths = 200;
inline constexpr qsizetype maximumHistoryText = 2000;
inline constexpr qsizetype maximumHistoryListItems = 50;
// T-36: the most of one entry's text the journal keeps whole beside the document.
inline constexpr qsizetype maximumWholeHistoryText = 65536;

// The entry within the bounds above. Entries read from older documents are bounded too, and
// so are the team's Updates (their text and data), when written and when read.
[[nodiscard]] HistoryEntry bounded(HistoryEntry entry);
[[nodiscard]] QString boundedText(QString text);
// `text` within `bound` characters; a cut one ends "… (N more characters; <hint>)", or
// without the hint when it is empty.
[[nodiscard]] QString cutText(QString text, qsizetype bound, const QString& hint = {});
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
    // T-90 (ADR 0123): the repository and integration branch the task works in when they are
    // not the team's: named when the task was created, or kept from before the team's
    // repository changed. Empty: the team's. Read them through taskScope().
    QString repository;
    QString integrationBranch;
    // T-93 (ADR 0123 §9): the branch the task's worktree gets, `<type>/<slug>-t<n>`, named or
    // derived when it was created. Empty slug: a task from before, `team/<id>/t-<n>`.
    QString branchType;
    QString branchSlug;
    int round{0};
    // T-23: the reviewer of its last round, which reviews its next round while it is on the
    // team.
    QString reviewer;
    // T-19: replies in a row without a verdict line for the round in review.
    int missingVerdicts{0};
    // T-40: the team's rounds and review place when the task last entered review, so a
    // change of the team's settings applies to tasks that enter review afterwards. The place
    // is the board for a task without a pull request. Zero and empty: never reviewed.
    int roundLimit{0};
    QString reviewPlace;
    // The link of the comment its last review on the pull request left there.
    QString reviewComment;
    QStringList outsideLane;
    // T-38: paths this task may change whatever its owner's lane, beside the team's shared
    // paths (such as a contract or a fixture it must update). Sent only when set.
    QStringList allowedPaths;
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

// T-23: one reviewer's share of the reviews (ADR 0114 §2): the task it reviews, the request
// that review is bound to, and the tasks waiting for it, first in first out. A team keeps
// one per reviewer, in the order the reviewers joined.
struct ReviewSlot final {
    QString reviewer;
    QString reviewing;
    // The id of the message that asks for that review, chosen when the review starts and
    // stored with it, before the message is sent. Empty with a review in flight only in a
    // journal written before reviews were bound to their request.
    QString message;
    QStringList queue;
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
    // T-56: the language the leader works in, "en" or "vi" (team_language). The team's fixed
    // texts, its Updates and the daemon's decisions and messages for it follow it.
    QString language{QStringLiteral("en")};
    bool leaderWritesRequirements{true};
    bool paused{false};
    QString pausedReason;
    QStringList sharedPaths;
    int maxRounds{defaultReviewRounds};
    QString reviewPlace{place::board};
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
    // ADR 0114 §2, T-23: each reviewer's reviews, and the tasks in review that wait while the
    // team has no reviewer, first in first out.
    QList<ReviewSlot> reviews;
    QStringList reviewQueue;
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
    [[nodiscard]] ReviewSlot* reviewSlot(const QString& reviewer);
    [[nodiscard]] const ReviewSlot* reviewSlot(const QString& reviewer) const;
    // The slot that reviews the task now or has it queued, else nullptr.
    [[nodiscard]] const ReviewSlot* reviewSlotOf(const QString& taskId) const;
    // A reviewer reviews the task now.
    [[nodiscard]] bool reviewInFlight(const QString& taskId) const;
    // The task is reviewed now, queued for a reviewer, or waits for one.
    [[nodiscard]] bool reviewPending(const QString& taskId) const;
    // A reviewer reviews something now, or has something queued.
    [[nodiscard]] bool reviewsWaiting() const;
};

// The document stored in the journal and sent to clients (without the client-only fields
// a SessionHost adds, such as numbers and titles).
[[nodiscard]] QJsonObject toJson(const Team& team);
[[nodiscard]] Team teamFromJson(const QJsonObject& json);
[[nodiscard]] QJsonObject toJson(const Task& task, bool withHistory = true);

// T-90 (ADR 0123): the team as the task sees it: its repository and integration branch are
// the task's own when it has them. Git about a task (its worktree, branch, merge, diff and
// cleanup) reads them from here, never from the team directly.
[[nodiscard]] Team taskScope(const Team& team, const Task& task);

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
[[nodiscard]] QJsonObject toJson(const HistoryEntry& entry);
[[nodiscard]] QJsonObject toJson(const Decision& decision);
[[nodiscard]] QJsonObject toJson(const Member& member);

[[nodiscard]] QString nowIso();

} // namespace ketplus::daemon::teams
