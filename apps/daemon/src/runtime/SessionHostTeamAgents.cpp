// ADR 0114: the team_* tools of the ketplus_agents server. The daemon checks the caller's
// role for every call; a role that may not act is refused with the role that may.
#include "runtime/SessionHost.h"

#include "agents/AgentInstructions.h"

namespace ketplus::daemon {
namespace {

QJsonObject refusal(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

QJsonObject onlyFor(const QString& role, const QString& tool) {
    return refusal(QStringLiteral("role_forbidden"),
                   QStringLiteral("%1 is for the team's %2.").arg(tool, role));
}

QStringList strings(const QJsonValue& value) {
    QStringList result;
    for (const auto& item : value.toArray())
        result.append(item.toString());
    return result;
}

} // namespace

QJsonObject SessionHost::teamMembership(const QString& sessionId) const {
    const teams::Team* team = teams_ ? teams_->teamOf(sessionId) : nullptr;
    if (!team)
        return {};
    const teams::Member* member = team->member(sessionId);
    QJsonObject json{{QStringLiteral("team_id"), QString::number(team->id)},
                     {QStringLiteral("name"), team->name},
                     {QStringLiteral("role"), member->role}};
    if (member->role == teams::role::developer) {
        json.insert(QStringLiteral("lane"), member->lane);
        json.insert(QStringLiteral("paths"), QJsonArray::fromStringList(member->paths));
    }
    for (const QString& role : {teams::role::leader, teams::role::reviewer})
        if (const QString number = teamRoleNumber(sessionId, role); !number.isEmpty())
            json.insert(role + QStringLiteral("_number"), number);
    return json;
}

QString SessionHost::teamRoleNumber(const QString& sessionId, const QString& role) const {
    const teams::Team* team = teams_ ? teams_->teamOf(sessionId) : nullptr;
    const teams::Member* member = team ? team->memberWithRole(role) : nullptr;
    if (!member)
        return {};
    const auto it = sessions_.constFind(member->sessionId);
    return it == sessions_.cend()
               ? QString{}
               : journal_->conversationNumber(it->stored.id, it->stored.generation);
}

QString SessionHost::teamInstructionsFor(const QString& sessionId) const {
    const teams::Team* team = teams_ ? teams_->teamOf(sessionId) : nullptr;
    if (!team)
        return {};
    const teams::Member* member = team->member(sessionId);
    return agent_messaging::teamInstructions(
        {.team = team->name,
         .goal = team->goal,
         .role = member->role,
         .lane = member->lane,
         .paths = member->paths,
         .sharedPaths = team->sharedPaths,
         .mergePolicy = team->mergePolicy,
         .integrationBranch =
             team->integrationBranch.isEmpty() ? QStringLiteral("HEAD") : team->integrationBranch,
         .leaderWritesRequirements = team->leaderWritesRequirements});
}

QJsonObject SessionHost::teamOperation(const QString& sessionId, const QString& operation,
                                       const QJsonObject& payload) {
    const teams::Team* team = teams_->teamOf(sessionId);
    if (!team)
        return refusal(QStringLiteral("not_on_team"),
                       QStringLiteral("This conversation is on no team."));
    const qint64 teamId = team->id;
    const QString role = team->member(sessionId)->role;
    const QString taskId = payload.value(QStringLiteral("task_id")).toString();
    const auto answer = [&](const teams::TeamResult& result, const QString& id) {
        if (!result.ok())
            return refusal(result.error->code, result.error->message);
        const teams::Team* current = teams_->team(teamId);
        const teams::Task* task = current && !id.isEmpty() ? current->task(id) : nullptr;
        return task ? QJsonObject{{QStringLiteral("ok"), true},
                                  {QStringLiteral("task"), teams::toJson(*task, false)}}
                    : QJsonObject{{QStringLiteral("ok"), true}};
    };
    if (operation == QStringLiteral("team_get")) {
        QJsonObject json = teams_->teamJson(*team);
        // Members read the team, not its bookkeeping.
        json.remove(QStringLiteral("tasks"));
        json.remove(QStringLiteral("decisions"));
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("team"), json},
                {QStringLiteral("you"), teamMembership(sessionId)}};
    }
    if (operation == QStringLiteral("team_tasks")) {
        if (!taskId.isEmpty()) {
            const teams::Task* task = team->task(taskId);
            if (!task)
                return refusal(QStringLiteral("task_not_found"),
                               QStringLiteral("There is no task %1.").arg(taskId));
            return {{QStringLiteral("ok"), true}, {QStringLiteral("task"), teams::toJson(*task)}};
        }
        QJsonArray tasks;
        for (const teams::Task& task : team->tasks)
            tasks.append(teams::toJson(task, false));
        return {{QStringLiteral("ok"), true}, {QStringLiteral("tasks"), tasks}};
    }
    if (operation == QStringLiteral("team_wait")) {
        if (role == teams::role::reviewer)
            return onlyFor(QStringLiteral("the leader and developers"), operation);
        const int minutes = payload.value(QStringLiteral("minutes")).toInt();
        const auto result = teams_->wait(
            teamId, sessionId, minutes, payload.value(QStringLiteral("reason")).toString(), taskId);
        if (!result.ok())
            return refusal(result.error->code, result.error->message);
        return {{QStringLiteral("ok"), true}, {QStringLiteral("wakes_in_minutes"), minutes}};
    }
    if (operation == QStringLiteral("team_task_update")) {
        if (role != teams::role::developer)
            return onlyFor(QStringLiteral("developers, on their own task"), operation);
        const teams::Task* task = team->task(taskId);
        if (!task)
            return refusal(QStringLiteral("task_not_found"),
                           QStringLiteral("There is no task %1.").arg(taskId));
        if (task->owner != sessionId)
            return refusal(QStringLiteral("not_owner"),
                           QStringLiteral("%1 is not your task.").arg(taskId));
        QJsonObject changes;
        for (const QString& key :
             {QStringLiteral("note"), QStringLiteral("pull_request"), QStringLiteral("met")})
            if (payload.contains(key))
                changes.insert(key, payload.value(key));
        if (!changes.isEmpty())
            if (const auto result = teams_->updateTask(teamId, taskId, changes, sessionId);
                !result.ok())
                return answer(result, taskId);
        const QString state = payload.value(QStringLiteral("state")).toString();
        if (!state.isEmpty() && state != team->task(taskId)->state)
            return answer(teams_->moveTask(teamId, taskId, state, sessionId,
                                           payload.value(QStringLiteral("question")).toString()),
                          taskId);
        return answer({.teamId = teamId}, taskId);
    }
    if (role != teams::role::leader)
        return onlyFor(QStringLiteral("leader"), operation);
    if (operation == QStringLiteral("team_task_create")) {
        QString created;
        const auto result = teams_->createTask(
            teamId,
            {.title = payload.value(QStringLiteral("title")).toString(),
             .description = payload.value(QStringLiteral("description")).toString(),
             .lane = payload.value(QStringLiteral("lane")).toString(),
             .dependsOn = strings(payload.value(QStringLiteral("depends_on"))),
             .criteria = strings(payload.value(QStringLiteral("criteria")))},
            sessionId, &created);
        return answer(result, created);
    }
    const auto memberByNumber = [&](const QString& number) -> QString {
        const QString wanted = agent_messaging::bareNumber(number);
        for (const teams::Member& member : team->members) {
            const auto it = sessions_.constFind(member.sessionId);
            if (it != sessions_.cend() &&
                journal_->conversationNumber(it->stored.id, it->stored.generation) == wanted)
                return member.sessionId;
        }
        return {};
    };
    if (operation == QStringLiteral("team_task_assign"))
        return answer(
            teams_->assignTask(teamId, taskId,
                               memberByNumber(payload.value(QStringLiteral("owner")).toString()),
                               sessionId),
            taskId);
    if (operation == QStringLiteral("team_task_handover"))
        return answer(
            teams_->handoverTask(teamId, taskId,
                                 memberByNumber(payload.value(QStringLiteral("owner")).toString()),
                                 payload.value(QStringLiteral("reason")).toString(), sessionId),
            taskId);
    if (operation == QStringLiteral("team_task_criteria"))
        return answer(teams_->setCriteria(teamId, taskId,
                                          strings(payload.value(QStringLiteral("criteria"))),
                                          sessionId),
                      taskId);
    if (operation == QStringLiteral("team_lane_set")) {
        const QString developer =
            memberByNumber(payload.value(QStringLiteral("developer")).toString());
        if (payload.value(QStringLiteral("reason")).toString().trimmed().isEmpty())
            return refusal(QStringLiteral("invalid_payload"),
                           QStringLiteral("Give the reason for the lane change."));
        return answer(teams_->updateMember(
                          teamId, developer, payload.value(QStringLiteral("lane")).toString(),
                          strings(payload.value(QStringLiteral("paths"))), sessionId,
                          payload.value(QStringLiteral("reason")).toString()),
                      {});
    }
    return teamDecisionOperation(sessionId, operation, payload);
}

