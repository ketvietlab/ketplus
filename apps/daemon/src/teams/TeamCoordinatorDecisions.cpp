// ADR 0114 §3 and open points 2 and 5: decisions, the one place where a team waits for the
// user. The leader raises questions; the daemon raises its own. Only the user answers, and the
// answer reaches the leader and the owner of the blocked task once.
#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::teams {
namespace {

constexpr int minimumChoices = 2;
constexpr int maximumChoices = 4;
const QString firstPlan = QStringLiteral("first_plan");
constexpr const char* startPlan = QT_TRANSLATE_NOOP("Team", "Start");

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
    if (auto failed = commit(
            *team, before, actor,
            {{.kind = QStringLiteral("decision_raised"),
              .text = say(team->language, QT_TRANSLATE_NOOP("Team", "%1 waits for the user: %2"))
                          .arg(id, question),
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
                     .text = say(team->language, QT_TRANSLATE_NOOP("Team", "%1 withdrawn: %2"))
                                 .arg(decisionId, decision->reason),
                     .taskId = decision->taskId}}))
        return {.error = failed};
    return {.teamId = teamId};
}

TeamResult TeamCoordinator::answerDecision(const qint64 teamId, const QString& decisionId,
                                           const QString& answer, const QString& actor) {
    return answerDecisions(teamId, {QJsonObject{{"decision_id", decisionId}, {"answer", answer}}},
                           actor, nullptr, nullptr, false);
}

TeamResult TeamCoordinator::answerDecisions(const qint64 teamId, const QJsonArray& answers,
                                            const QString& actor, QJsonArray* rejected,
                                            QJsonArray* accepted, const bool batch) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = refusal("team_not_found", "No such team.")};
    if (actor != "user")
        return {.error = refusal("role_forbidden", "Only the user answers a decision.")};
    const Team before = *team;
    QList<Update> updates;
    QStringList released;
    QJsonArray applied;
    QJsonArray refusals;
    QHash<QString, QStringList> ownerBodies;
    QHash<QString, QString> ownerTasks;
    QStringList leaderBodies;
    QString leaderTask;
    bool resume = false;
    bool resetStall = false;
    bool wakeStalledLeader = false;
    const Member* leader = team->memberWithRole(role::leader);
    const QString leaderSession = leader ? leader->sessionId : QString{};
    const QString language = team->language;
    for (const auto& value : answers) {
        const auto item = value.toObject();
        const auto decisionId = item.value("decision_id").toString();
        const auto raw = item.value("answer").toString().trimmed();
        const auto text = raw.left(4000);
        Decision* decision = team->decision(decisionId);
        std::optional<TeamError> invalid;
        if (!decision)
            invalid = refusal("invalid_payload",
                              QStringLiteral("There is no decision %1.").arg(decisionId));
        else if (decision->state != "open")
            invalid = refusal("decision_closed",
                              QStringLiteral("%1 is %2.").arg(decisionId, decision->state));
        else if (text.isEmpty())
            invalid = refusal("invalid_payload", "Choose one of the choices or write an answer.");
        else if (batch && raw.size() > 4000)
            invalid = refusal("invalid_payload", "Answers may use at most 4,000 characters.");
        if (invalid) {
            if (!batch)
                return {.error = invalid};
            refusals.append(QJsonObject{{"decision_id", decisionId},
                                        {"error_code", invalid->code},
                                        {"error_message", invalid->message}});
            continue;
        }
        decision->state = QStringLiteral("answered");
        decision->answer = text;
        decision->answeredAt = nowIso();
        const QString kind = decision->kind;
        const QString question = decision->question;
        const QString taskId = decision->taskId;
        // T-56: a fixed choice is known by its place, in whichever language it was raised.
        const QStringList choices = decision->choices;
        const auto chose = [&](const qsizetype index) {
            return index < choices.size() && text == choices.at(index);
        };
        updates.append({.kind = QStringLiteral("decision_answered"),
                        .text = say(team->language, QT_TRANSLATE_NOOP("Team", "%1 answered: %2"))
                                    .arg(decisionId, text),
                        .taskId = taskId,
                        .data = {{QStringLiteral("decision_id"), decisionId}}});
        if (kind == QStringLiteral("budget")) {
            // "Continue (N more turns)", the first choice, resumes; anything else leaves the team
            // paused by the user.
            resume = chose(0);
            if (!resume)
                team->pausedReason = QStringLiteral("user");
        } else if (kind == QStringLiteral("stalled")) {
            // The user moves a stopped team on, or pauses it; the watchdog starts over.
            // Members the user stopped stay stopped, except the leader the user now wakes.
            resetStall = true;
            if (chose(1)) {
                team->paused = true;
                team->pausedReason = QStringLiteral("user");
                resume = false;
            } else {
                wakeStalledLeader = true;
            }
        } else if (kind == QStringLiteral("merge")) {
            // ADR 0114 §5: the user allows exactly this merge.
            if (Task* ready = team->task(taskId); ready && chose(1))
                ready->leaderMayMerge = true;
        } else if (kind == firstPlan && chose(0)) {
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
            const QString back =
                task->resumeState.isEmpty() ? state::inProgress : task->resumeState;
            task->question.clear();
            applyMove(*team, *task, back, QStringLiteral("user"),
                      say(team->language, QT_TRANSLATE_NOOP("Team", "%1 answered")).arg(decisionId),
                      updates);
        }
        const QString body =
            QStringList{
                say(language, QT_TRANSLATE_NOOP("Team", "Team %1: the user answered %2."))
                    .arg(team->name, decisionId),
                say(language, QT_TRANSLATE_NOOP("Team", "Question: %1")).arg(question),
                say(language, QT_TRANSLATE_NOOP("Team", "Answer: %1")).arg(text),
                taskId.isEmpty()
                    ? QString{}
                    : say(language, QT_TRANSLATE_NOOP("Team", "Task %1 can go on.")).arg(taskId)}
                .join(QLatin1Char('\n'))
                .trimmed();
        leaderBodies.append(body);
        leaderTask = taskId;
        if (task && !task->owner.isEmpty() && task->owner != leaderSession && kind != "merge") {
            ownerBodies[task->owner].append(body);
            ownerTasks.insert(task->owner, taskId);
        }
        applied.append(decisionId);
    }
    if (rejected)
        *rejected = refusals;
    if (applied.isEmpty())
        return {.teamId = teamId};
    // A human answer resets the budget and opens the digest lock once per submit.
    team->automaticTurns = 0;
    team->quiescedReason.clear();
    if (batch) {
        QStringList stillOpen;
        for (const auto& decision : std::as_const(team->decisions))
            if (decision.state == "open")
                stillOpen.append(QStringLiteral("%1: %2").arg(decision.id, decision.question));
        if (!stillOpen.isEmpty())
            leaderBodies.append(QStringLiteral("Still open, with no answer submitted. "
                                               "Do not ask these questions again yet:\n%1")
                                    .arg(stillOpen.join(QLatin1Char('\n'))));
        if (applied.size() != 1)
            leaderTask.clear();
    }
    if (auto failed = commit(*team, before, actor, updates, batch))
        return {.error = failed};
    if (accepted)
        *accepted = applied;
    if (resetStall) {
        Stall& stall = stalls_[teamId];
        stall.signature.clear();
        stall.wakes = 0;
        stall.nudged.clear();
        if (wakeStalledLeader)
            stall.stopped.remove(leaderSession);
    }
    if (resume)
        (void)update(teamId, {{"paused", false}}, actor);

    const auto notify = [&](const QString& to, const QString& taskId, const QStringList& bodies) {
        const Team* current = this->team(teamId);
        if (!to.isEmpty() && current && current->member(to))
            wake(*current, {.teamId = teamId,
                            .teamName = current->name,
                            .toSession = to,
                            .kind = QStringLiteral("decision"),
                            .taskId = taskId,
                            .body = bodies.join(QStringLiteral("\n\n"))});
    };
    notify(leaderSession, leaderTask, leaderBodies);
    for (auto it = ownerBodies.cbegin(); it != ownerBodies.cend(); ++it)
        notify(it.key(), ownerTasks.value(it.key()), it.value());
    for (const QString& id : released) {
        const Team* current = this->team(teamId);
        if (const Task* held = current ? current->task(id) : nullptr)
            deliverAssignment(*current, *held, leaderSession, false);
    }
    return {.teamId = teamId};
}

