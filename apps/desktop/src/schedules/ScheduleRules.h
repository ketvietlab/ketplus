#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace ketplus {

// Desktop editor shape for a schedule rule. ADR 0126 leaves `rule` an opaque JSON
// object string; the recurrence engine (a later task) owns the schema. Until that
// lands, the editor reads and writes only this shape and leaves any other object
// untouched:
//   once      {kind, at: "YYYY-MM-DDTHH:mm", timezone}
//   daily     {kind, time: "HH:mm", timezone}
//   weekdays  {kind, time, timezone}
//   weekly    {kind, days: ["mon".."sun"], time, timezone}
//   interval  {kind, every_hours: 1..24, anchor: "YYYY-MM-DDTHH:mm", timezone}
struct ScheduleRule final {
    QString kind;
    QString at;
    QString time;
    QStringList days;
    int everyHours{0};
    QString anchor;
    QString timezone;
};

// Fields the edit dialog can change. The target stays the stored document's target.
struct ScheduleEdit final {
    QString name;
    QString prompt;
    ScheduleRule rule;
    QString busyPolicy;
    QString missedPolicy;
    // Non-empty when the stored rule is not the shape above. The update sends it back.
    QString preservedRule;
};

struct EditIssue final {
    QString code;
    QString message;
    QString field;

    [[nodiscard]] bool ok() const { return code.isEmpty(); }
};

[[nodiscard]] QStringList scheduleDayKeys();

// Empty when `rule` is not the editor's shape (unknown kind or extra keys).
[[nodiscard]] std::optional<ScheduleRule> scheduleRuleFromText(const QString& rule);
[[nodiscard]] QString scheduleRuleText(const ScheduleRule& rule);
[[nodiscard]] QString scheduleRuleSummary(const QString& rule);

// `createdBy` is "user" or a conversation id. A conversation target then reads
// "#8116" or "#8116 · agent-created", matching the schedules mock.
[[nodiscard]] QString scheduleTargetLabel(const QJsonObject& target,
                                          const QString& createdBy = {});
[[nodiscard]] QString scheduleWhenLabel(const QString& iso, const QString& timezone);
[[nodiscard]] QString scheduleRunLabel(const QJsonObject& run);
[[nodiscard]] QString scheduleLastLabel(const QJsonObject& schedule, const QJsonObject& latestRun);
[[nodiscard]] QString scheduleCreatedByLabel(const QString& createdBy);
[[nodiscard]] QString scheduleBusyLabel(const QString& policy);
[[nodiscard]] QString scheduleMissedLabel(const QString& policy);

// Same codes as ketplus::schedule. `field` is name, prompt, rule, busy_policy or missed_policy.
[[nodiscard]] EditIssue validateScheduleEdit(const QJsonObject& schedule, const ScheduleEdit& edit);
// `target` is included only for a team start, so an empty prompt still validates.
[[nodiscard]] QJsonObject scheduleUpdatePayload(const QJsonObject& schedule,
                                                const ScheduleEdit& edit);

// Daemon `error_message`, with a fallback when `not_implemented` arrives without one.
[[nodiscard]] QString scheduleFailureText(const QJsonObject& result, const QString& error);

} // namespace ketplus
