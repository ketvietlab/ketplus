#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ketplus::daemon {

class LocalJournal;

namespace agent_schedule {

// D-2 limits, recorded in ADR 0126. The agent tool is the only caller.
inline constexpr int minimumLeadMinutes = 1;
inline constexpr int maximumLeadDays = 7;
inline constexpr int maximumActivePerConversation = 5;

inline constexpr auto tooSoonCode = "too_soon";
inline constexpr auto tooFarCode = "too_far";
inline constexpr auto scheduleLimitCode = "schedule_limit";
inline constexpr auto notOwnerCode = "not_owner";
inline constexpr auto foreignTargetCode = "foreign_target";
inline constexpr auto onceOnlyCode = "once_only";

// `sessionId` is the calling conversation. The schedule targets that conversation,
// created_by is the session id, and the rule is once. `now` is UTC.
[[nodiscard]] QJsonObject createAgentSchedule(LocalJournal& journal, const QString& sessionId,
                                              const QJsonObject& payload, const QDateTime& now);
[[nodiscard]] QJsonObject listAgentSchedules(LocalJournal& journal, const QString& sessionId);
[[nodiscard]] QJsonObject cancelAgentSchedule(LocalJournal& journal, const QString& sessionId,
                                              const QJsonObject& payload);

// Disables enabled schedules this conversation created. Status stays empty, so the user
// can turn one back on after the conversation is unarchived. Returns the ids that changed.
[[nodiscard]] QStringList disableAgentSchedules(LocalJournal& journal, const QString& sessionId);

} // namespace agent_schedule
} // namespace ketplus::daemon