// The one open "Start these N tasks?" decision, with the held assignments as they are now.
void TeamCoordinator::askFirstPlan(Team& team) {
    QStringList plan;
    for (const Task& entry : team.tasks)
        if (entry.assignmentHeld) {
            const Member* owner = team.member(entry.owner);
            plan << QStringLiteral("%1 \"%2\" → %3")
                        .arg(entry.id, entry.title, owner ? owner->name : entry.owner);
        }
    const QString question =
        say(team.language, QT_TRANSLATE_NOOP("Team", "Start these %1 tasks?")).arg(plan.size());
    const QString context = plan.join(QLatin1Char('\n'));
    if (Decision* open = openDecision(team, firstPlan)) {
        open->question = question;
        open->context = context;
    } else {
        raiseDecision(team, firstPlan, question, context,
                      {say(team.language, startPlan),
                       say(team.language, QT_TRANSLATE_NOOP("Team", "Change the plan first"))},
                      say(team.language, startPlan), {}, QStringLiteral("daemon"));
    }
}

// The user answered the first plan with something other than Start: the leader changes the
// plan, and at the end of its turn the user is asked again. Without this the assignments
// would stay held with no question open.
void TeamCoordinator::askFirstPlanAgain(const qint64 teamId) {
    const Team* current = team(teamId);
    if (!current || current->planApproved)
        return;
    const bool held = std::any_of(current->tasks.cbegin(), current->tasks.cend(),
                                  [](const Task& task) { return task.assignmentHeld; });
    Team* team = held ? mutableTeam(teamId) : nullptr;
    if (!team || openDecision(*team, firstPlan))
        return;
    const Team before = *team;
    askFirstPlan(*team);
    if (commit(*team, before, QStringLiteral("daemon"), {}))
        qWarning("Unable to ask about the first plan of team %lld again.",
                 static_cast<long long>(teamId));
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
    askFirstPlan(*team);
    if (commit(
            *team, before, QStringLiteral("daemon"),
            {{.kind = QStringLiteral("assignment_held"),
              .text = say(team->language,
                          QT_TRANSLATE_NOOP("Team", "%1 waits for the first plan to be approved."))
                          .arg(taskId),
              .taskId = taskId}}))
        return false;
    return true;
}

} // namespace ketplus::daemon::teams
