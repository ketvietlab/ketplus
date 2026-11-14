#include "schedule/TeamStartRuntime.h"

#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::schedule {
namespace {
namespace documents = ::ketplus::schedule;

QString teamKey(const QString& teamId) { return QStringLiteral("team:") + teamId; }

} // namespace

TeamStartRuntime::TeamStartRuntime(teams::TeamCoordinator* teams, Changed changed)
    : teams_(teams), changed_(std::move(changed)) {}

Inspection inspectTeamStart(const teams::TeamCoordinator* teams,
                            const documents::Schedule& schedule) {
    Inspection inspection;
    if (schedule.target.kind != documents::TargetKind::TeamStart || teams == nullptr) {
        inspection.state = ConversationState::Missing;
        return inspection;
    }
    bool ok = false;
    const qint64 teamId = schedule.target.teamId.toLongLong(&ok);
    const teams::Team* team = ok ? teams->team(teamId) : nullptr;
    if (team == nullptr) {
        inspection.state = ConversationState::Missing;
        return inspection;
    }
    if (!team->leaderStarted && team->paused) {
        inspection.state = ConversationState::Busy;
        inspection.sessionId = teamKey(QString::number(teamId));
        return inspection;
    }
    return inspection;
}

Delivery deliverTeamStart(teams::TeamCoordinator* teams, const documents::Schedule& schedule) {
    Delivery delivery;
    bool ok = false;
    const qint64 teamId = schedule.target.teamId.toLongLong(&ok);
    const teams::Team* team =
        ok && teams != nullptr && schedule.target.kind == documents::TargetKind::TeamStart
            ? teams->team(teamId)
            : nullptr;
    if (team == nullptr) {
        delivery.errorCode = QString::fromLatin1(targetDeletedCode);
        delivery.errorMessage = QStringLiteral("The schedule target no longer exists.");
        return delivery;
    }
    const teams::Member* leader = team->memberWithRole(teams::role::leader);
    const QString sessionId = leader == nullptr ? QString{} : leader->sessionId;
    const bool hasGoal = !team->goal.isEmpty();
    if (!teams->startLeader(teamId)) {
        delivery.errorCode =
            hasGoal ? QStringLiteral("team_not_started") : QStringLiteral("goal_required");
        delivery.errorMessage =
            hasGoal ? QStringLiteral("The leader did not start.")
                    : QStringLiteral("The team needs a goal before the leader can start.");
        return delivery;
    }
    delivery.finished = true;
    delivery.sessionId = sessionId;
    return delivery;
}

Inspection TeamStartRuntime::inspect(const documents::Schedule& schedule) const {
    return inspectTeamStart(teams_, schedule);
}

Delivery TeamStartRuntime::deliver(const documents::Schedule& schedule, const documents::Run& run) {
    (void)run;
    return deliverTeamStart(teams_, schedule);
}

void TeamStartRuntime::emitRun(const documents::Run& run) { (void)run; }

void TeamStartRuntime::emitChanged(const QString& change, const documents::Schedule& schedule) {
    if (changed_)
        changed_(change, schedule);
}

} // namespace ketplus::daemon::schedule
