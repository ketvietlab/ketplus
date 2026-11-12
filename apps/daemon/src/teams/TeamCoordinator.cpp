// ADR 0114: the team store and the membership rules.
#include "teams/TeamCoordinator.h"

#include <QTimer>

#include "storage/LocalJournal.h"

#include <algorithm>

namespace ketplus::daemon::teams {
namespace {

TeamError journalError(const QString& message) {
    return {QStringLiteral("journal_error"),
            message.isEmpty() ? QStringLiteral("The journal refused the change.") : message};
}

TeamError notFound() {
    return {QStringLiteral("team_not_found"), QStringLiteral("The team no longer exists.")};
}

int roleOrder(const QString& role) {
    return role == role::leader ? 0 : role == role::developer ? 1 : 2;
}

// ADR 0116 §4: stamps each part of `team` that differs from `before` with the team's new
// revision, so a phone's change is refused only when the part it acts on moved.
void stampChanges(Team& team, const Team& before) {
    const auto settings = [](const Team& one) {
        return QJsonArray{one.name,   one.goal,         one.integrationBranch, one.mergePolicy,
                          one.paused, one.pausedReason, one.turnBudget};
    };
    team.settingsRevision =
        settings(team) == settings(before) ? before.settingsRevision : team.revision;
    const auto members = [](const Team& one) {
        QJsonArray list;
        for (const Member& member : one.members)
            list.append(toJson(member));
        return list;
    };
    team.membersRevision =
        members(team) == members(before) ? before.membersRevision : team.revision;
    for (Task& task : team.tasks) {
        const Task* old = before.task(task.id);
        task.changedRevision = old ? old->changedRevision : team.revision;
        if (old && toJson(task) != toJson(*old))
            task.changedRevision = team.revision;
    }
    for (Decision& decision : team.decisions) {
        const Decision* old = before.decision(decision.id);
        decision.changedRevision = old ? old->changedRevision : team.revision;
        if (old && toJson(decision) != toJson(*old))
            decision.changedRevision = team.revision;
    }
}

} // namespace

TeamCoordinator::TeamCoordinator(LocalJournal* journal, TeamHostPort* host, QObject* parent)
    : QObject(parent), journal_(journal), host_(host) {}

TeamCoordinator::~TeamCoordinator() = default;

void TeamCoordinator::load() {
    // A journal that could not be read is not a journal without teams: say so, so a client
    // asks again instead of showing none.
    bool read = false;
    const QList<QJsonObject> documents = journal_->teamDocuments(&read);
    loaded_ = read;
    if (!read) {
        qWarning("Unable to read the teams from the journal.");
        return;
    }
    teams_.clear();
    teamBySession_.clear();
    for (const QJsonObject& document : documents) {
        Team team = teamFromJson(document);
        if (team.id <= 0)
            continue;
        for (const Member& member : team.members)
            teamBySession_.insert(member.sessionId, team.id);
        teams_.insert(team.id, std::move(team));
    }
    // A review whose diff was being read when the daemon stopped is still queued: nothing was
    // stored for it. It starts now, not at the team's next change.
    const QList<qint64> ids = teams_.keys();
    for (const qint64 id : ids)
        if (const Team* team = this->team(id);
            team && team->reviewing.isEmpty() && !team->reviewQueue.isEmpty())
            pumpReviews(id);
    // ADR 0118: work merged while the daemon was stopped is reconciled now.
    for (const qint64 id : ids)
        checkPullRequests(id);
    // T-28: what was decided before the daemon stopped and not sent yet is sent now.
    for (const qint64 id : ids)
        if (const Team* team = this->team(id); team && !team->outbox.isEmpty())
            scheduleOutbox();
    // T-32: a handover answered before the daemon stopped moves its previous owner now.
    settleStoredRelocations();
}

QList<const Team*> TeamCoordinator::teams(const QString& repository) const {
    QList<const Team*> result;
    for (auto it = teams_.cbegin(); it != teams_.cend(); ++it)
        if (repository.isEmpty() || it->repository == repository)
            result.append(&it.value());
    std::sort(result.begin(), result.end(),
              [](const Team* left, const Team* right) { return left->id < right->id; });
    return result;
}

const Team* TeamCoordinator::team(const qint64 teamId) const {
    const auto it = teams_.constFind(teamId);
    return it == teams_.cend() ? nullptr : &it.value();
}

const Team* TeamCoordinator::teamOf(const QString& sessionId) const {
    const auto it = teamBySession_.constFind(sessionId);
    return it == teamBySession_.cend() ? nullptr : team(it.value());
}

Team* TeamCoordinator::mutableTeam(const qint64 teamId) {
    const auto it = teams_.find(teamId);
    return it == teams_.end() ? nullptr : &it.value();
}

Team* TeamCoordinator::mutableTeamOf(const QString& sessionId) {
    const auto it = teamBySession_.constFind(sessionId);
    return it == teamBySession_.cend() ? nullptr : mutableTeam(it.value());
}

QJsonObject TeamCoordinator::teamJson(const Team& team) const {
    QJsonObject json = toJson(team);
    // The daemon's own bookkeeping: clients see what was sent as the members' turns.
    json.remove(QStringLiteral("outbox"));
    QJsonArray members;
    QList<Member> ordered = team.members;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Member& left, const Member& right) {
        return roleOrder(left.role) < roleOrder(right.role);
    });
    for (const Member& member : ordered) {
        QJsonObject entry = toJson(member);
        if (const auto conversation = host_->teamConversation(member.sessionId)) {
            entry.insert(QStringLiteral("number"), conversation->number);
            entry.insert(QStringLiteral("session_generation"), conversation->generation);
            entry.insert(QStringLiteral("title"), conversation->title);
            entry.insert(QStringLiteral("provider"), conversation->provider);
            entry.insert(QStringLiteral("model"), conversation->model);
            entry.insert(QStringLiteral("effort"), conversation->effort);
            entry.insert(QStringLiteral("idle"), conversation->idle);
        }
        QString current;
        for (const Task& task : team.tasks)
            if (task.owner == member.sessionId && task.state == state::inProgress)
                current = task.id;
        entry.insert(QStringLiteral("current_task"), current);
        members.append(entry);
    }
    json.insert(QStringLiteral("members"), members);
    json.remove(QStringLiteral("next_task"));
    json.remove(QStringLiteral("next_decision"));
    // ADR 0115: a summary. History, held messages and old decisions grow with time; clients
    // read them with team.task.get and team.decision.list.
    QJsonArray tasks;
    for (const Task& task : team.tasks)
        tasks.append(summaryJson(task));
    json.insert(QStringLiteral("tasks"), tasks);
    json.remove(QStringLiteral("held"));
    json.insert(QStringLiteral("held_count"), team.held.size());
    QJsonArray decisions;
    qsizetype answered = 0;
    for (auto it = team.decisions.crbegin(); it != team.decisions.crend(); ++it) {
        const bool open = it->state == QStringLiteral("open");
        if (!open && answered >= summaryAnsweredDecisions)
            continue;
        answered += open ? 0 : 1;
        decisions.prepend(toJson(*it));
    }
    json.insert(QStringLiteral("decisions"), decisions);
    json.insert(QStringLiteral("decisions_total"), team.decisions.size());
    return json;
}

