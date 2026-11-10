#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace ketplus::daemon::teams {

// ADR 0112. Role, state and policy names are the protocol's strings.
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

// The choice of a "Merge T-n?" decision that lets the leader merge that task (ADR 0112 §5).
inline const QString leaderMergesChoice = QStringLiteral("Let the leader merge this one");

inline constexpr int maximumDevelopers = 5;
inline constexpr int maximumCriteria = 20;
inline constexpr int defaultReviewRounds = 2;
// Automatic turns a team may start since the user last acted on it (ADR 0112, open point 2).
inline constexpr int defaultTurnBudget = 40;

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
    // ADR 0112, open point 5: an assignment made before the first plan is approved.
    bool assignmentHeld{false};
    // The user allowed the leader to merge this task under mergePolicy "user".
    bool leaderMayMerge{false};
    // ADR 0112 §5: the task was offered for merging (ready, its dependencies done), once.
    bool mergeOffered{false};
    QList<HistoryEntry> history;
};

struct Decision final {
    QString id;
    // "question" (the leader), "first_plan", "budget", "merge", "empty_role".
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
};

struct Team final {
    qint64 id{0};
    QString repository;
    QString name;
    QString goal;
    QString integrationBranch;
    QString mergePolicy{role::leader};
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
    // Automatic turns held while the team is paused, oldest first (ADR 0112, open point 2):
    // {to, from, kind, task_id, body, expect_reply}.
    QJsonArray held;
    // ADR 0112 §2: tasks waiting for the reviewer, first in first out, and the one it reviews.
    QStringList reviewQueue;
    QString reviewing;
    int nextTask{1};
    int nextDecision{1};
    qint64 revision{0};
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
[[nodiscard]] QJsonObject toJson(const Decision& decision);
[[nodiscard]] QJsonObject toJson(const Member& member);

[[nodiscard]] QString nowIso();

} // namespace ketplus::daemon::teams
