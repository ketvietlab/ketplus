#include "teams/TeamRemoteView.h"

#include "teams/Team.h"
#include "teams/TeamRules.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace ketplus::daemon::teams::remote {
namespace {

// At most `characters`, never half a surrogate pair.
QString cut(const QString& text, const qsizetype characters) {
    if (text.size() <= characters)
        return text;
    qsizetype keep = characters;
    if (keep > 0 && text.at(keep - 1).isHighSurrogate())
        --keep;
    return text.left(keep);
}

// At most `bytes` of UTF-8, cut on a character.
QString cutBytes(const QString& text, const qsizetype bytes) {
    QString result = text;
    while (result.toUtf8().size() > bytes)
        result = cut(result, result.size() - 1);
    return result;
}

QJsonArray cutAll(const QJsonArray& values, const qsizetype characters) {
    QJsonArray result;
    for (const QJsonValue& value : values)
        result.append(cut(value.toString(), characters));
    return result;
}

QJsonObject member(const QJsonObject& team, const QString& sessionId) {
    for (const QJsonValue& value : team.value(QStringLiteral("members")).toArray())
        if (value.toObject().value(QStringLiteral("session_id")).toString() == sessionId)
            return value.toObject();
    return {};
}

QJsonObject task(const QJsonObject& team, const QString& taskId) {
    for (const QJsonValue& value : team.value(QStringLiteral("tasks")).toArray())
        if (value.toObject().value(QStringLiteral("id")).toString() == taskId)
            return value.toObject();
    return {};
}

// An actor of an Update or of a history entry: a member, the user or the daemon.
QJsonObject actor(const QJsonObject& team, const QString& id) {
    if (id == QStringLiteral("user"))
        return {{QStringLiteral("role"), QStringLiteral("user")}};
    const QJsonObject found = member(team, id);
    if (found.isEmpty())
        return {{QStringLiteral("role"), QStringLiteral("daemon")}};
    return {{QStringLiteral("role"), found.value(QStringLiteral("role"))},
            {QStringLiteral("number"), found.value(QStringLiteral("number")).toString()},
            {QStringLiteral("session_id"), id}};
}

QString memberState(const QJsonObject& entry, const QJsonObject& team) {
    if (!entry.value(QStringLiteral("idle")).toBool(true))
        return QStringLiteral("working");
    bool review = false;
    const QString sessionId = entry.value(QStringLiteral("session_id")).toString();
    for (const QJsonValue& value : team.value(QStringLiteral("tasks")).toArray()) {
        const QJsonObject own = value.toObject();
        if (own.value(QStringLiteral("owner")).toString() != sessionId)
            continue;
        const QString state = own.value(QStringLiteral("state")).toString();
        if (state == state::blocked)
            return QStringLiteral("blocked");
        review = review || state == state::inReview;
    }
    return review ? QStringLiteral("waiting_for_review") : QStringLiteral("idle");
}

QJsonObject phoneMember(const QJsonObject& entry, const QJsonObject& team) {
    const QString role = entry.value(QStringLiteral("role")).toString();
    const QString current = entry.value(QStringLiteral("current_task")).toString();
    const QJsonObject now = current.isEmpty() ? QJsonObject{} : task(team, current);
    QJsonObject json{
        {QStringLiteral("session_id"), entry.value(QStringLiteral("session_id"))},
        {QStringLiteral("session_generation"),
         entry.value(QStringLiteral("session_generation")).toInteger(1)},
        {QStringLiteral("number"), entry.value(QStringLiteral("number")).toString()},
        {QStringLiteral("title"), entry.value(QStringLiteral("title"))
                                      .toString(entry.value(QStringLiteral("name")).toString())},
        {QStringLiteral("role"), role},
        {QStringLiteral("state"), memberState(entry, team)},
        {QStringLiteral("now"),
         now.isEmpty() ? QJsonValue(QJsonValue::Null)
                       : QJsonObject{{QStringLiteral("task_id"), current},
                                     {QStringLiteral("title"),
                                      cut(now.value(QStringLiteral("title")).toString(), 200)}}},
        {QStringLiteral("agent"),
         QJsonObject{{QStringLiteral("provider"), entry.value(QStringLiteral("provider"))},
                     {QStringLiteral("model"), entry.value(QStringLiteral("model"))},
                     {QStringLiteral("effort"), entry.value(QStringLiteral("effort"))}}},
        {QStringLiteral("changed_revision"),
         team.value(QStringLiteral("members_revision")).toInteger()}};
    if (role == role::developer) {
        json.insert(QStringLiteral("lane"), entry.value(QStringLiteral("lane")).toString());
        json.insert(QStringLiteral("paths"), entry.value(QStringLiteral("paths")).toArray());
    }
    return json;
}

QString openDecisionOf(const QJsonObject& team, const QString& taskId) {
    for (const QJsonValue& value : team.value(QStringLiteral("decisions")).toArray()) {
        const QJsonObject decision = value.toObject();
        if (decision.value(QStringLiteral("state")).toString() == QStringLiteral("open") &&
            decision.value(QStringLiteral("task_id")).toString() == taskId)
            return decision.value(QStringLiteral("id")).toString();
    }
    return {};
}

QJsonObject phoneTask(const QJsonObject& local, const QJsonObject& team, const Ids& ids) {
    const QString worktree = local.value(QStringLiteral("worktree")).toString();
    const QJsonArray outside = local.value(QStringLiteral("outside_lane")).toArray();
    return {{QStringLiteral("id"), local.value(QStringLiteral("id"))},
            {QStringLiteral("title"), cut(local.value(QStringLiteral("title")).toString(), 200)},
            {QStringLiteral("state"), local.value(QStringLiteral("state"))},
            {QStringLiteral("owner"), local.value(QStringLiteral("owner")).toString()},
            {QStringLiteral("lane"), local.value(QStringLiteral("lane")).toString()},
            {QStringLiteral("depends_on"), local.value(QStringLiteral("depends_on")).toArray()},
            {QStringLiteral("criteria_total"),
             local.value(QStringLiteral("criteria")).toArray().size()},
            {QStringLiteral("criteria_met"), local.value(QStringLiteral("criteria_met")).toInt()},
            {QStringLiteral("round"), local.value(QStringLiteral("round")).toInt()},
            {QStringLiteral("pull_request"),
             cut(local.value(QStringLiteral("pull_request")).toString(), 500)},
            {QStringLiteral("branch"), local.value(QStringLiteral("branch")).toString()},
            {QStringLiteral("worktree_id"),
             worktree.isEmpty() || !ids.worktree ? QString{} : ids.worktree(worktree)},
            {QStringLiteral("assignment_held"),
             local.value(QStringLiteral("assignment_held")).toBool()},
            {QStringLiteral("outside_lane_total"),
             local.value(QStringLiteral("outside_lane_total")).toInt(int(outside.size()))},
            {QStringLiteral("decision_id"),
             openDecisionOf(team, local.value(QStringLiteral("id")).toString())},
            {QStringLiteral("changed_revision"), local.value(QStringLiteral("changed_revision"))}};
}

QJsonObject phoneDecision(const QJsonObject& local, const QJsonObject& team) {
    const QJsonObject raiser = member(team, local.value(QStringLiteral("raised_by")).toString());
    const QJsonObject raisedBy =
        raiser.value(QStringLiteral("role")).toString() == role::leader
            ? QJsonObject{{QStringLiteral("role"), role::leader},
                          {QStringLiteral("number"),
                           raiser.value(QStringLiteral("number")).toString()}}
            : QJsonObject{{QStringLiteral("role"), QStringLiteral("daemon")}};
    // Where the answer goes: the leader, and the owner of the task the decision blocks.
    QJsonArray deliversTo;
    QSet<QString> named;
    const auto deliver = [&](const QJsonObject& entry) {
        const QString sessionId = entry.value(QStringLiteral("session_id")).toString();
        if (sessionId.isEmpty() || named.contains(sessionId))
            return;
        named.insert(sessionId);
        deliversTo.append(QJsonObject{
            {QStringLiteral("role"), entry.value(QStringLiteral("role"))},
            {QStringLiteral("session_id"), sessionId},
            {QStringLiteral("number"), entry.value(QStringLiteral("number")).toString()}});
    };
    for (const QJsonValue& value : team.value(QStringLiteral("members")).toArray())
        if (value.toObject().value(QStringLiteral("role")).toString() == role::leader)
            deliver(value.toObject());
    const QString taskId = local.value(QStringLiteral("task_id")).toString();
    if (!taskId.isEmpty())
        deliver(member(team, task(team, taskId).value(QStringLiteral("owner")).toString()));
    const QString recommended = local.value(QStringLiteral("recommended")).toString();
    return {
        {QStringLiteral("id"), local.value(QStringLiteral("id"))},
        {QStringLiteral("kind"), local.value(QStringLiteral("kind"))},
        {QStringLiteral("question"), cut(local.value(QStringLiteral("question")).toString(), 1000)},
        {QStringLiteral("context"), cut(local.value(QStringLiteral("context")).toString(), 4000)},
        {QStringLiteral("choices"), cutAll(local.value(QStringLiteral("choices")).toArray(), 200)},
        {QStringLiteral("recommended"),
         recommended.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(cut(recommended, 200))},
        {QStringLiteral("task_id"), taskId},
        {QStringLiteral("raised_by"), raisedBy},
        {QStringLiteral("raised_at"), local.value(QStringLiteral("raised_at"))},
        {QStringLiteral("state"), local.value(QStringLiteral("state"))},
        {QStringLiteral("answer"), local.value(QStringLiteral("answer")).toString()},
        {QStringLiteral("answered_at"), local.value(QStringLiteral("answered_at")).toString()},
        {QStringLiteral("delivers_to"), deliversTo},
        {QStringLiteral("changed_revision"), local.value(QStringLiteral("changed_revision"))}};
}

// The lane a repository path belongs to, and the developer whose lane it is.
std::pair<QString, QJsonValue> laneOf(const QString& path, const QJsonObject& team) {
    qsizetype best = -1;
    std::pair<QString, QJsonValue> found{QString{}, QJsonValue::Null};
    for (const QJsonValue& value : team.value(QStringLiteral("members")).toArray()) {
        const QJsonObject entry = value.toObject();
        for (const QJsonValue& prefix : entry.value(QStringLiteral("paths")).toArray()) {
            const QString lanePath = prefix.toString();
            if (lanePath.size() > best && rules::pathUnder(path, lanePath)) {
                best = lanePath.size();
                found = {entry.value(QStringLiteral("lane")).toString(),
                         entry.value(QStringLiteral("number")).toString()};
            }
        }
    }
    return found;
}

qsizetype size(const QJsonObject& json) {
    return QJsonDocument(json).toJson(QJsonDocument::Compact).size();
}

} // namespace