QJsonObject TeamCoordinator::directoryJson(const Team& team) const {
    QJsonObject json = teamJson(team);
    json.remove(QStringLiteral("tasks"));
    json.remove(QStringLiteral("decisions"));
    json.insert(QStringLiteral("tasks_total"), team.tasks.size());
    json.insert(QStringLiteral("directory"), true);
    return json;
}

QList<TeamUpdateRecord> TeamCoordinator::updateRecords(const qint64 teamId,
                                                       const bool undigestedOnly, const int limit,
                                                       const qint64 beforeId) const {
    // Updates written before the bounds can hold thousands of paths.
    QList<TeamUpdateRecord> updates =
        journal_->teamUpdates(teamId, undigestedOnly, limit, beforeId);
    for (TeamUpdateRecord& update : updates) {
        update.text = boundedText(std::move(update.text));
        update.data = boundedData(std::move(update.data));
    }
    return updates;
}

QJsonArray TeamCoordinator::updatesJson(const qint64 teamId, const int limit,
                                        const qint64 beforeId) const {
    QJsonArray updates;
    for (const TeamUpdateRecord& update :
         updateRecords(teamId, false, std::clamp(limit, 1, maximumPage), beforeId))
        updates.append(QJsonObject{{QStringLiteral("id"), update.id},
                                   {QStringLiteral("at"), update.at},
                                   {QStringLiteral("actor"), update.actor},
                                   {QStringLiteral("kind"), update.kind},
                                   {QStringLiteral("task_id"), update.taskId},
                                   {QStringLiteral("text"), update.text},
                                   {QStringLiteral("data"), update.data},
                                   {QStringLiteral("actionable"), update.actionable}});
    return updates;
}

