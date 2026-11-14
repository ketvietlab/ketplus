#include "schedule/TeamStartRuntime.h"

#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::schedule {
namespace {
namespace documents = ::ketplus::schedule;

QString teamKey(const QString& teamId) { return QStringLiteral("team:") + teamId; }

} // namespace

TeamStartRuntime::TeamStartRuntime(teams::TeamCoordinator* teams) : teams_(teams) {}

ScheduleRuntime::Inspection
TeamStartRuntime::inspect(const documents::Schedule& schedule) const {
    if (schedule.target.kind != documents::TargetKind::TeamStart || !teams_)
        return {ConversationState::Missing, {}};
    const teams::Team* team = teams_->team(schedule.target.teamId.toLongLong());
    if (!team)
        return {ConversationState::Missing, {}};
    if (!team->leaderStarted && team->paused)
        return {ConversationState::Busy, teamKey(schedule.target.teamId)};
    return {ConversationState::Idle, {}};
}

ScheduleRuntime::Delivery TeamStartRuntime::deliver(const documents::Schedule& schedule,
                                                    const documents::Run& run) {
    (void)run;
    Delivery delivery;
    if (schedule.target.kind != documents::TargetKind::TeamStart || !teams_) {
        delivery.errorCode = QString::fromLatin1(targetDeletedCode);
        delivery.errorMessage = QStringLiteral("The schedule target no longer exists.");
        return delivery;
    }
    const qint64 teamId = schedule.target.teamId.toLongLong();
    const teams::Team* team = teams_->team(teamId);
    if (!team) {
        delivery.errorCode = QString::fromLatin1(targetDeletedCode);
        delivery.errorMessage = QStringLiteral("The schedule target no longer exists.");
        return delivery;
    }
    const QString leader = [&] {
        const teams::Member* member = team->memberWithRole(teams::role::leader);
        return member ? member->sessionId : QString{};
    }();
    teams_->startLeader(teamId);
    delivery.finished = true;
    delivery.sessionId = leader;
    return delivery;
}

void TeamStartRuntime::emitRun(const documents::Run& run) { (void)run; }

void TeamStartRuntime::emitChanged(const QString& change, const documents::Schedule& schedule) {
    (void)change;
    (void)schedule;
}

} // namespace ketplus::daemon::schedule
