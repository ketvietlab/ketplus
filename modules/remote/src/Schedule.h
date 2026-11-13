#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace ketplus::schedule {

// ADR 0126. The daemon owns schedules. Desktop and the daemon share these types;
// both already link ketplus_remote, whose public include path is this directory.
// Wire names are snake_case, matching the rest of the daemon protocol. The journal
// and SessionHost handlers are a separate change and use this header unchanged.

inline constexpr int maximumRetainedRuns = 50;
inline constexpr int defaultRunsPage = maximumRetainedRuns;
inline constexpr int maximumRunsPage = maximumRetainedRuns;

inline constexpr qsizetype maximumScheduleIdLength = 128;
inline constexpr qsizetype maximumScheduleNameLength = 200;
inline constexpr qsizetype maximumSchedulePromptLength = 32 * 1024;
inline constexpr qsizetype maximumScheduleRuleLength = 4096;
inline constexpr qsizetype maximumSchedulePathLength = 4096;
inline constexpr qsizetype maximumScheduleTitleLength = 200;
inline constexpr qsizetype maximumScheduleProviderLength = 64;
inline constexpr qsizetype maximumScheduleModelLength = 128;
inline constexpr qsizetype maximumScheduleEffortLength = 32;

inline constexpr auto unknownFieldCode = "unknown_field";
inline constexpr auto emptyPromptCode = "empty_prompt";
inline constexpr auto missingTargetCode = "missing_target";
inline constexpr auto invalidEnumCode = "invalid_enum";
inline constexpr auto invalidPayloadCode = "invalid_payload";
inline constexpr auto invalidRuleCode = "invalid_rule";
inline constexpr auto promptNotAllowedCode = "prompt_not_allowed";
inline constexpr auto teamStartConstraintCode = "team_start_constraint";

struct Failure final {
    QString code;
    QString message;
    [[nodiscard]] bool ok() const { return code.isEmpty(); }
};

// conversation: an existing session. new: a conversation created per run.
// team: the prompt is a work order for that team's leader. team_start: start the
// team once, with no prompt.
enum class TargetKind { Conversation, NewConversation, Team, TeamStart };
enum class BusyPolicy { Queue, Skip };
enum class MissedPolicy { RunOnce, Skip };
enum class RunStatus { Queued, Running, Succeeded, Failed, Skipped, Missed };

[[nodiscard]] QString nameOf(TargetKind kind);
[[nodiscard]] QString nameOf(BusyPolicy policy);
[[nodiscard]] QString nameOf(MissedPolicy policy);
[[nodiscard]] QString nameOf(RunStatus status);
[[nodiscard]] std::optional<TargetKind> targetKindFromName(const QString& name);
[[nodiscard]] std::optional<BusyPolicy> busyPolicyFromName(const QString& name);
[[nodiscard]] std::optional<MissedPolicy> missedPolicyFromName(const QString& name);
[[nodiscard]] std::optional<RunStatus> runStatusFromName(const QString& name);

struct Target final {
    TargetKind kind{TargetKind::Conversation};
    // conversation
    QString sessionId;
    // new. `workspace` is the repository path. Provider, model and effort may be
    // empty, which means the same defaults as a normal new conversation.
    QString workspace;
    QString provider;
    QString model;
    QString effort;
    bool isolatedWorktree{false};
    QString titleTemplate;
    // team and team_start
    QString teamId;
};

struct Schedule final {
    QString id;
    QString name;
    bool enabled{true};
    QString prompt;
    Target target;
    // Opaque JSON object text. The recurrence engine owns the schema. This layer
    // only checks that the text is a JSON object, and that a team start says
    // `"kind":"once"`.
    QString rule;
    BusyPolicy busyPolicy{BusyPolicy::Queue};
    MissedPolicy missedPolicy{MissedPolicy::RunOnce};
    // The exact string "user", or the id of the agent conversation that created it.
    QString createdBy;
    QString createdAt;
    QString updatedAt;
    QString nextRunAt;
    QString lastRunAt;
};

struct Run final {
    QString id;
    QString scheduleId;
    QString scheduledFor;
    QString startedAt;
    QString finishedAt;
    RunStatus status{RunStatus::Queued};
    QString sessionId;
    // The conversation turn this run started, when it has one.
    QString turnId;
    QString errorCode;
    QString errorMessage;
};

[[nodiscard]] QJsonObject toJson(const Target& target);
[[nodiscard]] QJsonObject toJson(const Schedule& schedule);
[[nodiscard]] QJsonObject toJson(const Run& run);

// Strict document parse. On failure the optional is empty and `failure` is set
// when it is not null. A team start must have an empty prompt, a once rule and
// missed policy run_once. A merged update is checked again with scheduleFromJson.
[[nodiscard]] std::optional<Target> targetFromJson(const QJsonObject& json, Failure* failure);
[[nodiscard]] std::optional<Schedule> scheduleFromJson(const QJsonObject& json, Failure* failure);
[[nodiscard]] std::optional<Run> runFromJson(const QJsonObject& json, Failure* failure);

// `teamStart` requires the object's kind to be "once". Other rules stay opaque
// beyond "this text is a JSON object".
[[nodiscard]] Failure validateRule(const QString& rule, bool teamStart);
// A team start must have a blank prompt. Every other target requires one.
[[nodiscard]] Failure validatePrompt(const QString& prompt, TargetKind kind);
// Empty is `invalid_payload` unless `allowEmpty`. Otherwise 1-128 of [A-Za-z0-9._:-].
[[nodiscard]] Failure validateScheduleId(const QString& id, const QString& field, bool allowEmpty);
// The exact string "user", or a conversation id.
[[nodiscard]] Failure validateCreatedBy(const QString& createdBy);

} // namespace ketplus::schedule