std::optional<Candidate> TeamCoordinator::candidate(const QString& sessionId) const {
    const auto conversation = host_->teamConversation(sessionId);
    if (!conversation)
        return std::nullopt;
    const Team* current = teamOf(sessionId);
    return Candidate{.sessionId = sessionId,
                     .repository = host_->conversationRepository(sessionId),
                     .provider = conversation->provider,
                     .teamId = current ? current->id : 0};
}

std::optional<TeamError> TeamCoordinator::commit(Team& team, const Team& before,
                                                 const QString& actor,
                                                 const QList<Update>& updates) {
    team.revision = before.revision + 1;
    team.changedAt = nowIso();
    stampChanges(team, before);
    QString error;
    if (!journal_->saveTeam(team.id, before.revision, toJson(team), team.memberSessions(),
                            &error)) {
        team = before;
        return journalError(error);
    }
    for (const QString& session : before.memberSessions())
        teamBySession_.remove(session);
    for (const QString& session : team.memberSessions())
        teamBySession_.insert(session, team.id);
    bool actionable = false;
    for (const Update& update : updates) {
        actionable = actionable || update.actionable;
        (void)journal_->appendTeamUpdate({.teamId = team.id,
                                          .at = team.changedAt,
                                          .actor = actor,
                                          .kind = update.kind,
                                          .taskId = update.taskId,
                                          .text = boundedText(update.text),
                                          .data = boundedData(update.data),
                                          .actionable = update.actionable});
    }
    emit teamChanged(directoryJson(team));
    if (actionable)
        scheduleDigest(team.id);
    if (team.reviewing.isEmpty() && !team.reviewQueue.isEmpty())
        pumpReviews(team.id);
    return std::nullopt;
}

void TeamCoordinator::addHistory(Task& task, const QString& actor, const QString& kind,
                                 const QString& text, const QJsonObject& data) {
    task.history.append(
        bounded({.at = nowIso(), .actor = actor, .kind = kind, .text = text, .data = data}));
}

Decision& TeamCoordinator::raiseDecision(Team& team, const QString& kind, const QString& question,
                                         const QString& context, const QStringList& choices,
                                         const QString& recommended, const QString& taskId,
                                         const QString& raisedBy) {
    Decision decision;
    decision.id = QStringLiteral("D-%1").arg(team.nextDecision++);
    decision.kind = kind;
    decision.question = question;
    decision.context = context;
    decision.choices = choices;
    decision.recommended = recommended;
    decision.taskId = taskId;
    decision.raisedBy = raisedBy;
    decision.raisedAt = nowIso();
    team.decisions.append(decision);
    return team.decisions.last();
}

