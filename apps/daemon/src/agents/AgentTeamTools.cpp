#include "agents/AgentTeamTools.h"

#include <QJsonObject>
#include <QStringList>

namespace ketplus::daemon::agent_mcp {
namespace {

QJsonObject text(const QString& description) {
    return {{QStringLiteral("type"), QStringLiteral("string")},
            {QStringLiteral("description"), description}};
}

QJsonObject list(const QString& description) {
    return {
        {QStringLiteral("type"), QStringLiteral("array")},
        {QStringLiteral("items"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
        {QStringLiteral("description"), description}};
}

QJsonObject tool(const QString& name, const QString& description, const QJsonObject& properties,
                 const QStringList& required = {}) {
    QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")},
                       {QStringLiteral("properties"), properties},
                       {QStringLiteral("additionalProperties"), false}};
    if (!required.isEmpty())
        schema.insert(QStringLiteral("required"), QJsonArray::fromStringList(required));
    return {{QStringLiteral("name"), name},
            {QStringLiteral("description"), description},
            {QStringLiteral("inputSchema"), schema}};
}

const QStringList& names() {
    static const QStringList value{QStringLiteral("team_get"),
                                   QStringLiteral("team_tasks"),
                                   QStringLiteral("team_task_update"),
                                   QStringLiteral("team_wait"),
                                   QStringLiteral("team_task_create"),
                                   QStringLiteral("team_task_assign"),
                                   QStringLiteral("team_task_handover"),
                                   QStringLiteral("team_task_criteria"),
                                   QStringLiteral("team_lane_set"),
                                   QStringLiteral("team_decision_raise"),
                                   QStringLiteral("team_decision_withdraw")};
    return value;
}

} // namespace

QJsonArray teamTools() {
    const QJsonObject taskId = text(QStringLiteral("The task id, such as T-3."));
    const QJsonObject member = text(QStringLiteral("The member's conversation number."));
    return {
        tool(QStringLiteral("team_get"),
             QStringLiteral("Your team: its goal, settings, members with roles and lanes, and "
                            "your own role and lane."),
             {}),
        tool(QStringLiteral("team_tasks"),
             QStringLiteral("The team's tasks, or one task with its criteria and history."),
             {{QStringLiteral("task_id"), taskId}}),
        tool(QStringLiteral("team_task_update"),
             QStringLiteral("Developers, on their own task: move it (in_progress, in_review once "
                            "committed, blocked with a question), add a note, mark criteria or "
                            "set the pull request. Starts no turn of anybody."),
             {{QStringLiteral("task_id"), taskId},
              {QStringLiteral("state"),
               QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                           {QStringLiteral("enum"),
                            QJsonArray{QStringLiteral("in_progress"), QStringLiteral("in_review"),
                                       QStringLiteral("blocked")}}}},
              {QStringLiteral("question"),
               text(QStringLiteral("For blocked: what you need answered."))},
              {QStringLiteral("note"),
               text(QStringLiteral("Progress worth recording on the task."))},
              {QStringLiteral("pull_request"), text(QStringLiteral("The pull request, if any."))},
              {QStringLiteral("met"),
               QJsonObject{{QStringLiteral("type"), QStringLiteral("array")},
                           {QStringLiteral("items"),
                            QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                        {QStringLiteral("properties"),
                                         QJsonObject{{QStringLiteral("index"),
                                                      QJsonObject{{QStringLiteral("type"),
                                                                   QStringLiteral("integer")}}},
                                                     {QStringLiteral("met"),
                                                      QJsonObject{{QStringLiteral("type"),
                                                                   QStringLiteral("boolean")}}}}}}},
                           {QStringLiteral("description"),
                            QStringLiteral("Criteria marks by zero-based index.")}}}},
             {QStringLiteral("task_id")}),
        tool(QStringLiteral("team_wait"),
             QStringLiteral("Leader and developers: you wait for something outside the team (a "
                            "build, a deployment, another conversation's answer). Records it "
                            "for the team and starts your turn again after `minutes` so you "
                            "check. Call it, then end your turn; do not poll and do not end "
                            "the turn without it."),
             {{QStringLiteral("reason"), text(QStringLiteral("What you wait for."))},
              {QStringLiteral("minutes"),
               QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                           {QStringLiteral("minimum"), 1},
                           {QStringLiteral("maximum"), 60},
                           {QStringLiteral("description"),
                            QStringLiteral("When to check again, 1 to 60.")}}},
              {QStringLiteral("task_id"), text(QStringLiteral("The task it holds up, if any."))}},
             {QStringLiteral("reason"), QStringLiteral("minutes")}),
        tool(QStringLiteral("team_task_create"),
             QStringLiteral("Leader: create a task with its acceptance criteria, lane and the "
                            "tasks it depends on."),
             {{QStringLiteral("title"), text(QStringLiteral("What the task delivers."))},
              {QStringLiteral("description"), text(QStringLiteral("Context for the owner."))},
              {QStringLiteral("lane"), text(QStringLiteral("The lane it is planned for."))},
              {QStringLiteral("criteria"), list(QStringLiteral("Acceptance criteria, in order."))},
              {QStringLiteral("depends_on"),
               list(QStringLiteral("Task ids that must be done first."))}},
             {QStringLiteral("title")}),
        tool(QStringLiteral("team_task_assign"),
             QStringLiteral("Leader: assign a To do task to a developer. Starts the owner's turn."),
             {{QStringLiteral("task_id"), taskId}, {QStringLiteral("owner"), member}},
             {QStringLiteral("task_id"), QStringLiteral("owner")}),
        tool(QStringLiteral("team_task_handover"),
             QStringLiteral("Leader: give a task to another developer, with its worktree, branch "
                            "and history."),
             {{QStringLiteral("task_id"), taskId},
              {QStringLiteral("owner"), member},
              {QStringLiteral("reason"), text(QStringLiteral("Why, recorded in the history."))}},
             {QStringLiteral("task_id"), QStringLiteral("owner"), QStringLiteral("reason")}),
        tool(QStringLiteral("team_task_criteria"),
             QStringLiteral("Leader: replace the text of a task's acceptance criteria. The old "
                            "and new text are kept in its history."),
             {{QStringLiteral("task_id"), taskId},
              {QStringLiteral("criteria"), list(QStringLiteral("The criteria, in order."))}},
             {QStringLiteral("task_id"), QStringLiteral("criteria")}),
        tool(QStringLiteral("team_lane_set"),
             QStringLiteral("Leader: change a developer's lane and path prefixes. Logged in "
                            "Updates and in the history of the tasks it affects."),
             {{QStringLiteral("developer"), member},
              {QStringLiteral("lane"), text(QStringLiteral("The lane's name."))},
              {QStringLiteral("paths"),
               list(QStringLiteral("Path prefixes relative to the repository."))},
              {QStringLiteral("reason"), text(QStringLiteral("Why the lane changes."))}},
             {QStringLiteral("developer"), QStringLiteral("paths"), QStringLiteral("reason")}),
        tool(
            QStringLiteral("team_decision_raise"),
            QStringLiteral("Leader: ask the user a question that changes the product, with two "
                           "to four choices and your recommendation. Shown on the team page."),
            {{QStringLiteral("question"), text(QStringLiteral("The question."))},
             {QStringLiteral("context"), text(QStringLiteral("A short context."))},
             {QStringLiteral("choices"), list(QStringLiteral("Two to four choices."))},
             {QStringLiteral("recommended"), text(QStringLiteral("The choice you recommend."))},
             {QStringLiteral("task_id"), text(QStringLiteral("The task it blocks, if any."))}},
            {QStringLiteral("question"), QStringLiteral("choices"), QStringLiteral("recommended")}),
        tool(
            QStringLiteral("team_decision_withdraw"),
            QStringLiteral("Leader: withdraw a decision you raised that is no longer needed, "
                           "with the reason."),
            {{QStringLiteral("decision_id"), text(QStringLiteral("The decision id, such as D-2."))},
             {QStringLiteral("reason"), text(QStringLiteral("Why it is no longer needed."))}},
            {QStringLiteral("decision_id"), QStringLiteral("reason")})};
}

QString teamOperationFor(const QString& tool) {
    return names().contains(tool) ? QStringLiteral("agents.") + tool : QString{};
}

} // namespace ketplus::daemon::agent_mcp
