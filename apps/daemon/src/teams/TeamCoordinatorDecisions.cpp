// ADR 0114 §3 and open points 2 and 5: decisions, the one place where a team waits for the
// user. The leader raises questions; the daemon raises its own. Only the user answers, and the
// answer reaches the leader and the owner of the blocked task once.
#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::teams {
namespace {

constexpr int minimumChoices = 2;
constexpr int maximumChoices = 4;
const QString firstPlan = QStringLiteral("first_plan");
const QString startPlan = QStringLiteral("Start");

TeamError refusal(const QString& code, const QString& message) { return {code, message}; }

QStringList strings(const QJsonValue& value) {
    QStringList result;
    for (const auto& item : value.toArray())
        if (const QString text = item.toString().trimmed(); !text.isEmpty())
            result.append(text.left(200));
    return result;
}

Decision* openDecision(Team& team, const QString& kind) {
    for (Decision& decision : team.decisions)
        if (decision.kind == kind && decision.state == QStringLiteral("open"))
            return &decision;
    return nullptr;
}

} // namespace

TeamResult TeamCoordinator::raiseQuestion(const qint64 teamId, const QJsonObject& draft,
                                          const QString& actor, QString* decisionId) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error =
                    refusal(QStringLiteral("team_not_found"), QStringLiteral("No such team."))};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    if (actorRole(*team, actor) != role::leader)
        return {.error = refusal(QStringLiteral("role_forbidden"),
                                 QStringLiteral("Only the team's leader raises a decision."))};
    const QString question = draft.value(QStringLiteral("question")).toString().trimmed();
    const QStringList choices = strings(draft.value(QStringLiteral("choices")));
    QString recommended = draft.value(QStringLiteral("recommended")).toString().trimmed();
    const QString taskId = draft.value(QStringLiteral("task_id")).toString().trimmed();
    if (question.isEmpty())
        return {.error =
                    refusal(QStringLiteral("invalid_payload"), QStringLiteral("Ask a question."))};
    if (choices.size() < minimumChoices || choices.size() > maximumChoices ||
        QSet<QString>(choices.cbegin(), choices.cend()).size() != choices.size())
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("Give two to four different choices."))};
    if (!recommended.isEmpty() && !choices.contains(recommended))
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("The recommendation is one of the choices."))};
    if (!taskId.isEmpty() && !team->task(taskId))
        return {.error = refusal(QStringLiteral("task_not_found"),
                                 QStringLiteral("There is no task %1.").arg(taskId))};
    const Decision& decision =
        raiseDecision(*team, QStringLiteral("question"), question.left(1000),
                      draft.value(QStringLiteral("context")).toString().trimmed().left(4000),
                      choices, recommended, taskId, actor);
    const QString id = decision.id;
    if (auto failed =
            commit(*team, before, actor,
                   {{.kind = QStringLiteral("decision_raised"),
                     .text = QStringLiteral("%1 waits for the user: %2").arg(id, question),
                     .taskId = taskId,
                     .data = {{QStringLiteral("decision_id"), id}}}}))
        return {.error = failed};
    if (decisionId)
        *decisionId = id;
    return {.teamId = teamId};
}

TeamResult TeamCoordinator::withdrawDecision(const qint64 teamId, const QString& decisionId,
                                             const QString& reason, const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error =
                    refusal(QStringLiteral("team_not_found"), QStringLiteral("No such team."))};
    const Team before = *team;
    Decision* decision = team->decision(decisionId);
    if (!decision)
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("There is no decision %1.").arg(decisionId))};
    const QString role = actorRole(*team, actor);
    // The leader withdraws its own questions; the daemon's own decisions are the user's.
    if (role != QStringLiteral("user") &&
        (role != role::leader || decision->kind != QStringLiteral("question")))
        return {.error =
                    refusal(QStringLiteral("role_forbidden"),
                            QStringLiteral("The leader withdraws only a question it raised."))};
    if (decision->state != QStringLiteral("open"))
        return {.error = refusal(QStringLiteral("decision_closed"),
                                 QStringLiteral("%1 is %2.").arg(decisionId, decision->state))};
    if (reason.trimmed().isEmpty())
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("Give the reason it is no longer needed."))};
    decision->state = QStringLiteral("withdrawn");
    decision->reason = reason.trimmed().left(1000);
    decision->answeredAt = nowIso();
    if (auto failed =
            commit(*team, before, actor,
                   {{.kind = QStringLiteral("decision_withdrawn"),
                     .text = QStringLiteral("%1 withdrawn: %2").arg(decisionId, decision->reason),
                     .taskId = decision->taskId}}))
        return {.error = failed};
    return {.teamId = teamId};
}