// ADR 0114 §3: the leader's decisions. The caller's role was checked by teamOperation.
QJsonObject SessionHost::teamDecisionOperation(const QString& sessionId, const QString& operation,
                                               const QJsonObject& payload) {
    const teams::Team* team = teams_->teamOf(sessionId);
    if (!team)
        return refusal(QStringLiteral("not_on_team"),
                       QStringLiteral("This conversation is on no team."));
    const qint64 teamId = team->id;
    const QString decisionId = payload.value(QStringLiteral("decision_id")).toString();
    const auto decisionJson = [&](const QString& id) -> QJsonObject {
        const teams::Team* current = teams_->team(teamId);
        const teams::Decision* decision = current ? current->decision(id) : nullptr;
        return decision ? QJsonObject{{QStringLiteral("ok"), true},
                                      {QStringLiteral("decision"), teams::toJson(*decision)}}
                        : QJsonObject{{QStringLiteral("ok"), true}};
    };
    if (operation == QStringLiteral("team_decision_raise")) {
        QString raised;
        const auto result = teams_->raiseQuestion(teamId, payload, sessionId, &raised);
        return result.ok() ? decisionJson(raised)
                           : refusal(result.error->code, result.error->message);
    }
    if (operation == QStringLiteral("team_decision_withdraw")) {
        const auto result = teams_->withdrawDecision(
            teamId, decisionId, payload.value(QStringLiteral("reason")).toString(), sessionId);
        return result.ok() ? decisionJson(decisionId)
                           : refusal(result.error->code, result.error->message);
    }
    return refusal(QStringLiteral("unsupported_operation"),
                   QStringLiteral("Unknown team tool %1.").arg(operation));
}

} // namespace ketplus::daemon