TeamResult TeamCoordinator::create(Team draft, const QString& actor) {
    QList<Candidate> candidates;
    for (Member& member : draft.members) {
        member.name = member.name.trimmed();
        if (member.role == role::developer) {
            if (const auto paths = rules::normalisedPaths(member.paths))
                member.paths = *paths;
        } else {
            member.lane.clear();
            member.paths.clear();
        }
        if (const auto found = candidate(member.sessionId))
            candidates.append(*found);
    }
    draft.name = draft.name.trimmed();
    draft.goal = draft.goal.trimmed();
    if (auto refused = rules::validGoal(draft.goal))
        return {.error = refused};
    if (const auto paths = rules::normalisedPaths(draft.sharedPaths))
        draft.sharedPaths = *paths;
    else
        return {.error = TeamError{QStringLiteral("invalid_payload"),
                                   QStringLiteral("Shared paths are relative to the repository.")}};
    if (auto refused = rules::canCreate(draft, candidates))
        return {.error = refused};
    const QString now = nowIso();
    draft.createdAt = now;
    draft.changedAt = now;
    for (Member& member : draft.members)
        member.joinedAt = now;
    draft.tasks.clear();
    draft.decisions.clear();
    draft.held = {};
    draft.automaticTurns = 0;
    draft.nextTask = 1;
    draft.nextDecision = 1;
    draft.settingsRevision = 1;
    draft.membersRevision = 1;
    QString error;
    const auto id =
        journal_->insertTeam(draft.repository, toJson(draft), draft.memberSessions(), &error);
    if (!id)
        return {.error = journalError(error)};
    draft.id = *id;
    draft.revision = 1;
    for (const Member& member : draft.members)
        teamBySession_.insert(member.sessionId, draft.id);
    teams_.insert(draft.id, draft);
    (void)journal_->appendTeamUpdate({.teamId = draft.id,
                                      .at = now,
                                      .actor = actor,
                                      .kind = QStringLiteral("team_created"),
                                      .text = QStringLiteral("Team %1 created with %2 members.")
                                                  .arg(draft.name)
                                                  .arg(draft.members.size())});
    emit teamChanged(directoryJson(draft));
    startLeader(draft.id);
    return {.teamId = draft.id};
}

