// ADR 0114: the team.* requests of the teams/1 feature, for trusted local clients.
#include "runtime/SessionHost.h"

#include "protocol/DaemonProtocol.h"

#include <QSet>
#include <QUuid>

#include <algorithm>

namespace ketplus::daemon {
namespace {

QJsonObject failure(const QString& code, const QString& message, const QString& role = {}) {
    QJsonObject json{{QStringLiteral("ok"), false},
                     {QStringLiteral("error_code"), code},
                     {QStringLiteral("error_message"), message}};
    if (!role.isEmpty())
        json.insert(QStringLiteral("role"), role);
    return json;
}

QJsonObject failure(const teams::TeamError& error) { return failure(error.code, error.message); }

bool knownProvider(const QString& provider) {
    return provider == QStringLiteral("codex") || provider == QStringLiteral("claude") ||
           provider == QStringLiteral("cursor");
}

QStringList strings(const QJsonValue& value) {
    QStringList result;
    for (const auto& item : value.toArray())
        result.append(item.toString());
    return result;
}

// One member to start: its role, name, agent and (for a developer) lane.
struct MemberPlan final {
    QString role;
    QString name;
    QString provider;
    QString model;
    QString effort;
    QString lane;
    QStringList paths;
};

MemberPlan planOf(const QString& role, const QJsonObject& agent, const QString& fallbackName) {
    const QString name = agent.value(QStringLiteral("name")).toString().trimmed();
    return {.role = role,
            .name = name.isEmpty() ? fallbackName : name,
            .provider = agent.value(QStringLiteral("provider")).toString(),
            .model = agent.value(QStringLiteral("model")).toString(),
            .effort = agent.value(QStringLiteral("effort")).toString(),
            .lane = agent.value(QStringLiteral("lane")).toString().trimmed(),
            .paths = strings(agent.value(QStringLiteral("paths")))};
}

} // namespace

QString SessionHost::createMemberConversation(const QString& worktree, const QString& provider,
                                              const QString& model, const QString& effort,
                                              const QString& title, QString* problem,
                                              const QString& executionPolicy) {
    const QString directory = normalizedWorktree(worktree);
    if (directory.isEmpty()) {
        *problem = QStringLiteral("The team's repository is not an existing directory.");
        return {};
    }
    Session session;
    session.stored.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.stored.worktreePath = directory;
    session.stored.adapter = provider;
    session.stored.state = QStringLiteral("ready");
    session.stored.generation = 1;
    session.stored.title = title;
    session.stored.model = model;
    session.stored.effort = effort;
    if (!executionPolicy.isEmpty())
        session.stored.executionPolicy = executionPolicy;
    if (!journal_->upsertSession(session.stored)) {
        *problem = journal_->errorString();
        return {};
    }
    auto it = sessions_.insert(session.stored.id, session);
    QString numberError;
    if (journal_->assignConversationNumber(it->stored.id, 1, &numberError).isEmpty())
        qWarning("Unable to number conversation %s: %s", qUtf8Printable(it->stored.id),
                 qUtf8Printable(numberError));
    QString createError;
    if (!appendEvent(*it, QStringLiteral("session.created"),
                     {{QStringLiteral("worktree_path"), directory},
                      {QStringLiteral("adapter"), provider},
                      {QStringLiteral("title"), title}},
                     &createError)) {
        *problem = createError;
        discardMemberConversation(it->stored.id);
        return {};
    }
    return session.stored.id;
}

void SessionHost::discardMemberConversation(const QString& sessionId) {
    const QJsonObject result = deleteSession(
        {{QStringLiteral("protocol_version"), protocol::currentVersion},
         {QStringLiteral("message_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
         {QStringLiteral("message_type"), QStringLiteral("session.delete")},
         {QStringLiteral("sent_at"), protocol::timestamp()},
         {QStringLiteral("session_id"), sessionId},
         {QStringLiteral("session_generation"), 1},
         {QStringLiteral("payload"), QJsonObject{}}});
    if (!result.value(QStringLiteral("ok")).toBool())
        qWarning("Unable to remove the unused member conversation %s: %s",
                 qUtf8Printable(sessionId),
                 qUtf8Printable(result.value(QStringLiteral("error_message")).toString()));
}

QJsonObject SessionHost::teamCommand(const QString& type, const QJsonObject& request) {
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const qint64 teamId = payload.value(QStringLiteral("team_id")).toString().toLongLong();
    const auto answer = [this](const teams::TeamResult& result) {
        if (!result.ok())
            return failure(*result.error);
        const teams::Team* team = teams_->team(result.teamId);
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("team"), team ? teams_->teamJson(*team) : QJsonObject{}}};
    };
    // The teams could not be read when the daemon started: read them now, or say so. No team
    // request acts on teams that are not all there.
    if (!teams_->isLoaded())
        teams_->load();
    if (!teams_->isLoaded())
        return failure(QStringLiteral("teams_unavailable"),
                       QStringLiteral("The teams could not be read. Try again."));
    if (type == QStringLiteral("team.list")) {
        const QString repository = payload.value(QStringLiteral("repository")).toString();
        QJsonArray list;
        for (const teams::Team* team :
             teams_->teams(repository.isEmpty() ? QString{} : repositoryOf(repository)))
            list.append(teams_->directoryJson(*team));
        return {{QStringLiteral("ok"), true}, {QStringLiteral("teams"), list}};
    }
    if (type == QStringLiteral("team.get")) {
        // ADR 0114, open point 3: reading the team asks git whether a task was merged outside
        // the app. The answer comes after this reply, as a `team.changed` event.
        teams_->checkMerged(teamId);
        const teams::Team* team = teams_->team(teamId);
        if (!team)
            return failure(QStringLiteral("team_not_found"),
                           QStringLiteral("The team no longer exists."));
        // ADR 0115: the newest page of Updates; team.updates.list reads older ones.
        const QJsonArray updates = teams_->updatesJson(teamId, teams::defaultPage + 1);
        QJsonArray page;
        for (qsizetype index = 0; index < std::min<qsizetype>(updates.size(), teams::defaultPage);
             ++index)
            page.append(updates.at(index));
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("team"), teams_->teamJson(*team)},
                {QStringLiteral("updates"), page},
                {QStringLiteral("updates_has_more"), updates.size() > teams::defaultPage}};
    }
    if (type == QStringLiteral("team.updates.list")) {
        if (!teams_->team(teamId))
            return failure(QStringLiteral("team_not_found"),
                           QStringLiteral("The team no longer exists."));
        const int limit =
            std::clamp(payload.value(QStringLiteral("limit")).toInt(teams::defaultPage), 1,
                       teams::maximumPage);
        const qint64 before = payload.value(QStringLiteral("before_id")).toInteger();
        QJsonArray updates = teams_->updatesJson(teamId, limit + 1, before);
        const bool more = updates.size() > limit;
        while (updates.size() > limit)
            updates.removeLast();
        return {{QStringLiteral("ok"), true},
                {QStringLiteral("updates"), updates},
                {QStringLiteral("has_more"), more}};
    }
    if (type == QStringLiteral("team.create"))
        return createTeam(payload);
    if (type == QStringLiteral("team.update"))
        return answer(teams_->update(teamId, payload, QStringLiteral("user")));
    if (type == QStringLiteral("team.delete")) {
        if (const auto refused = teams_->remove(teamId))
            return failure(*refused);
        return {{QStringLiteral("ok"), true}, {QStringLiteral("team_id"), QString::number(teamId)}};
    }
    if (type == QStringLiteral("team.member.add")) {
        const teams::Team* team = teams_->team(teamId);
        if (!team)
            return failure(QStringLiteral("team_not_found"),
                           QStringLiteral("The team no longer exists."));
        const MemberPlan plan = planOf(teams::role::developer, payload, {});
        if (const auto invalid = teams::rules::validName(plan.name, QStringLiteral("a developer")))
            return failure(*invalid);
        if (team->count(teams::role::developer) >= teams::maximumDevelopers)
            return failure(QStringLiteral("team_full"),
                           QStringLiteral("The team has five developers."));
        if (!knownProvider(plan.provider))
            return failure(QStringLiteral("provider_unavailable"),
                           QStringLiteral("Choose the developer's agent."), plan.role);
        if (plan.provider != QStringLiteral("codex") && !availableProvider(plan.provider))
            return failure(QStringLiteral("provider_unavailable"),
                           missingProviderMessage(plan.provider), plan.role);
        QString problem;
        const QString sessionId =
            createMemberConversation(team->repository, plan.provider, plan.model, plan.effort,
                                     plan.name, &problem, team->executionPolicy);
        if (sessionId.isEmpty())
            return failure(QStringLiteral("member_unavailable"), problem, plan.role);
        const auto result = teams_->addMember(teamId,
                                              {.sessionId = sessionId,
                                               .role = teams::role::developer,
                                               .name = plan.name,
                                               .lane = plan.lane.isEmpty() ? plan.name : plan.lane,
                                               .paths = plan.paths},
                                              QStringLiteral("user"));
        if (!result.ok())
            discardMemberConversation(sessionId);
        return answer(result);
    }
    if (type == QStringLiteral("team.member.join")) {
        const QString sessionId = payload.value(QStringLiteral("session_id")).toString();
        const auto it = sessions_.constFind(sessionId);
        if (it == sessions_.cend())
            return failure(QStringLiteral("session_not_found"),
                           QStringLiteral("The conversation no longer exists."));
        if (!it->stored.archivedAt.isEmpty())
            return failure(QStringLiteral("invalid_payload"),
                           QStringLiteral("Unarchive the conversation before it joins a team."));
        if (journal_->isConversationReviewer(sessionId))
            return failure(QStringLiteral("invalid_payload"),
                           QStringLiteral("This conversation reviews another conversation. Choose "
                                          "another reviewer there first."));
        const QString lane = payload.value(QStringLiteral("lane")).toString().trimmed();
        return answer(teams_->addMember(
            teamId,
            {.sessionId = sessionId,
             .role = teams::role::developer,
             .name = it->stored.title.isEmpty() ? QStringLiteral("Developer") : it->stored.title,
             .lane = lane.isEmpty() ? it->stored.title : lane,
             .paths = strings(payload.value(QStringLiteral("paths")))},
            QStringLiteral("user")));
    }
    if (type == QStringLiteral("team.member.update")) {
        const teams::Team* team = teams_->team(teamId);
        const QString sessionId = payload.value(QStringLiteral("session_id")).toString();
        const teams::Member* member = team ? team->member(sessionId) : nullptr;
        return answer(teams_->updateMember(teamId, sessionId,
                                           payload.contains(QStringLiteral("lane"))
                                               ? payload.value(QStringLiteral("lane")).toString()
                                               : (member ? member->lane : QString{}),
                                           payload.contains(QStringLiteral("paths"))
                                               ? strings(payload.value(QStringLiteral("paths")))
                                               : (member ? member->paths : QStringList{}),
                                           QStringLiteral("user"),
                                           payload.value(QStringLiteral("reason")).toString()));
    }
    if (type == QStringLiteral("team.member.remove"))
        return answer(teams_->removeMember(teamId,
                                           payload.value(QStringLiteral("session_id")).toString(),
                                           QStringLiteral("user")));
    return teamTaskCommand(type, request);
}