TeamResult TeamCoordinator::answerDecision(const qint64 teamId, const QString& decisionId,
                                           const QString& answer, const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error =
                    refusal(QStringLiteral("team_not_found"), QStringLiteral("No such team."))};
    const Team before = *team;
    // Nothing a member sends answers a decision (ADR 0114 §1).
    if (actor != QStringLiteral("user"))
        return {.error = refusal(QStringLiteral("role_forbidden"),
                                 QStringLiteral("Only the user answers a decision."))};
    Decision* decision = team->decision(decisionId);
    if (!decision)
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("There is no decision %1.").arg(decisionId))};
    if (decision->state != QStringLiteral("open"))
        return {.error = refusal(QStringLiteral("decision_closed"),
                                 QStringLiteral("%1 is %2.").arg(decisionId, decision->state))};
    const QString text = answer.trimmed().left(4000);
    if (text.isEmpty())
        return {.error = refusal(QStringLiteral("invalid_payload"),
                                 QStringLiteral("Choose one of the choices or write an answer."))};
    decision->state = QStringLiteral("answered");
    decision->answer = text;
    decision->answeredAt = nowIso();
    // Answering is the user acting on the team: the budget starts again (open point 2).
    team->automaticTurns = 0;
    const QString kind = decision->kind;
    const QString question = decision->question;
    const QString taskId = decision->taskId;
    QList<Update> updates{{.kind = QStringLiteral("decision_answered"),
                           .text = QStringLiteral("%1 answered: %2").arg(decisionId, text),
                           .taskId = taskId,
                           .data = {{QStringLiteral("decision_id"), decisionId}}}};
    QStringList released;
    bool resume = false;
    if (kind == QStringLiteral("budget")) {
        // "Continue (N more turns)" resumes; anything else leaves the team paused by the user.
        resume = text.startsWith(QStringLiteral("Continue"));
        if (!resume)
            team->pausedReason = QStringLiteral("user");
    } else if (kind == QStringLiteral("merge")) {
        // ADR 0114 §5: the user allows exactly this merge.
        if (Task* ready = team->task(taskId); ready && text == leaderMergesChoice)
            ready->leaderMayMerge = true;
    } else if (kind == firstPlan && text == startPlan) {
        team->planApproved = true;
        for (Task& task : team->tasks)
            if (task.assignmentHeld) {
                task.assignmentHeld = false;
                if (!task.owner.isEmpty())
                    released.append(task.id);
            }
    }
    // The blocked task returns to the state it had.
    Task* task = taskId.isEmpty() ? nullptr : team->task(taskId);
    if (task && task->state == state::blocked) {
        const QString back = task->resumeState.isEmpty() ? state::inProgress : task->resumeState;
        task->question.clear();
        applyMove(*team, *task, back, QStringLiteral("user"),
                  QStringLiteral("%1 answered").arg(decisionId), updates);
    }
    const QString owner = task ? task->owner : QString{};
    const Member* leader = team->memberWithRole(role::leader);
    const QString leaderSession = leader ? leader->sessionId : QString{};
    if (auto failed = commit(*team, before, actor, updates))
        return {.error = failed};
    if (resume)
        (void)update(teamId, {{QStringLiteral("paused"), false}}, actor);
    const QString body =
        QStringList{QStringLiteral("Team %1: the user answered %2.")
                        .arg(this->team(teamId)->name, decisionId),
                    QStringLiteral("Question: %1").arg(question),
                    QStringLiteral("Answer: %1").arg(text),
                    taskId.isEmpty() ? QString{} : QStringLiteral("Task %1 can go on.").arg(taskId)}
            .join(QLatin1Char('\n'))
            .trimmed();
    // Once to the leader and once to the blocked task's owner.
    for (const QString& to : QStringList{leaderSession, owner}) {
        const Team* current = this->team(teamId);
        if (to.isEmpty() || !current || !current->member(to) ||
            (to == owner && (owner == leaderSession || kind == QStringLiteral("merge"))))
            continue;
        wake(*current, {.teamId = teamId,
                        .teamName = current->name,
                        .toSession = to,
                        .kind = QStringLiteral("decision"),
                        .taskId = taskId,
                        .body = body});
    }
    for (const QString& id : released) {
        const Team* current = this->team(teamId);
        if (const Task* held = current ? current->task(id) : nullptr)
            deliverAssignment(*current, *held, leaderSession, false);
    }
    return {.teamId = teamId};
}

bool TeamCoordinator::holdForFirstPlan(const qint64 teamId, const QString& taskId,
                                       const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team || team->planApproved || actorRole(*team, actor) != role::leader)
        return false;
    const Team before = *team;
    Task* task = team->task(taskId);
    if (!task)
        return false;
    task->assignmentHeld = true;
    QStringList plan;
    for (const Task& entry : team->tasks)
        if (entry.assignmentHeld) {
            const Member* owner = team->member(entry.owner);
            plan << QStringLiteral("%1 \"%2\" → %3")
                        .arg(entry.id, entry.title, owner ? owner->name : entry.owner);
        }
    const QString question = QStringLiteral("Start these %1 tasks?").arg(plan.size());
    const QString context = plan.join(QLatin1Char('\n'));
    if (Decision* open = openDecision(*team, firstPlan)) {
        open->question = question;
        open->context = context;
    } else {
        raiseDecision(*team, firstPlan, question, context,
                      {startPlan, QStringLiteral("Change the plan first")}, startPlan, {},
                      QStringLiteral("daemon"));
    }
    if (commit(*team, before, QStringLiteral("daemon"),
               {{.kind = QStringLiteral("assignment_held"),
                 .text = QStringLiteral("%1 waits for the first plan to be approved.").arg(taskId),
                 .taskId = taskId}}))
        return false;
    return true;
}

} // namespace ketplus::daemon::teams