TeamResult TeamCoordinator::update(const qint64 teamId, const QJsonObject& changes,
                                   const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    QList<Update> updates;
    if (changes.contains(QStringLiteral("name"))) {
        const QString name = changes.value(QStringLiteral("name")).toString().trimmed();
        if (auto invalid = rules::validName(name, QStringLiteral("The team")))
            return {.error = invalid};
        team->name = name;
    }
    if (changes.contains(QStringLiteral("goal"))) {
        const QString goal = changes.value(QStringLiteral("goal")).toString().trimmed();
        if (auto invalid = rules::validGoal(goal)) {
            *team = before;
            return {.error = invalid};
        }
        team->goal = goal;
    }
    if (changes.contains(QStringLiteral("integration_branch"))) {
        const QString branch =
            changes.value(QStringLiteral("integration_branch")).toString().trimmed();
        if (auto invalid = rules::validBranch(branch)) {
            *team = before;
            return {.error = invalid};
        }
        // Lanes and merges are checked against it: a team that has one changes it, not drops it.
        if (branch.isEmpty() && !team->integrationBranch.isEmpty()) {
            *team = before;
            return {.error = TeamError{QStringLiteral("invalid_payload"),
                                       QStringLiteral("The integration branch can be changed, "
                                                      "not removed.")}};
        }
        team->integrationBranch = branch;
    }
    if (changes.contains(QStringLiteral("merge_policy"))) {
        const QString policy = changes.value(QStringLiteral("merge_policy")).toString();
        if (policy != role::leader && policy != QStringLiteral("user")) {
            *team = before;
            return {.error =
                        TeamError{QStringLiteral("invalid_payload"),
                                  QStringLiteral("Who merges tasks is \"leader\" or \"user\".")}};
        }
        if (policy != team->mergePolicy)
            updates.append({.kind = QStringLiteral("merge_policy"),
                            .text = policy == role::leader
                                        ? QStringLiteral("The leader merges reviewed tasks.")
                                        : QStringLiteral("The user merges reviewed tasks."),
                            .data = {{QStringLiteral("from"), team->mergePolicy},
                                     {QStringLiteral("to"), policy}}});
        team->mergePolicy = policy;
    }
    if (changes.contains(QStringLiteral("leader_writes_requirements")))
        team->leaderWritesRequirements =
            changes.value(QStringLiteral("leader_writes_requirements")).toBool(true);
    if (changes.contains(QStringLiteral("max_rounds"))) {
        const int rounds = changes.value(QStringLiteral("max_rounds")).toInt();
        if (rounds < 1 || rounds > 3) {
            *team = before;
            return {.error = TeamError{QStringLiteral("invalid_payload"),
                                       QStringLiteral("Review rounds are 1 to 3.")}};
        }
        team->maxRounds = rounds;
    }
    if (changes.contains(QStringLiteral("shared_paths"))) {
        QStringList paths;
        for (const auto& value : changes.value(QStringLiteral("shared_paths")).toArray())
            paths.append(value.toString());
        const auto normalised = rules::normalisedPaths(paths);
        if (!normalised) {
            *team = before;
            return {.error =
                        TeamError{QStringLiteral("invalid_payload"),
                                  QStringLiteral("Shared paths are relative to the repository.")}};
        }
        if (*normalised != team->sharedPaths)
            updates.append(
                {.kind = QStringLiteral("shared_paths"),
                 .text = QStringLiteral("Shared paths: %1.")
                             .arg(normalised->isEmpty() ? QStringLiteral("none")
                                                        : normalised->join(QStringLiteral(", ")))});
        team->sharedPaths = *normalised;
        recheckLanes(*team, updates);
    }
    if (changes.contains(QStringLiteral("turn_budget"))) {
        if (auto invalid =
                changeTurnBudget(*team, changes.value(QStringLiteral("turn_budget")), updates)) {
            *team = before;
            return {.error = invalid};
        }
    }
    if (changes.contains(QStringLiteral("paused"))) {
        const bool paused = changes.value(QStringLiteral("paused")).toBool();
        if (paused != team->paused) {
            team->paused = paused;
            team->pausedReason = paused ? QStringLiteral("user") : QString{};
            if (!paused) {
                team->automaticTurns = 0; // The user acted on the team (ADR 0114, point 2).
                for (Decision& decision : team->decisions)
                    if (decision.kind == QStringLiteral("budget") &&
                        decision.state == QStringLiteral("open")) {
                        decision.state = QStringLiteral("withdrawn");
                        decision.reason = QStringLiteral("The team was resumed.");
                        decision.answeredAt = nowIso();
                    }
            }
            updates.append({.kind = paused ? QStringLiteral("paused") : QStringLiteral("resumed"),
                            .text = paused ? QStringLiteral("Automatic turns paused.")
                                           : QStringLiteral("Automatic turns resumed.")});
        }
    }
    const bool resumed = before.paused && !team->paused;
    if (auto failed = commit(*team, before, actor, updates))
        return {.error = failed};
    if (resumed) {
        releaseHeld(teamId);
        scheduleDigest(teamId);
    }
    startLeader(teamId);
    return {.teamId = teamId};
}