bool isWarning(const QJsonObject& update) {
    static const QSet<QString> kinds{
        QStringLiteral("blocked"),       QStringLiteral("round_limit"),
        QStringLiteral("outside_lane"),  QStringLiteral("merged_unreviewed"),
        QStringLiteral("stalled"),       QStringLiteral("message_failed"),
        QStringLiteral("merge_waiting"), QStringLiteral("review_tampered")};
    const QString kind = update.value(QStringLiteral("kind")).toString();
    return kinds.contains(kind) ||
           (kind == QStringLiteral("state") &&
            update.value(QStringLiteral("data")).toObject().value(QStringLiteral("to")) ==
                state::blocked);
}

std::optional<int> pullRequestNumber(const QString& pullRequest) {
    static const QRegularExpression number(QStringLiteral("(?:/pull/|#)([0-9]{1,9})/?$"));
    const QRegularExpressionMatch match = number.match(pullRequest.trimmed());
    if (!match.hasMatch())
        return std::nullopt;
    return match.captured(1).toInt();
}

QJsonObject directoryEntry(const QJsonObject& team, const Ids& ids) {
    QJsonArray members;
    for (const QJsonValue& value : team.value(QStringLiteral("members")).toArray()) {
        if (members.size() >= maximumMembers)
            break;
        const QJsonObject entry = value.toObject();
        members.append(QJsonObject{
            {QStringLiteral("session_id"), entry.value(QStringLiteral("session_id"))},
            {QStringLiteral("number"), entry.value(QStringLiteral("number")).toString()},
            {QStringLiteral("role"), entry.value(QStringLiteral("role"))}});
    }
    const QString repository = team.value(QStringLiteral("repository")).toString();
    QJsonObject entry{
        {QStringLiteral("id"), team.value(QStringLiteral("id"))},
        {QStringLiteral("revision"), team.value(QStringLiteral("revision"))},
        {QStringLiteral("repository_id"), ids.repository ? ids.repository(repository) : QString{}},
        {QStringLiteral("name"), cutBytes(team.value(QStringLiteral("name")).toString(), 120)},
        {QStringLiteral("paused"), team.value(QStringLiteral("paused")).toBool()},
        {QStringLiteral("paused_reason"), team.value(QStringLiteral("paused_reason")).toString()},
        {QStringLiteral("open_decisions"), team.value(QStringLiteral("open_decisions")).toInt()},
        {QStringLiteral("tasks_total"),
         team.value(QStringLiteral("tasks_total"))
             .toInt(int(team.value(QStringLiteral("tasks")).toArray().size()))},
        {QStringLiteral("tasks_done"), team.value(QStringLiteral("tasks_done")).toInt()},
        {QStringLiteral("members"), members}};
    if (team.contains(QStringLiteral("start_at")))
        entry.insert(QStringLiteral("start_at"), team.value(QStringLiteral("start_at")));
    if (team.contains(QStringLiteral("status")))
        entry.insert(QStringLiteral("status"), team.value(QStringLiteral("status")));
    return entry;
}

