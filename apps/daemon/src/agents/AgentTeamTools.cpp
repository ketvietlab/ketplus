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

QJsonObject flag(const QString& description) {
    return {{QStringLiteral("type"), QStringLiteral("boolean")},
            {QStringLiteral("description"), description}};
}

QJsonObject number(const QString& description, const int minimum, const int maximum = 0) {
    QJsonObject json{{QStringLiteral("type"), QStringLiteral("integer")},
                     {QStringLiteral("minimum"), minimum},
                     {QStringLiteral("description"), description}};
    if (maximum > 0)
        json.insert(QStringLiteral("maximum"), maximum);
    return json;
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
                                   QStringLiteral("team_task_close"),
                                   QStringLiteral("team_review_void"),
                                   QStringLiteral("team_review_accept"),
                                   QStringLiteral("team_lane_set"),
                                   QStringLiteral("team_paths_allow"),
                                   QStringLiteral("team_repository_set"),
                                   QStringLiteral("team_decision_raise"),
                                   QStringLiteral("team_decision_withdraw")};
    return value;
}

} // namespace

QJsonArray teamTools() {
    const QJsonObject taskId = text(QStringLiteral("The task id, such as T-3."));
    // T-93 (ADR 0121 §9): the branch `<type>/<slug>-t<n>` a task's worktree gets.
    const QJsonObject branchType{
        {QStringLiteral("type"), QStringLiteral("string")},
        {QStringLiteral("enum"),
         QJsonArray{QStringLiteral("feat"), QStringLiteral("fix"), QStringLiteral("chore"),
                    QStringLiteral("docs"), QStringLiteral("refactor"), QStringLiteral("test")}},
        {QStringLiteral("description"),
         QStringLiteral("Leader, optional: the branch's type. Default: read from the title, "
                        "else feat.")}};
    const QJsonObject branchSlug =
        text(QStringLiteral("Leader, optional: the branch's slug, lower-case words joined by "
                            "hyphens, at most 40 characters (the branch is <type>/<slug>-t<n>). "
                            "Default: from the title."));
    const QJsonObject member = text(QStringLiteral("The member's conversation number."));
    return {
        tool(QStringLiteral("team_get"),
             QStringLiteral("Your team: its goal, settings, members with roles and lanes, and "
                            "your own role and lane."),
             {}),
        tool(QStringLiteral("team_tasks"),
             QStringLiteral(
                 "Without task_id: one short row per task (id, title, state, round, owner, "
                 "lane, depends_on, pull_request, and the question of a blocked task); "
                 "open=true leaves out done tasks, mine=true keeps the tasks you own or review. "
                 "With task_id: that task's description, criteria and its last 10 history "
                 "entries, with history_total and history_start; history_start and "
                 "history_limit (at most 50) read any other part. Each entry has its index; "
                 "a cut text says so, and entry=<index> returns that entry whole."),
             {{QStringLiteral("task_id"), taskId},
              {QStringLiteral("open"), flag(QStringLiteral("Only tasks that are not done."))},
              {QStringLiteral("mine"), flag(QStringLiteral("Only the tasks you own or review."))},
              {QStringLiteral("history_start"),
               number(QStringLiteral("With task_id: the first history entry to read, from 0."), 0)},
              {QStringLiteral("history_limit"),
               number(QStringLiteral("With task_id: how many entries, 1 to 50 (default 10)."), 1,
                      50)},
              {QStringLiteral("entry"),
               number(QStringLiteral("With task_id: one history entry by index, uncut."), 0)}}),
        tool(QStringLiteral("team_task_update"),
             QStringLiteral("Developers, on their own task: move it (in_progress, in_review once "
                            "committed, blocked with a question), add a note, mark criteria or "
                            "set the pull request. Leader: rename a task's branch with "
                            "branch_type/branch_slug before its worktree exists. Starts no turn "
                            "of anybody."),
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
              {QStringLiteral("head_commit"),
               text(QStringLiteral(
                   "With in_review, for work done in another worktree or repository: the "
                   "commit the pull request is reviewed at. A commit on the task's own branch "
                   "is read from git instead."))},
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
                            QStringLiteral("Criteria marks by zero-based index.")}}},
              {QStringLiteral("branch_type"), branchType},
              {QStringLiteral("branch_slug"), branchSlug}},
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
               list(QStringLiteral("Task ids that must be done first."))},
              {QStringLiteral("repository"),
               text(QStringLiteral("Optional: the full path of another repository's main "
                                   "checkout this task works in. Its branch, worktree, merge "
                                   "check and cleanup use it. Default: the team's."))},
              {QStringLiteral("integration_branch"),
               text(QStringLiteral("Optional, with repository: its integration branch, which "
                                   "origin must have. Default: the team's."))},
              {QStringLiteral("branch_type"), branchType},
              {QStringLiteral("branch_slug"), branchSlug}},
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
        tool(QStringLiteral("team_task_close"),
             QStringLiteral("Leader: mark a task done by hand, when its work is in with no pull "
                            "request or the remote cannot be reached. A merged pull request "
                            "closes its task by itself."),
             {{QStringLiteral("task_id"), taskId},
              {QStringLiteral("evidence"),
               text(QStringLiteral("A pull request URL or the commit that contains the work."))},
              {QStringLiteral("note"), text(QStringLiteral("Why the task is done."))}},
             {QStringLiteral("task_id"), QStringLiteral("evidence"), QStringLiteral("note")}),
        tool(QStringLiteral("team_review_void"),
             QStringLiteral("Leader: void the last recorded review verdict of a task that should "
                            "not have counted. The round counter goes back by one and the task "
                            "returns to in review; the history keeps the verdict and the reason."),
             {{QStringLiteral("task_id"), taskId},
              {QStringLiteral("reason"), text(QStringLiteral("Why the verdict does not count."))}},
             {QStringLiteral("task_id"), QStringLiteral("reason")}),
        tool(QStringLiteral("team_review_accept"),
             QStringLiteral("Leader: accept a review made outside the board, for example on the "
                            "pull request. A task in review, or blocked by its review, becomes "
                            "ready."),
             {{QStringLiteral("task_id"), taskId},
              {QStringLiteral("evidence"),
               text(QStringLiteral("Where the review is: a pull request review or comment URL, "
                                   "or what was checked."))},
              {QStringLiteral("reason"), text(QStringLiteral("Why it is accepted."))}},
             {QStringLiteral("task_id"), QStringLiteral("evidence"), QStringLiteral("reason")}),
        tool(QStringLiteral("team_lane_set"),
             QStringLiteral("Leader: change a developer's lane and path prefixes. Logged in "
                            "Updates and in the history of the tasks it affects."),
             {{QStringLiteral("developer"), member},
              {QStringLiteral("lane"), text(QStringLiteral("The lane's name."))},
              {QStringLiteral("paths"),
               list(QStringLiteral("Path prefixes relative to the repository."))},
              {QStringLiteral("reason"), text(QStringLiteral("Why the lane changes."))}},
             {QStringLiteral("developer"), QStringLiteral("paths"), QStringLiteral("reason")}),
        tool(QStringLiteral("team_paths_allow"),
             QStringLiteral("Leader: name the paths any lane may change, such as contracts, "
                            "snapshots, fixtures or tests: the team's shared paths, or with "
                            "task_id that task's own. The list replaces the one before; a "
                            "change there is never flagged outside a lane."),
             {{QStringLiteral("task_id"), text(QStringLiteral("The task, or none for the team."))},
              {QStringLiteral("paths"),
               list(QStringLiteral("Paths relative to the repository root, such as contracts "
                                   "or tests/fixtures."))},
              {QStringLiteral("reason"), text(QStringLiteral("Why, recorded in the history."))}},
             {QStringLiteral("paths"), QStringLiteral("reason")}),
        tool(QStringLiteral("team_repository_set"),
             QStringLiteral("Leader: move the team to another repository (the full path of its "
                            "main checkout) and integration branch, which origin must have. "
                            "New tasks and tasks not started yet use it; started tasks keep the "
                            "repository, branch and worktree they began in. Nothing on disk "
                            "moves. Logged in Updates."),
             {{QStringLiteral("repository"),
               text(QStringLiteral("The full path of the repository's main checkout."))},
              {QStringLiteral("integration_branch"),
               text(QStringLiteral("Its integration branch. Default: the team's current one."))},
              {QStringLiteral("reason"), text(QStringLiteral("Why, recorded in Updates."))}},
             {QStringLiteral("repository"), QStringLiteral("reason")}),
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
