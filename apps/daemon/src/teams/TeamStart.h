#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace ketplus::daemon::teams {

// team.create start_at, and team.startAt.update. Wire names are snake_case.
// `at` is a local civil time "YYYY-MM-DDTHH:mm". `timezone` is an IANA id.
struct TeamStartRequest final {
    QString at;
    QString timeZone;
};

struct TeamStartError final {
    QString code;
    QString message;
};

struct TeamStartPlan final {
    QString ruleJson;
    // UTC ISO-8601 with a zone, the once rule's only fire.
    QString nextRunAt;
};

// An empty object is not a start time. A present object must be {at, timezone}.
[[nodiscard]] std::optional<TeamStartError> parseTeamStart(const QJsonObject& startAt,
                                                           TeamStartRequest* request);

// The fire must be at least one minute and at most 30 days after `now`.
[[nodiscard]] std::optional<TeamStartError> planTeamStart(const TeamStartRequest& request,
                                                          const QDateTime& now, TeamStartPlan* plan);

} // namespace ketplus::daemon::teams