QJsonObject summary(const QJsonObject& team, const Ids& ids) {
    QJsonObject json = directoryEntry(team, ids);
    json.remove(QStringLiteral("members"));
    const int budget = team.value(QStringLiteral("turn_budget")).toInt();
    const int used = team.value(QStringLiteral("automatic_turns")).toInt();
    json.insert(QStringLiteral("goal"),
                cut(team.value(QStringLiteral("goal")).toString(), rules::maximumGoalCharacters));
    json.insert(QStringLiteral("integration_branch"),
                team.value(QStringLiteral("integration_branch")).toString());
    json.insert(QStringLiteral("merge_policy"), team.value(QStringLiteral("merge_policy")));
    json.insert(QStringLiteral("leader_writes_requirements"),
                team.value(QStringLiteral("leader_writes_requirements")).toBool(true));
    json.insert(QStringLiteral("execution_policy"), team.value(QStringLiteral("execution_policy")));
    json.insert(QStringLiteral("language"), team.value(QStringLiteral("language")));
    json.insert(QStringLiteral("plan_approved"),
                team.value(QStringLiteral("plan_approved")).toBool());
    json.insert(QStringLiteral("held_count"), team.value(QStringLiteral("held_count")).toInt());
    json.insert(QStringLiteral("turn_budget"), budget);
    json.insert(QStringLiteral("automatic_turns"), used);
    json.insert(QStringLiteral("turns_left"), std::max(0, budget - used));
    // T-53: the review passes per PR, read-only on the phone (the desktop changes it).
    json.insert(QStringLiteral("max_rounds"), team.value(QStringLiteral("max_rounds")).toInt());
    // T-23: "reviewing" and "queued" stay as they were for one reviewer; "reviewers" adds what
    // each reviewer reviews and has queued, in the order they joined.
    QJsonArray reviewers;
    for (const QJsonValue& value : team.value(QStringLiteral("reviews")).toArray()) {
        const QJsonObject slot = value.toObject();
        reviewers.append(QJsonObject{
            {QStringLiteral("session_id"), slot.value(QStringLiteral("reviewer")).toString()},
            {QStringLiteral("reviewing"), slot.value(QStringLiteral("reviewing")).toString()},
            {QStringLiteral("queued"), slot.value(QStringLiteral("queue")).toArray()}});
    }
    json.insert(
        QStringLiteral("review"),
        QJsonObject{
            {QStringLiteral("reviewing"), team.value(QStringLiteral("reviewing")).toString()},
            {QStringLiteral("queued"), team.value(QStringLiteral("review_queue")).toArray()},
            {QStringLiteral("reviewers"), reviewers}});
    json.insert(QStringLiteral("decisions_total"),
                team.value(QStringLiteral("decisions_total")).toInt());
    json.insert(QStringLiteral("created_at"), team.value(QStringLiteral("created_at")));
    json.insert(QStringLiteral("changed_at"), team.value(QStringLiteral("changed_at")));

    QJsonArray members;
    for (const QJsonValue& value : team.value(QStringLiteral("members")).toArray())
        if (members.size() < maximumMembers)
            members.append(phoneMember(value.toObject(), team));
    json.insert(QStringLiteral("members"), members);

    // Every task that is not done, then the newest done ones.
    QJsonArray open;
    QJsonArray done;
    const QJsonArray localTasks = team.value(QStringLiteral("tasks")).toArray();
    for (const QJsonValue& value : localTasks) {
        const QJsonObject entry = value.toObject();
        (entry.value(QStringLiteral("state")).toString() == state::done ? done : open)
            .append(phoneTask(entry, team, ids));
    }
    QJsonArray tasks;
    for (const QJsonValue& entry : std::as_const(open))
        if (tasks.size() < maximumTasks)
            tasks.append(entry);
    for (qsizetype index = done.size() - 1; index >= 0 && tasks.size() < maximumTasks; --index)
        tasks.append(done.at(index));
    json.insert(QStringLiteral("tasks"), tasks);

    // The open decisions, then the newest answered or withdrawn ones.
    QJsonArray openDecisions;
    QJsonArray closedDecisions;
    const QJsonArray localDecisions = team.value(QStringLiteral("decisions")).toArray();
    for (qsizetype index = localDecisions.size() - 1; index >= 0; --index) {
        const QJsonObject entry = localDecisions.at(index).toObject();
        const bool isOpen =
            entry.value(QStringLiteral("state")).toString() == QStringLiteral("open");
        QJsonArray& list = isOpen ? openDecisions : closedDecisions;
        if (list.size() < (isOpen ? maximumOpenDecisions : maximumClosedDecisions))
            list.append(phoneDecision(entry, team));
    }
    QJsonArray decisions = openDecisions;
    for (const QJsonValue& entry : std::as_const(closedDecisions))
        decisions.append(entry);
    json.insert(QStringLiteral("decisions"), decisions);
    return json;
}