void TeamCoordinator::startLeader(const qint64 teamId) {
    Team* team = mutableTeam(teamId);
    if (!team || team->leaderStarted || team->goal.isEmpty())
        return;
    const Member* leader = team->memberWithRole(role::leader);
    if (!leader)
        return;
    const Team before = *team;
    team->leaderStarted = true;
    if (commit(*team, before, QStringLiteral("daemon"), {}))
        return;
    QStringList members;
    for (const Member& member : team->members) {
        const auto conversation = host_->teamConversation(member.sessionId);
        const QString number = conversation ? conversation->number : QString{};
        members << QStringLiteral("- #%1 %2, %3%4")
                       .arg(number, member.name, member.role,
                            member.role == role::developer
                                ? QStringLiteral(", lane %1 (%2)")
                                      .arg(member.lane,
                                           member.paths.isEmpty()
                                               ? QStringLiteral("no paths")
                                               : member.paths.join(QStringLiteral(", ")))
                                : QString{});
    }
    QStringList body{QStringLiteral("Team %1 is ready and you lead it.").arg(team->name),
                     QStringLiteral("Goal: %1").arg(team->goal),
                     QStringLiteral("Integration branch: %1")
                         .arg(team->integrationBranch.isEmpty() ? QStringLiteral("HEAD")
                                                                : team->integrationBranch),
                     QStringLiteral("Members:"), members.join(QLatin1Char('\n'))};
    // ADR 0114: a mention in the goal points at context; nobody was messaged for it.
    if (const QStringList numbers = rules::goalMentions(team->goal); !numbers.isEmpty()) {
        // Bounded, so the turn fits in one message beside the goal (rules::maximumGoalBytes).
        QStringList mentioned;
        for (const QString& number :
             numbers.first(std::min<qsizetype>(numbers.size(), rules::maximumListedMentions))) {
            const auto conversation = host_->conversationByNumber(number);
            QString title = conversation ? conversation->title : QString{};
            if (title.size() > rules::maximumMentionTitleCharacters)
                title = title.left(rules::maximumMentionTitleCharacters - 1) + QChar(0x2026);
            mentioned << (conversation ? QStringLiteral("- #%1 %2").arg(number, title)
                                       : QStringLiteral("- #%1 (no conversation with this number "
                                                        "on this computer)")
                                             .arg(number));
        }
        if (numbers.size() > rules::maximumListedMentions)
            mentioned << QStringLiteral("- and %1 more; their numbers are in the goal.")
                             .arg(numbers.size() - rules::maximumListedMentions);
        body << QString{} << QStringLiteral("Conversations the goal mentions:")
             << mentioned.join(QLatin1Char('\n'))
             << QStringLiteral("You may ask them for context through agent messaging "
                               "(agents_send). Creating the team sent them nothing.");
    }
    body << QString{}
         << QStringLiteral("Plan the work: create the tasks with their criteria and lanes "
                           "(team_task_create), then assign them (team_task_assign).");
    wake(*team, {.teamId = team->id,
                 .teamName = team->name,
                 .toSession = leader->sessionId,
                 .kind = QStringLiteral("team"),
                 .body = body.join(QLatin1Char('\n'))});
}

std::optional<TeamError> TeamCoordinator::remove(const qint64 teamId) {
    const Team* team = this->team(teamId);
    if (!team)
        return notFound();
    QString error;
    if (!journal_->deleteTeam(teamId, &error))
        return journalError(error);
    const QString repository = team->repository;
    for (const QString& session : team->memberSessions()) {
        cancelWait(session);
        teamBySession_.remove(session);
    }
    teams_.remove(teamId);
    delete digestTimers_.take(teamId);
    delete stallTimers_.take(teamId);
    stalls_.remove(teamId);
    emit teamRemoved(QString::number(teamId), repository);
    return std::nullopt;
}

TeamResult TeamCoordinator::addMember(const qint64 teamId, Member member, const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    const auto found = candidate(member.sessionId);
    if (!found)
        return {.error = TeamError{QStringLiteral("invalid_payload"),
                                   QStringLiteral("The conversation no longer exists.")}};
    member.name = member.name.trimmed();
    if (member.role == role::developer) {
        if (const auto paths = rules::normalisedPaths(member.paths))
            member.paths = *paths;
    }
    if (auto refused = rules::canAddMember(*team, member, *found))
        return {.error = refused};
    member.joinedAt = nowIso();
    team->members.append(member);
    // An empty role filled again lifts the pause it caused (ADR 0114, point 4).
    if (team->paused && team->pausedReason == QStringLiteral("empty_role") &&
        team->count(role::leader) == 1 && team->count(role::reviewer) == 1 &&
        team->count(role::developer) >= 1) {
        team->paused = false;
        team->pausedReason.clear();
        for (Decision& decision : team->decisions)
            if (decision.kind == QStringLiteral("empty_role") &&
                decision.state == QStringLiteral("open")) {
                decision.state = QStringLiteral("withdrawn");
                decision.reason = QStringLiteral("The role was filled.");
            }
    }
    if (auto failed =
            commit(*team, before, actor,
                   {{.kind = QStringLiteral("member_added"),
                     .text = QStringLiteral("%1 joined as %2.").arg(member.name, member.role),
                     .data = {{QStringLiteral("session_id"), member.sessionId}}}}))
        return {.error = failed};
    return {.teamId = teamId};
}

