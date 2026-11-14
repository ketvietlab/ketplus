#pragma once

#include "teams/TeamRules.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace ketplus::daemon::teams {

// team.create `repeat`: a work order on any v1 rule, stored as a team schedule.
// Wire names are snake_case. Only `rule` and `work_order` are required; `busy_policy`
// and `missed_policy` default to queue and run_once.
struct TeamRepeatPlan {
    QString ruleJson;
    QString workOrder;
    QString busyPolicy;
    QString missedPolicy;
    QString nextRunAt;
};

[[nodiscard]] std::optional<TeamError> parseTeamRepeat(const QJsonObject& repeat,
                                                       const QDateTime& now, TeamRepeatPlan* plan);

} // namespace ketplus::daemon::teams