QJsonObject SessionHost::createTeam(const QJsonObject& payload) {
    const QString repository =
        repositoryOf(normalizedWorktree(payload.value(QStringLiteral("repository")).toString()));
    if (repository.isEmpty())
        return failure(QStringLiteral("invalid_repository"),
                       QStringLiteral("Select a repository."));
    const QString teamName = payload.value(QStringLiteral("name")).toString().trimmed();
    if (const auto invalid = teams::rules::validName(teamName, QStringLiteral("The team")))
        return failure(*invalid);
    const QJsonObject plan = payload.value(QStringLiteral("plan")).toObject();
    const QJsonObject developers = plan.value(QStringLiteral("developers")).toObject();
    const QJsonArray named = developers.value(QStringLiteral("members")).toArray();
    const int count = developers.value(QStringLiteral("count")).toInt(qMax(1, int(named.size())));
    if (count < 1)
        return failure(QStringLiteral("last_developer"),
                       QStringLiteral("A team needs at least one developer."),
                       teams::role::developer);
    if (count > teams::maximumDevelopers)
        return failure(QStringLiteral("team_full"),
                       QStringLiteral("A team has at most five developers."),
                       teams::role::developer);
    QList<MemberPlan> members{planOf(teams::role::leader,
                                     plan.value(QStringLiteral("leader")).toObject(),
                                     QStringLiteral("Lead ") + teamName)};
    for (int index = 0; index < count; ++index) {
        QJsonObject agent = developers;
        agent.remove(QStringLiteral("name"));
        const QJsonObject own = named.at(index).toObject();
        for (auto it = own.constBegin(); it != own.constEnd(); ++it)
            agent.insert(it.key(), it.value());
        MemberPlan member =
            planOf(teams::role::developer, agent, QStringLiteral("Developer %1").arg(index + 1));
        if (member.lane.isEmpty())
            member.lane = member.name;
        members.append(member);
    }
    members.append(planOf(teams::role::reviewer, plan.value(QStringLiteral("reviewer")).toObject(),
                          QStringLiteral("Reviewer")));
    // Every member is checked before any conversation starts: all or nothing.
    for (const MemberPlan& member : std::as_const(members)) {
        if (const auto invalid = teams::rules::validName(member.name, QStringLiteral("a member")))
            return failure(invalid->code, invalid->message, member.role);
        if (!knownProvider(member.provider))
            return failure(QStringLiteral("provider_unavailable"),
                           QStringLiteral("Choose the agent of the %1.").arg(member.role),
                           member.role);
        if (member.role == teams::role::reviewer &&
            !teams::rules::reviewerProviderSupported(member.provider))
            return failure(QStringLiteral("reviewer_unsupported"),
                           QStringLiteral("The reviewer must be a Claude conversation, the "
                                          "provider that keeps a review read-only."),
                           member.role);
        if (member.provider != QStringLiteral("codex") && !availableProvider(member.provider))
            return failure(QStringLiteral("provider_unavailable"),
                           missingProviderMessage(member.provider), member.role);
    }
    teams::Team draft;
    draft.repository = repository;
    draft.name = teamName;
    draft.goal = payload.value(QStringLiteral("goal")).toString().trimmed().left(4000);
    draft.integrationBranch =
        payload.value(QStringLiteral("integration_branch")).toString().trimmed();
    draft.mergePolicy = payload.value(QStringLiteral("merge_policy")).toString(teams::role::leader);
    draft.executionPolicy =
        payload.value(QStringLiteral("execution_policy")).toString(QStringLiteral("ask"));
    if (!QSet<QString>{QStringLiteral("read-only"), QStringLiteral("ask"), QStringLiteral("full")}
             .contains(draft.executionPolicy))
        return failure(QStringLiteral("invalid_payload"),
                       QStringLiteral("Choose the team's permissions."));
    // Full access needs the same acknowledgement as choosing it for one conversation.
    if (draft.executionPolicy == QStringLiteral("full") &&
        !payload.value(QStringLiteral("risk_acknowledged")).toBool())
        return failure(QStringLiteral("execution_policy_denied"),
                       QStringLiteral("The requested permission was not authorized."));
    draft.leaderWritesRequirements =
        payload.value(QStringLiteral("leader_writes_requirements")).toBool(true);
    draft.sharedPaths = strings(payload.value(QStringLiteral("shared_paths")));
    // Lanes are checked before anything starts, with placeholder ids.
    for (int index = 0; index < members.size(); ++index)
        draft.members.append({.sessionId = QStringLiteral("planned-%1").arg(index),
                              .role = members.at(index).role,
                              .name = members.at(index).name,
                              .lane = members.at(index).lane,
                              .paths = members.at(index).paths});
    for (const teams::Member& member : std::as_const(draft.members)) {
        if (member.role != teams::role::developer)
            continue;
        const auto paths = teams::rules::normalisedPaths(member.paths);
        if (!paths)
            return failure(QStringLiteral("invalid_payload"),
                           QStringLiteral("A lane's paths are relative to the repository root."),
                           member.role);
        teams::Team others = draft;
        others.members.removeIf(
            [&](const teams::Member& entry) { return entry.sessionId == member.sessionId; });
        if (const auto overlap = teams::rules::laneOverlap(others, member.sessionId, *paths))
            return failure(overlap->code, overlap->message, member.role);
    }
    QStringList started;
    for (int index = 0; index < members.size(); ++index) {
        const MemberPlan& member = members.at(index);
        QString problem;
        const QString sessionId = createMemberConversation(
            repository, member.provider, member.model, member.effort, member.name, &problem,
            // The reviewer keeps a conversation's default; its review turns are read-only.
            member.role == teams::role::reviewer ? QString{} : draft.executionPolicy);
        if (sessionId.isEmpty()) {
            for (const QString& id : std::as_const(started))
                discardMemberConversation(id);
            return failure(QStringLiteral("member_unavailable"), problem, member.role);
        }
        started.append(sessionId);
        draft.members[index].sessionId = sessionId;
    }
    const auto result = teams_->create(draft, QStringLiteral("user"));
    if (!result.ok()) {
        for (const QString& id : std::as_const(started))
            discardMemberConversation(id);
        return failure(*result.error);
    }
    const teams::Team* team = teams_->team(result.teamId);
    return {{QStringLiteral("ok"), true}, {QStringLiteral("team"), teams_->teamJson(*team)}};
}

} // namespace ketplus::daemon