QJsonArray updates(const QJsonArray& local, const QJsonObject& team) {
    QJsonArray result;
    for (const QJsonValue& value : local) {
        const QJsonObject entry = value.toObject();
        const QString taskId = entry.value(QStringLiteral("task_id")).toString();
        const QJsonObject data = entry.value(QStringLiteral("data")).toObject();
        std::optional<int> number =
            pullRequestNumber(data.value(QStringLiteral("pull_request")).toString());
        if (!number && !taskId.isEmpty())
            number = pullRequestNumber(
                task(team, taskId).value(QStringLiteral("pull_request")).toString());
        result.append(QJsonObject{
            {QStringLiteral("id"), entry.value(QStringLiteral("id"))},
            {QStringLiteral("kind"), entry.value(QStringLiteral("kind"))},
            {QStringLiteral("at"), entry.value(QStringLiteral("at"))},
            {QStringLiteral("actor"), actor(team, entry.value(QStringLiteral("actor")).toString())},
            {QStringLiteral("task_id"), taskId},
            {QStringLiteral("pull_request_number"),
             number ? QJsonValue(*number) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("text"), cut(entry.value(QStringLiteral("text")).toString(), 500)},
            {QStringLiteral("actionable"), entry.value(QStringLiteral("actionable")).toBool()},
            {QStringLiteral("warning"), isWarning(entry)}});
    }
    return result;
}

