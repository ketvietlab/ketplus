#pragma once

#include "Schedule.h"

#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace ketplus::daemon::protocol {

// The recurrence engine occupies ketplus::daemon::schedule, so an unqualified
// schedule:: here would miss the documents. Pin them.
namespace schedule = ::ketplus::schedule;

// ADR 0126. Request and event names of the `schedules/1` feature. Validation
// rejects unknown fields, an empty prompt, a missing target and unknown enum
// values with the stable codes in ketplus::schedule. Storage and SessionHost
// wiring are a separate change; they call these parsers and then scheduleFromJson
// on a merged document.

inline constexpr auto schedulesFeature = "schedules/1";

inline constexpr auto scheduleListType = "schedule.list";
inline constexpr auto scheduleGetType = "schedule.get";
inline constexpr auto scheduleCreateType = "schedule.create";
inline constexpr auto scheduleUpdateType = "schedule.update";
inline constexpr auto scheduleDeleteType = "schedule.delete";
inline constexpr auto scheduleSetEnabledType = "schedule.setEnabled";
inline constexpr auto scheduleRunNowType = "schedule.runNow";
inline constexpr auto scheduleRunsType = "schedule.runs";
inline constexpr auto scheduleChangedEvent = "schedule.changed";
inline constexpr auto scheduleRunEvent = "schedule.run";

inline constexpr auto scheduleCreatedChange = "created";
inline constexpr auto scheduleUpdatedChange = "updated";
inline constexpr auto scheduleDeletedChange = "deleted";

// Handler codes. The request parser does not return these.
inline constexpr auto scheduleNotFoundCode = "schedule_not_found";
// schedule.runNow answers this until the runtime that fires a schedule exists.
inline constexpr auto scheduleNotImplementedCode = "not_implemented";

struct MessageValidation final {
    bool ok{false};
    QString code;
    QString message;
    // The payload written back with only the accepted fields.
    QJsonObject canonical;
};

struct ScheduleListQuery final {
    std::optional<schedule::TargetKind> targetKind;
    QString sessionId;
    QString teamId;
};

struct ScheduleUpdate final {
    QString id;
    std::optional<QString> name;
    std::optional<bool> enabled;
    std::optional<QString> prompt;
    std::optional<schedule::Target> target;
    std::optional<QString> rule;
    std::optional<schedule::BusyPolicy> busyPolicy;
    std::optional<schedule::MissedPolicy> missedPolicy;
};

struct ScheduleEnabled final {
    QString id;
    bool enabled{false};
};

struct ScheduleRunsQuery final {
    QString id;
    int limit{schedule::defaultRunsPage};
    QString beforeId;
};

struct ScheduleChanged final {
    QString change;
    schedule::Schedule schedule;
};

[[nodiscard]] bool isScheduleRequest(const QString& type);
[[nodiscard]] bool isScheduleEvent(const QString& type);

[[nodiscard]] std::optional<ScheduleListQuery> parseScheduleList(const QJsonObject& payload,
                                                                 schedule::Failure* failure);
// schedule.get, schedule.delete and schedule.runNow.
[[nodiscard]] std::optional<QString> parseScheduleId(const QJsonObject& payload,
                                                     schedule::Failure* failure);
// Server-owned fields (id, timestamps, next/last run) are unknown here.
[[nodiscard]] std::optional<schedule::Schedule> parseScheduleCreate(const QJsonObject& payload,
                                                                    schedule::Failure* failure);
[[nodiscard]] std::optional<ScheduleUpdate> parseScheduleUpdate(const QJsonObject& payload,
                                                                schedule::Failure* failure);
[[nodiscard]] std::optional<ScheduleEnabled> parseScheduleSetEnabled(const QJsonObject& payload,
                                                                     schedule::Failure* failure);
[[nodiscard]] std::optional<ScheduleRunsQuery> parseScheduleRuns(const QJsonObject& payload,
                                                                 schedule::Failure* failure);

[[nodiscard]] MessageValidation validateScheduleRequest(const QString& type,
                                                        const QJsonObject& payload);

[[nodiscard]] QJsonObject scheduleChangedPayload(const QString& change,
                                                 const schedule::Schedule& schedule);
[[nodiscard]] QJsonObject scheduleRunPayload(const schedule::Run& run);
[[nodiscard]] std::optional<ScheduleChanged> parseScheduleChanged(const QJsonObject& payload,
                                                                  schedule::Failure* failure);
[[nodiscard]] std::optional<schedule::Run> parseScheduleRunEvent(const QJsonObject& payload,
                                                                 schedule::Failure* failure);
[[nodiscard]] MessageValidation validateScheduleEvent(const QString& type,
                                                      const QJsonObject& payload);

[[nodiscard]] QJsonObject scheduleListResult(const QList<schedule::Schedule>& schedules);
[[nodiscard]] QJsonObject scheduleDocumentResult(const schedule::Schedule& schedule);
[[nodiscard]] QJsonObject scheduleDeletedResult(const QString& id);
[[nodiscard]] QJsonObject scheduleRunsResult(const QList<schedule::Run>& runs, bool hasMore);
[[nodiscard]] QJsonObject scheduleRunResult(const schedule::Run& run);
[[nodiscard]] MessageValidation validateScheduleResult(const QString& type,
                                                       const QJsonObject& payload);

} // namespace ketplus::daemon::protocol
