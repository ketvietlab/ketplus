#include "teams/TeamCoordinator.h"

#include "storage/LocalJournal.h"
#include "teams/TeamStart.h"

#include <QUuid>

namespace ketplus::daemon::teams {
namespace {

TeamError journalError(const QString& message) {
    return {QStringLiteral("journal_error"),
            message.isEmpty() ? QStringLiteral("The journal refused the change.") : message};
}

TeamError notFound() {
    return {QStringLiteral("team_not_found"), QStringLiteral("The team no longer exists.")};
}

TeamError coded(const QString& code, const QString& message) { return {code, message}; }

} // namespace

QString storeTeamStart(LocalJournal& journal, const Team& team, const TeamStartPlan& plan,
                       QString* error) {
    ScheduleRecord record;
    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.name = team.name.left(200);
    if (record.name.isEmpty())
        record.name = QStringLiteral("Team start");
    record.enabled = true;
    record.target.kind = QStringLiteral("team_start");
    record.target.teamId = QString::number(team.id);
    record.ruleJson = plan.ruleJson;
    record.busyPolicy = QStringLiteral("queue");
    record.missedPolicy = QStringLiteral("run_once");
    record.createdBy = QStringLiteral("user");
    record.nextRunAt = plan.nextRunAt;
    if (!journal.createSchedule(record, error))
        return {};
    return record.id;
}

namespace {

void dropSchedule(LocalJournal& journal, const QString& id) {
    if (id.isEmpty())
        return;
    QString error;
    (void)journal.deleteSchedule(id, &error);
}

} // namespace

TeamResult TeamCoordinator::startNow(const qint64 teamId, const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    if (team->leaderStarted)
        return {.error = coded(QStringLiteral("team_already_started"),
                               QStringLiteral("The leader has already started."))};
    const QString scheduleId = team->startScheduleId;
    const Team before = *team;
    team->startWaiting = false;
    team->startAt.clear();
    team->startTimeZone.clear();
    team->startScheduleId.clear();
    if (const auto failed = commit(*team, before, actor, {}))
        return {.error = failed};
    dropSchedule(*journal_, scheduleId);
    if (host_)
        host_->schedulesChanged();
    startLeader(teamId);
    return {.teamId = teamId};
}

TeamResult TeamCoordinator::updateStartAt(const qint64 teamId, const QJsonObject& startAt,
                                          const QString& actor, const QDateTime& now) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    if (team->leaderStarted)
        return {.error = coded(QStringLiteral("team_already_started"),
                               QStringLiteral("The leader has already started."))};
    if (!team->startWaiting)
        return {.error = coded(QStringLiteral("start_not_scheduled"),
                               QStringLiteral("This team is not waiting for a start time."))};
    TeamStartRequest request;
    if (const auto invalid = parseTeamStart(startAt, &request))
        return {.error = coded(invalid->code, invalid->message)};
    TeamStartPlan plan;
    const QDateTime clock = now.isValid() ? now : QDateTime::currentDateTimeUtc();
    if (const auto invalid = planTeamStart(request, clock, &plan))
        return {.error = coded(invalid->code, invalid->message)};
    const QString previous = team->startScheduleId;
    QString error;
    const QString scheduleId = storeTeamStart(*journal_, *team, plan, &error);
    if (scheduleId.isEmpty())
        return {.error = journalError(error)};
    const Team before = *team;
    team->startAt = request.at;
    team->startTimeZone = request.timeZone;
    team->startScheduleId = scheduleId;
    if (const auto failed = commit(*team, before, actor, {})) {
        dropSchedule(*journal_, scheduleId);
        return {.error = failed};
    }
    dropSchedule(*journal_, previous);
    if (host_)
        host_->schedulesChanged();
    return {.teamId = teamId};
}

TeamResult TeamCoordinator::cancelStartAt(const qint64 teamId, const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    if (team->leaderStarted)
        return {.error = coded(QStringLiteral("team_already_started"),
                               QStringLiteral("The leader has already started."))};
    if (!team->startWaiting)
        return {.error = coded(QStringLiteral("start_not_scheduled"),
                               QStringLiteral("This team is not waiting for a start time."))};
    const QString scheduleId = team->startScheduleId;
    const Team before = *team;
    team->startAt.clear();
    team->startTimeZone.clear();
    team->startScheduleId.clear();
    if (const auto failed = commit(*team, before, actor, {}))
        return {.error = failed};
    dropSchedule(*journal_, scheduleId);
    if (host_)
        host_->schedulesChanged();
    return {.teamId = teamId};
}

} // namespace ketplus::daemon::teams