TeamResult TeamCoordinator::updateMember(const qint64 teamId, const QString& sessionId,
                                         const QString& lane, const QStringList& paths,
                                         const QString& actor, const QString& reason) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    Member* member = team->member(sessionId);
    if (!member || member->role != role::developer)
        return {.error = TeamError{QStringLiteral("not_member"),
                                   QStringLiteral("Only a developer of the team has a lane.")}};
    const auto normalised = rules::normalisedPaths(paths);
    if (!normalised)
        return {.error = TeamError{
                    QStringLiteral("invalid_payload"),
                    QStringLiteral("A lane's paths are relative to the repository root.")}};
    if (auto overlap = rules::laneOverlap(*team, sessionId, *normalised))
        return {.error = overlap};
    const QString oldLane = member->lane;
    const QStringList oldPaths = member->paths;
    member->lane = lane.trimmed().isEmpty() ? oldLane : lane.trimmed();
    member->paths = *normalised;
    const QString text =
        QStringLiteral("Lane %1 of %2: %3 → %4.%5")
            .arg(member->lane, member->name,
                 oldPaths.isEmpty() ? QStringLiteral("(none)")
                                    : oldPaths.join(QStringLiteral(", ")),
                 normalised->isEmpty() ? QStringLiteral("(none)")
                                       : normalised->join(QStringLiteral(", ")),
                 reason.isEmpty() ? QString{} : QStringLiteral(" Reason: ") + reason);
    const QJsonObject data{{QStringLiteral("session_id"), sessionId},
                           {QStringLiteral("lane"), member->lane},
                           {QStringLiteral("old_lane"), oldLane},
                           {QStringLiteral("from"), QJsonArray::fromStringList(oldPaths)},
                           {QStringLiteral("to"), QJsonArray::fromStringList(*normalised)},
                           {QStringLiteral("reason"), reason}};
    // ADR 0114 §4: in the history of every open task of the lanes it touches.
    for (Task& task : team->tasks)
        if (task.state != state::done &&
            (task.owner == sessionId || task.lane == oldLane || task.lane == member->lane))
            addHistory(task, actor, QStringLiteral("lane"), text, data);
    QList<Update> updates{{.kind = QStringLiteral("lane_changed"), .text = text, .data = data}};
    recheckLanes(*team, updates);
    if (auto failed = commit(*team, before, actor, updates))
        return {.error = failed};
    return {.teamId = teamId};
}

void TeamCoordinator::unassignTasksOf(Team& team, const QString& sessionId, const QString& actor,
                                      const QString& reason, QList<Update>& updates) {
    team.relocations.removeIf(
        [&sessionId](const Relocation& move) { return move.session == sessionId; });
    for (Task& task : team.tasks) {
        if (task.owner != sessionId || task.state == state::done)
            continue;
        task.owner.clear();
        task.assignmentHeld = false;
        if (task.state != state::todo) {
            addHistory(task, actor, QStringLiteral("state"),
                       QStringLiteral("%1 → todo: %2").arg(task.state, reason),
                       {{QStringLiteral("from"), task.state}, {QStringLiteral("to"), state::todo}});
            task.state = state::todo;
        }
        addHistory(task, actor, QStringLiteral("owner"),
                   QStringLiteral("Unassigned: %1").arg(reason),
                   {{QStringLiteral("from"), sessionId}, {QStringLiteral("to"), QString{}}});
        updates.append({.kind = QStringLiteral("task_unassigned"),
                        .text = QStringLiteral("%1 is unassigned: %2").arg(task.id, reason),
                        .taskId = task.id,
                        .actionable = true});
    }
}