QJsonArray decisions(const QJsonArray& local, const QJsonObject& team) {
    QJsonArray result;
    for (const QJsonValue& value : local)
        result.append(phoneDecision(value.toObject(), team));
    return result;
}

QJsonObject taskDetail(const QJsonObject& local, const QJsonObject& team, const Ids& ids) {
    QJsonObject json = phoneTask(local, team, ids);
    json.insert(QStringLiteral("description"),
                cutBytes(local.value(QStringLiteral("description")).toString(), 16 * 1024));
    QJsonArray criteria;
    for (const QJsonValue& value : local.value(QStringLiteral("criteria")).toArray()) {
        if (criteria.size() >= maximumCriteria)
            break;
        const QJsonObject entry = value.toObject();
        criteria.append(QJsonObject{{QStringLiteral("text"), entry.value(QStringLiteral("text"))},
                                    {QStringLiteral("met"), entry.value(QStringLiteral("met"))}});
    }
    json.insert(QStringLiteral("criteria"), criteria);
    QJsonArray outside;
    for (const QJsonValue& value : local.value(QStringLiteral("outside_lane")).toArray()) {
        if (outside.size() >= summaryOutsidePaths)
            break;
        const auto [lane, owner] = laneOf(value.toString(), team);
        outside.append(QJsonObject{{QStringLiteral("path"), value.toString()},
                                   {QStringLiteral("lane"), lane},
                                   {QStringLiteral("owner_number"), owner}});
    }
    json.insert(QStringLiteral("outside_lane"), outside);
    // The newest change to the criteria, from the whole history the page was read from.
    QString note;
    QJsonArray history;
    const QJsonArray localHistory = local.value(QStringLiteral("history")).toArray();
    for (const QJsonValue& value : localHistory) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("kind")).toString() == QStringLiteral("criteria"))
            note = cut(QStringLiteral("Changed %1: %2")
                           .arg(entry.value(QStringLiteral("at")).toString(),
                                entry.value(QStringLiteral("text")).toString()),
                       500);
        history.append(QJsonObject{
            {QStringLiteral("at"), entry.value(QStringLiteral("at"))},
            {QStringLiteral("actor"), actor(team, entry.value(QStringLiteral("actor")).toString())},
            {QStringLiteral("kind"), entry.value(QStringLiteral("kind"))},
            {QStringLiteral("text"), cut(entry.value(QStringLiteral("text")).toString(), 2000)}});
    }
    if (local.contains(QStringLiteral("criteria_note")))
        note = local.value(QStringLiteral("criteria_note")).toString();
    json.insert(QStringLiteral("criteria_note"), note);
    json.insert(QStringLiteral("history"), history);
    return json;
}

