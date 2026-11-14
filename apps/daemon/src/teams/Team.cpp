#include "teams/Team.h"

#include <QDateTime>

#include <algorithm>

namespace ketplus::daemon::teams {
namespace {

QJsonArray strings(const QStringList& values) { return QJsonArray::fromStringList(values); }

QStringList stringList(const QJsonValue& value) {
    QStringList result;
    for (const auto& item : value.toArray())
        result.append(item.toString());
    return result;
}

HistoryEntry historyFromJson(const QJsonObject& json) {
    return bounded({.at = json.value(QStringLiteral("at")).toString(),
                    .actor = json.value(QStringLiteral("actor")).toString(),
                    .kind = json.value(QStringLiteral("kind")).toString(),
                    .text = json.value(QStringLiteral("text")).toString(),
                    .data = json.value(QStringLiteral("data")).toObject()});
}

Member memberFromJson(const QJsonObject& json) {
    return {.sessionId = json.value(QStringLiteral("session_id")).toString(),
            .role = json.value(QStringLiteral("role")).toString(),
            .name = json.value(QStringLiteral("name")).toString(),
            .lane = json.value(QStringLiteral("lane")).toString(),
            .paths = stringList(json.value(QStringLiteral("paths"))),
            .joinedAt = json.value(QStringLiteral("joined_at")).toString()};
}

Task taskFromJson(const QJsonObject& json) {
    Task task;
    task.id = json.value(QStringLiteral("id")).toString();
    task.title = json.value(QStringLiteral("title")).toString();
    task.description = json.value(QStringLiteral("description")).toString();
    task.lane = json.value(QStringLiteral("lane")).toString();
    task.dependsOn = stringList(json.value(QStringLiteral("depends_on")));
    for (const auto& value : json.value(QStringLiteral("criteria")).toArray())
        task.criteria.append({.text = value.toObject().value(QStringLiteral("text")).toString(),
                              .met = value.toObject().value(QStringLiteral("met")).toBool()});
    task.owner = json.value(QStringLiteral("owner")).toString();
    task.state = json.value(QStringLiteral("state")).toString(state::todo);
    task.resumeState = json.value(QStringLiteral("resume_state")).toString();
    task.question = json.value(QStringLiteral("question")).toString();
    task.reviewBlock = json.value(QStringLiteral("review_block")).toString();
    task.branch = json.value(QStringLiteral("branch")).toString();
    task.worktree = json.value(QStringLiteral("worktree")).toString();
    task.baseCommit = json.value(QStringLiteral("base_commit")).toString();
    task.headCommit = json.value(QStringLiteral("head_commit")).toString();
    task.pullRequest = json.value(QStringLiteral("pull_request")).toString();
    task.repository = json.value(QStringLiteral("repository")).toString();
    task.integrationBranch = json.value(QStringLiteral("integration_branch")).toString();
    task.branchType = json.value(QStringLiteral("branch_type")).toString();
    task.branchSlug = json.value(QStringLiteral("branch_slug")).toString();
    task.round = json.value(QStringLiteral("round")).toInt();
    task.reviewer = json.value(QStringLiteral("reviewer")).toString();
    task.missingVerdicts = json.value(QStringLiteral("missing_verdicts")).toInt();
    task.roundLimit = json.value(QStringLiteral("round_limit")).toInt();
    task.reviewPlace = json.value(QStringLiteral("review_place")).toString();
    task.reviewComment = json.value(QStringLiteral("review_comment")).toString();
    task.outsideLane = stringList(json.value(QStringLiteral("outside_lane")));
    task.allowedPaths = stringList(json.value(QStringLiteral("allowed_paths")));
    if (task.outsideLane.size() > maximumOutsidePaths)
        task.outsideLane.resize(maximumOutsidePaths);
    task.assignmentHeld = json.value(QStringLiteral("assignment_held")).toBool();
    task.leaderMayMerge = json.value(QStringLiteral("leader_may_merge")).toBool();
    task.mergeOffered = json.value(QStringLiteral("merge_offered")).toBool();
    task.changedRevision = json.value(QStringLiteral("changed_revision")).toInteger();
    for (const auto& value : json.value(QStringLiteral("history")).toArray())
        task.history.append(historyFromJson(value.toObject()));
    return task;
}

Decision decisionFromJson(const QJsonObject& json) {
    return {.id = json.value(QStringLiteral("id")).toString(),
            .kind = json.value(QStringLiteral("kind")).toString(QStringLiteral("question")),
            .question = json.value(QStringLiteral("question")).toString(),
            .context = json.value(QStringLiteral("context")).toString(),
            .choices = stringList(json.value(QStringLiteral("choices"))),
            .recommended = json.value(QStringLiteral("recommended")).toString(),
            .taskId = json.value(QStringLiteral("task_id")).toString(),
            .raisedBy = json.value(QStringLiteral("raised_by")).toString(),
            .raisedAt = json.value(QStringLiteral("raised_at")).toString(),
            .state = json.value(QStringLiteral("state")).toString(QStringLiteral("open")),
            .answer = json.value(QStringLiteral("answer")).toString(),
            .answeredAt = json.value(QStringLiteral("answered_at")).toString(),
            .reason = json.value(QStringLiteral("reason")).toString(),
            .changedRevision = json.value(QStringLiteral("changed_revision")).toInteger()};
}

} // namespace

QJsonObject toJson(const HistoryEntry& entry) {
    QJsonObject json{{QStringLiteral("at"), entry.at},
                     {QStringLiteral("actor"), entry.actor},
                     {QStringLiteral("kind"), entry.kind},
                     {QStringLiteral("text"), entry.text}};
    if (!entry.data.isEmpty())
        json.insert(QStringLiteral("data"), entry.data);
    return json;
}

QString cutText(QString text, const qsizetype bound, const QString& hint) {
    if (text.size() <= bound)
        return text;
    // The result stays within the bound, so a text bounded when it was written is the same
    // when the document is read again: room for the note, and no surrogate pair cut in half.
    const qsizetype note = 40 + (hint.isEmpty() ? 0 : hint.size() + 2);
    qsizetype keep = std::max<qsizetype>(1, bound - note);
    if (text.at(keep - 1).isHighSurrogate())
        --keep;
    const qsizetype more = text.size() - keep;
    return hint.isEmpty()
               ? QStringLiteral("%1… (%2 more characters)").arg(text.left(keep)).arg(more)
               : QStringLiteral("%1… (%2 more characters; %3)")
                     .arg(text.left(keep))
                     .arg(more)
                     .arg(hint);
}

QString boundedText(QString text) { return cutText(std::move(text), maximumHistoryText); }

QJsonObject boundedData(QJsonObject data) {
    const QStringList keys = data.keys();
    for (const QString& key : keys) {
        QJsonArray list = data.value(key).toArray();
        if (list.size() <= maximumHistoryListItems)
            continue;
        const QString total = key + QStringLiteral("_total");
        if (!data.contains(total))
            data.insert(total, list.size());
        while (list.size() > maximumHistoryListItems)
            list.removeLast();
        data.insert(key, list);
    }
    return data;
}

HistoryEntry bounded(HistoryEntry entry) {
    entry.text = boundedText(std::move(entry.text));
    entry.data = boundedData(std::move(entry.data));
    return entry;
}

QJsonObject summaryJson(const Task& task) {
    QJsonObject json = toJson(task, false);
    if (task.outsideLane.size() > summaryOutsidePaths) {
        json.insert(QStringLiteral("outside_lane"),
                    strings(task.outsideLane.first(summaryOutsidePaths)));
        json.insert(QStringLiteral("outside_lane_total"), task.outsideLane.size());
    }
    return json;
}

QJsonObject taskPageJson(const Task& task, const int limit, const qsizetype before) {
    QJsonObject json = toJson(task, false);
    const qsizetype total = task.history.size();
    const qsizetype end = before < 0 || before > total ? total : before;
    const qsizetype start = std::max<qsizetype>(0, end - std::clamp(limit, 1, maximumPage));
    QJsonArray history;
    for (qsizetype index = start; index < end; ++index)
        history.append(toJson(task.history.at(index)));
    json.insert(QStringLiteral("history"), history);
    json.insert(QStringLiteral("history_total"), total);
    json.insert(QStringLiteral("history_start"), start);
    return json;
}

QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

const Member* Team::member(const QString& sessionId) const {
    for (const Member& entry : members)
        if (entry.sessionId == sessionId)
            return &entry;
    return nullptr;
}

Member* Team::member(const QString& sessionId) {
    for (Member& entry : members)
        if (entry.sessionId == sessionId)
            return &entry;
    return nullptr;
}

const Member* Team::memberWithRole(const QString& wanted) const {
    for (const Member& entry : members)
        if (entry.role == wanted)
            return &entry;
    return nullptr;
}

int Team::count(const QString& wanted) const {
    int total = 0;
    for (const Member& entry : members)
        total += entry.role == wanted ? 1 : 0;
    return total;
}

const Task* Team::task(const QString& wanted) const {
    for (const Task& entry : tasks)
        if (entry.id == wanted)
            return &entry;
    return nullptr;
}

Task* Team::task(const QString& wanted) {
    for (Task& entry : tasks)
        if (entry.id == wanted)
            return &entry;
    return nullptr;
}

Decision* Team::decision(const QString& wanted) {
    for (Decision& entry : decisions)
        if (entry.id == wanted)
            return &entry;
    return nullptr;
}

const Decision* Team::decision(const QString& wanted) const {
    for (const Decision& entry : decisions)
        if (entry.id == wanted)
            return &entry;
    return nullptr;
}

int Team::openDecisions() const {
    int total = 0;
    for (const Decision& entry : decisions)
        total += entry.state == QStringLiteral("open") ? 1 : 0;
    return total;
}

QStringList Team::memberSessions() const {
    QStringList sessions;
    for (const Member& entry : members)
        sessions.append(entry.sessionId);
    return sessions;
}

ReviewSlot* Team::reviewSlot(const QString& reviewer) {
    for (ReviewSlot& slot : reviews)
        if (slot.reviewer == reviewer)
            return &slot;
    return nullptr;
}

const ReviewSlot* Team::reviewSlot(const QString& reviewer) const {
    for (const ReviewSlot& slot : reviews)
        if (slot.reviewer == reviewer)
            return &slot;
    return nullptr;
}

const ReviewSlot* Team::reviewSlotOf(const QString& taskId) const {
    for (const ReviewSlot& slot : reviews)
        if (slot.reviewing == taskId || slot.queue.contains(taskId))
            return &slot;
    return nullptr;
}

bool Team::reviewInFlight(const QString& taskId) const {
    for (const ReviewSlot& slot : reviews)
        if (slot.reviewing == taskId)
            return true;
    return false;
}

bool Team::reviewPending(const QString& taskId) const {
    return reviewQueue.contains(taskId) || reviewSlotOf(taskId);
}

bool Team::reviewsWaiting() const {
    for (const ReviewSlot& slot : reviews)
        if (!slot.reviewing.isEmpty() || !slot.queue.isEmpty())
            return true;
    return false;
}

QJsonObject toJson(const Member& member) {
    QJsonObject json{{QStringLiteral("session_id"), member.sessionId},
                     {QStringLiteral("role"), member.role},
                     {QStringLiteral("name"), member.name},
                     {QStringLiteral("joined_at"), member.joinedAt}};
    if (member.role == role::developer) {
        json.insert(QStringLiteral("lane"), member.lane);
        json.insert(QStringLiteral("paths"), strings(member.paths));
    }
    return json;
}

QJsonObject toJson(const Task& task, const bool withHistory) {
    QJsonArray criteria;
    int met = 0;
    for (const Criterion& criterion : task.criteria) {
        criteria.append(QJsonObject{{QStringLiteral("text"), criterion.text},
                                    {QStringLiteral("met"), criterion.met}});
        met += criterion.met ? 1 : 0;
    }
    QJsonObject json{{QStringLiteral("id"), task.id},
                     {QStringLiteral("title"), task.title},
                     {QStringLiteral("description"), task.description},
                     {QStringLiteral("lane"), task.lane},
                     {QStringLiteral("depends_on"), strings(task.dependsOn)},
                     {QStringLiteral("criteria"), criteria},
                     {QStringLiteral("criteria_met"), met},
                     {QStringLiteral("owner"), task.owner},
                     {QStringLiteral("state"), task.state},
                     {QStringLiteral("resume_state"), task.resumeState},
                     {QStringLiteral("question"), task.question},
                     {QStringLiteral("review_block"), task.reviewBlock},
                     {QStringLiteral("branch"), task.branch},
                     {QStringLiteral("worktree"), task.worktree},
                     {QStringLiteral("base_commit"), task.baseCommit},
                     {QStringLiteral("head_commit"), task.headCommit},
                     {QStringLiteral("pull_request"), task.pullRequest},
                     {QStringLiteral("round"), task.round},
                     {QStringLiteral("reviewer"), task.reviewer},
                     {QStringLiteral("missing_verdicts"), task.missingVerdicts},
                     {QStringLiteral("round_limit"), task.roundLimit},
                     {QStringLiteral("review_place"), task.reviewPlace},
                     {QStringLiteral("review_comment"), task.reviewComment},
                     {QStringLiteral("outside_lane"), strings(task.outsideLane)},
                     {QStringLiteral("assignment_held"), task.assignmentHeld},
                     {QStringLiteral("leader_may_merge"), task.leaderMayMerge},
                     {QStringLiteral("merge_offered"), task.mergeOffered},
                     {QStringLiteral("changed_revision"), task.changedRevision}};
    if (!task.allowedPaths.isEmpty())
        json.insert(QStringLiteral("allowed_paths"), strings(task.allowedPaths));
    // T-90: sent only when the task does not work in the team's repository.
    if (!task.repository.isEmpty()) {
        json.insert(QStringLiteral("repository"), task.repository);
        json.insert(QStringLiteral("integration_branch"), task.integrationBranch);
    }
    // T-93: sent only for a task named the new way.
    if (!task.branchSlug.isEmpty()) {
        json.insert(QStringLiteral("branch_type"), task.branchType);
        json.insert(QStringLiteral("branch_slug"), task.branchSlug);
    }
    if (withHistory) {
        QJsonArray history;
        for (const HistoryEntry& entry : task.history)
            history.append(toJson(entry));
        json.insert(QStringLiteral("history"), history);
    }
    return json;
}

QJsonObject toJson(const Decision& decision) {
    return {{QStringLiteral("id"), decision.id},
            {QStringLiteral("kind"), decision.kind},
            {QStringLiteral("question"), decision.question},
            {QStringLiteral("context"), decision.context},
            {QStringLiteral("choices"), strings(decision.choices)},
            {QStringLiteral("recommended"), decision.recommended},
            {QStringLiteral("task_id"), decision.taskId},
            {QStringLiteral("raised_by"), decision.raisedBy},
            {QStringLiteral("raised_at"), decision.raisedAt},
            {QStringLiteral("state"), decision.state},
            {QStringLiteral("answer"), decision.answer},
            {QStringLiteral("answered_at"), decision.answeredAt},
            {QStringLiteral("reason"), decision.reason},
            {QStringLiteral("changed_revision"), decision.changedRevision}};
}

QJsonObject toJson(const Team& team) {
    QJsonArray members;
    for (const Member& member : team.members)
        members.append(toJson(member));
    QJsonArray tasks;
    int done = 0;
    for (const Task& task : team.tasks) {
        tasks.append(toJson(task));
        done += task.state == state::done ? 1 : 0;
    }
    QJsonArray decisions;
    for (const Decision& decision : team.decisions)
        decisions.append(toJson(decision));
    QJsonArray relocations;
    for (const Relocation& move : team.relocations)
        relocations.append(QJsonObject{{QStringLiteral("session"), move.session},
                                       {QStringLiteral("from"), move.from}});
    QJsonArray reviews;
    QStringList waiting = team.reviewQueue;
    const ReviewSlot* first = nullptr;
    for (const ReviewSlot& slot : team.reviews) {
        reviews.append(QJsonObject{{QStringLiteral("reviewer"), slot.reviewer},
                                   {QStringLiteral("reviewing"), slot.reviewing},
                                   {QStringLiteral("reviewing_message"), slot.message},
                                   {QStringLiteral("queue"), strings(slot.queue)}});
        waiting.append(slot.queue);
        if (!first && !slot.reviewing.isEmpty())
            first = &slot;
    }
    QJsonObject json{
        {QStringLiteral("id"), QString::number(team.id)},
        {QStringLiteral("repository"), team.repository},
        {QStringLiteral("name"), team.name},
        {QStringLiteral("goal"), team.goal},
        {QStringLiteral("integration_branch"), team.integrationBranch},
        {QStringLiteral("merge_policy"), team.mergePolicy},
        {QStringLiteral("execution_policy"), team.executionPolicy},
        {QStringLiteral("language"), team.language},
        {QStringLiteral("leader_writes_requirements"), team.leaderWritesRequirements},
        {QStringLiteral("paused"), team.paused},
        {QStringLiteral("paused_reason"), team.pausedReason},
        {QStringLiteral("shared_paths"), strings(team.sharedPaths)},
        {QStringLiteral("max_rounds"), team.maxRounds},
        {QStringLiteral("review_place"), team.reviewPlace},
        {QStringLiteral("turn_budget"), team.turnBudget},
        {QStringLiteral("automatic_turns"), team.automaticTurns},
        {QStringLiteral("plan_approved"), team.planApproved},
        {QStringLiteral("leader_started"), team.leaderStarted},
        {QStringLiteral("members"), members},
        {QStringLiteral("tasks"), tasks},
        {QStringLiteral("tasks_done"), done},
        {QStringLiteral("decisions"), decisions},
        {QStringLiteral("open_decisions"), team.openDecisions()},
        {QStringLiteral("held"), team.held},
        {QStringLiteral("outbox"), team.outbox},
        {QStringLiteral("reviews"), reviews},
        {QStringLiteral("review_waiting"), strings(team.reviewQueue)},
        // Before T-23 a team had one reviewer: these say what that one did, for a reader
        // that knows no "reviews" — the first review in flight and every task waiting.
        {QStringLiteral("review_queue"), strings(waiting)},
        {QStringLiteral("reviewing"), first ? first->reviewing : QString{}},
        {QStringLiteral("reviewing_message"), first ? first->message : QString{}},
        {QStringLiteral("relocations"), relocations},
        {QStringLiteral("next_task"), team.nextTask},
        {QStringLiteral("next_decision"), team.nextDecision},
        {QStringLiteral("revision"), team.revision},
        {QStringLiteral("settings_revision"), team.settingsRevision},
        {QStringLiteral("members_revision"), team.membersRevision},
        {QStringLiteral("created_at"), team.createdAt},
        {QStringLiteral("changed_at"), team.changedAt}};
    if (team.startWaiting) {
        json.insert(QStringLiteral("start_waiting"), true);
        // `scheduled` means a start time is still set. After cancel the team waits for
        // team.start, with no start_at, so a client can tell the two apart.
        json.insert(QStringLiteral("status"), team.startAt.isEmpty() ? QStringLiteral("waiting")
                                                                     : QStringLiteral("scheduled"));
        if (!team.startAt.isEmpty())
            json.insert(QStringLiteral("start_at"),
                        QJsonObject{{QStringLiteral("at"), team.startAt},
                                    {QStringLiteral("timezone"), team.startTimeZone}});
        if (!team.startScheduleId.isEmpty())
            json.insert(QStringLiteral("start_schedule_id"), team.startScheduleId);
    }
    return json;
}

Team teamFromJson(const QJsonObject& json) {
    Team team;
    team.id = json.value(QStringLiteral("id")).toString().toLongLong();
    team.repository = json.value(QStringLiteral("repository")).toString();
    team.name = json.value(QStringLiteral("name")).toString();
    team.goal = json.value(QStringLiteral("goal")).toString();
    team.integrationBranch = json.value(QStringLiteral("integration_branch")).toString();
    team.mergePolicy = json.value(QStringLiteral("merge_policy")).toString(role::leader);
    team.executionPolicy =
        json.value(QStringLiteral("execution_policy")).toString(QStringLiteral("ask"));
    // A team saved before T-56 is English.
    team.language = ::ketplus::remote::team_language::orEnglish(
        json.value(QStringLiteral("language")).toString());
    team.leaderWritesRequirements =
        json.value(QStringLiteral("leader_writes_requirements")).toBool(true);
    team.paused = json.value(QStringLiteral("paused")).toBool();
    team.pausedReason = json.value(QStringLiteral("paused_reason")).toString();
    team.sharedPaths = stringList(json.value(QStringLiteral("shared_paths")));
    // A saved value outside one to five is not a team's choice; it reads as the default.
    const int rounds = json.value(QStringLiteral("max_rounds")).toInt(defaultReviewRounds);
    team.maxRounds = rounds >= 1 && rounds <= maximumReviewRounds ? rounds : defaultReviewRounds;
    team.reviewPlace = json.value(QStringLiteral("review_place")).toString() == place::pullRequest
                           ? place::pullRequest
                           : place::board;
    team.turnBudget = json.value(QStringLiteral("turn_budget")).toInt(defaultTurnBudget);
    team.automaticTurns = json.value(QStringLiteral("automatic_turns")).toInt();
    team.planApproved = json.value(QStringLiteral("plan_approved")).toBool();
    team.leaderStarted = json.value(QStringLiteral("leader_started")).toBool();
    team.startWaiting = json.value(QStringLiteral("start_waiting")).toBool();
    team.startScheduleId = json.value(QStringLiteral("start_schedule_id")).toString();
    const QJsonObject startAt = json.value(QStringLiteral("start_at")).toObject();
    team.startAt = startAt.value(QStringLiteral("at")).toString();
    team.startTimeZone = startAt.value(QStringLiteral("timezone")).toString();
    for (const auto& value : json.value(QStringLiteral("members")).toArray())
        team.members.append(memberFromJson(value.toObject()));
    for (const auto& value : json.value(QStringLiteral("tasks")).toArray())
        team.tasks.append(taskFromJson(value.toObject()));
    for (const auto& value : json.value(QStringLiteral("decisions")).toArray())
        team.decisions.append(decisionFromJson(value.toObject()));
    team.held = json.value(QStringLiteral("held")).toArray();
    team.outbox = json.value(QStringLiteral("outbox")).toArray();
    if (json.contains(QStringLiteral("reviews"))) {
        for (const auto& value : json.value(QStringLiteral("reviews")).toArray()) {
            const QJsonObject slot = value.toObject();
            team.reviews.append(
                {.reviewer = slot.value(QStringLiteral("reviewer")).toString(),
                 .reviewing = slot.value(QStringLiteral("reviewing")).toString(),
                 .message = slot.value(QStringLiteral("reviewing_message")).toString(),
                 .queue = stringList(slot.value(QStringLiteral("queue")))});
        }
        team.reviewQueue = stringList(json.value(QStringLiteral("review_waiting")));
    }
    // One slot per reviewer, in the order they joined. A document from before T-23 gives its
    // one review and queue to the team's reviewer, or keeps the queue waiting without one.
    QList<ReviewSlot> ordered;
    for (const Member& member : team.members)
        if (member.role == role::reviewer) {
            const ReviewSlot* stored = team.reviewSlot(member.sessionId);
            ordered.append(stored ? *stored : ReviewSlot{.reviewer = member.sessionId});
        }
    team.reviews = ordered;
    if (!json.contains(QStringLiteral("reviews"))) {
        const QStringList queue = stringList(json.value(QStringLiteral("review_queue")));
        const QString reviewing = json.value(QStringLiteral("reviewing")).toString();
        if (team.reviews.isEmpty()) {
            team.reviewQueue = queue;
            if (!reviewing.isEmpty() && !queue.contains(reviewing))
                team.reviewQueue.prepend(reviewing);
        } else {
            team.reviews.first().queue = queue;
            team.reviews.first().reviewing = reviewing;
            team.reviews.first().message =
                json.value(QStringLiteral("reviewing_message")).toString();
        }
    }
    for (const auto& value : json.value(QStringLiteral("relocations")).toArray())
        team.relocations.append(
            Relocation{.session = value.toObject().value(QStringLiteral("session")).toString(),
                       .from = value.toObject().value(QStringLiteral("from")).toString()});
    team.nextTask = json.value(QStringLiteral("next_task")).toInt(1);
    team.nextDecision = json.value(QStringLiteral("next_decision")).toInt(1);
    team.revision = json.value(QStringLiteral("revision")).toInteger();
    team.settingsRevision = json.value(QStringLiteral("settings_revision")).toInteger();
    team.membersRevision = json.value(QStringLiteral("members_revision")).toInteger();
    team.createdAt = json.value(QStringLiteral("created_at")).toString();
    team.changedAt = json.value(QStringLiteral("changed_at")).toString();
    return team;
}

Team taskScope(const Team& team, const Task& task) {
    Team scope = team;
    if (!task.repository.isEmpty()) {
        scope.repository = task.repository;
        scope.integrationBranch = task.integrationBranch;
    }
    return scope;
}

} // namespace ketplus::daemon::teams
