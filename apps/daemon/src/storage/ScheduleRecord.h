#pragma once

#include <QList>
#include <QString>

namespace ketplus::daemon {

// Where a schedule delivers its prompt. The kinds are exclusive: a conversation names a
// session, a team or team_start names a team, and new names the workspace a run will open.
struct ScheduleTarget final {
    // "conversation", "new", "team" or "team_start".
    QString kind;
    QString sessionId;
    // Journal team id, decimal text, for team and team_start.
    QString teamId;
    QString workspacePath;
    QString repositoryPath;
    QString provider;
    QString model;
    QString effort;
    bool isolatedWorktree{false};
    QString titleTemplate;
};

// One schedule. `ruleJson` is opaque JSON owned by the recurrence engine. `status` is empty,
// or "target_deleted" after the conversation a conversation target names is deleted.
struct ScheduleRecord final {
    QString id;
    QString name;
    bool enabled{true};
    QString status;
    QString prompt;
    ScheduleTarget target;
    QString ruleJson;
    // "queue" or "skip".
    QString busyPolicy;
    // "run_once" or "skip".
    QString missedPolicy;
    QString createdBy;
    QString createdAt;
    QString updatedAt;
    QString nextRunAt;
    QString lastRunAt;
};

struct ScheduleFilter final {
    QString targetKind;
    QString sessionId;
    QString teamId;
    // When set, only schedules this conversation or "user" created.
    QString createdBy;
};

// One firing of a schedule. `status` is queued, running, succeeded, failed, skipped or missed.
struct ScheduleRunRecord final {
    QString id;
    QString scheduleId;
    QString scheduledFor;
    QString startedAt;
    QString finishedAt;
    QString status;
    QString sessionId;
    QString turnReference;
    QString errorCode;
    QString errorMessage;
};

// Newest first. `nextBefore` is the id to pass back for the next older page, empty on the last.
struct ScheduleRunPage final {
    QList<ScheduleRunRecord> runs;
    QString nextBefore;
};

} // namespace ketplus::daemon