std::optional<QJsonObject> bounded(QJsonObject result) {
    if (size(result) <= maximumResultBytes)
        return result;
    QJsonObject team = result.value(QStringLiteral("team")).toObject();
    const auto shed = [&](const QString& part, const auto& keep) {
        if (part == QStringLiteral("updates")) {
            if (result.contains(QStringLiteral("updates"))) {
                result.insert(QStringLiteral("updates"), QJsonArray{});
                result.insert(QStringLiteral("updates_truncated"), true);
            }
        } else {
            QJsonArray kept;
            bool dropped = false;
            for (const QJsonValue& value : team.value(part).toArray()) {
                if (keep(value.toObject()))
                    kept.append(value);
                else
                    dropped = true;
            }
            team.insert(part, kept);
            if (dropped)
                team.insert(part + QStringLiteral("_truncated"), true);
            result.insert(QStringLiteral("team"), team);
        }
        return size(result) <= maximumResultBytes;
    };
    const auto open = [](const QJsonObject& decision) {
        return decision.value(QStringLiteral("state")).toString() == QStringLiteral("open");
    };
    const auto notDone = [](const QJsonObject& entry) {
        return entry.value(QStringLiteral("state")).toString() != state::done;
    };
    if (shed(QStringLiteral("decisions"), open) || shed(QStringLiteral("updates"), open) ||
        shed(QStringLiteral("tasks"), notDone))
        return result;
    return std::nullopt;
}

} // namespace ketplus::daemon::teams::remote