TeamResult TeamCoordinator::removeMember(const qint64 teamId, const QString& sessionId,
                                         const QString& actor) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = notFound()};
    // Before any pointer into the team: the copy shares its lists until they detach.
    const Team before = *team;
    if (auto refused = rules::canRemoveMember(*team, sessionId))
        return {.error = refused};
    const QString name = team->member(sessionId)->name;
    QList<Update> updates;
    cancelWait(sessionId);
    unassignTasksOf(*team, sessionId, actor, QStringLiteral("%1 left the team").arg(name), updates);
    team->members.removeIf([&](const Member& member) { return member.sessionId == sessionId; });
    updates.prepend({.kind = QStringLiteral("member_removed"),
                     .text = QStringLiteral("%1 left the team.").arg(name),
                     .data = {{QStringLiteral("session_id"), sessionId}}});
    if (auto failed = commit(*team, before, actor, updates))
        return {.error = failed};
    return {.teamId = teamId};
}

void TeamCoordinator::conversationDeleted(const QString& sessionId) {
    Team* team = mutableTeamOf(sessionId);
    if (!team)
        return;
    const Team before = *team;
    const Member* member = team->member(sessionId);
    const QString role = member->role;
    const QString name = member->name;
    cancelWait(sessionId);
    QList<Update> updates;
    unassignTasksOf(*team, sessionId, QStringLiteral("daemon"),
                    QStringLiteral("%1 was deleted").arg(name), updates);
    team->members.removeIf([&](const Member& entry) { return entry.sessionId == sessionId; });
    updates.prepend({.kind = QStringLiteral("member_removed"),
                     .text = QStringLiteral("%1 was deleted and left the team.").arg(name),
                     .data = {{QStringLiteral("session_id"), sessionId}}});
    if (role == role::reviewer && !team->reviewing.isEmpty()) {
        // The review in flight waits for the next reviewer, first in the queue.
        team->reviewQueue.prepend(team->reviewing);
        team->reviewing.clear();
        team->reviewingMessage.clear();
    }
    if (role != role::developer || team->count(role::developer) == 0) {
        const QString empty = role == role::developer ? QStringLiteral("developer") : role;
        team->paused = true;
        team->pausedReason = QStringLiteral("empty_role");
        raiseDecision(*team, QStringLiteral("empty_role"),
                      QStringLiteral("The team has no %1. Add one or delete the team?").arg(empty),
                      QStringLiteral("%1 was deleted. The team is paused until the role is filled.")
                          .arg(name),
                      {QStringLiteral("Add a %1").arg(empty), QStringLiteral("Delete the team")},
                      QStringLiteral("Add a %1").arg(empty), {}, QStringLiteral("daemon"));
        updates.append({.kind = QStringLiteral("paused"),
                        .text = QStringLiteral("Paused: the team has no %1.").arg(empty),
                        .actionable = false});
    }
    if (commit(*team, before, QStringLiteral("daemon"), updates))
        qWarning("Unable to record that %s left its team.", qUtf8Printable(sessionId));
}

std::optional<TeamError> TeamCoordinator::archiveRefusal(const QString& sessionId) const {
    const Team* team = teamOf(sessionId);
    if (!team)
        return std::nullopt;
    // A developer can be removed; the leader and the reviewer stay as long as the team does.
    const Member* member = team->member(sessionId);
    const QString role = member ? member->role : QString{};
    if (role == role::leader)
        return TeamError{QStringLiteral("team_member"),
                         QStringLiteral("This conversation leads team %1, and a team keeps its "
                                        "leader. Delete the team before archiving it.")
                             .arg(team->name)};
    if (role == role::reviewer)
        return TeamError{QStringLiteral("team_member"),
                         QStringLiteral("This conversation reviews for team %1, and a team keeps "
                                        "its reviewer. Give the team another reviewer, or delete "
                                        "it, before archiving this conversation.")
                             .arg(team->name)};
    return TeamError{QStringLiteral("team_member"),
                     QStringLiteral("This conversation is on team %1. Remove it from the team "
                                    "before archiving it.")
                         .arg(team->name)};
}

} // namespace ketplus::daemon::teams
