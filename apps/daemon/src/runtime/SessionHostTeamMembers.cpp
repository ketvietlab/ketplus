// ADR 0114: team.member.add, for trusted local clients and paired devices. T-4: a local client
// may add several developers at once with `developers`; all of them join or none does.
#include "runtime/SessionHost.h"
#include "teams/TeamRules.h"

#include "agents/ReviewerPolicy.h"

#include <QJsonArray>
#include <QSet>

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

QJsonObject failure(const teams::TeamError& error, const QString& role = {}) {
    return failure(error.code, error.message, role);
}

// One member to start. T-10: a lane is only a name; `paths` from an older client is ignored.
struct NewMember final {
    QString name;
    QString lane;
};

QString folded(const QString& text) { return text.trimmed().toCaseFolded(); }

// T-4: the developers of one request are told apart from each other and from the team's members
// by their names, and by their lanes from the team's developers.
std::optional<teams::TeamError> distinct(const teams::Team& team, const QList<NewMember>& added) {
    QSet<QString> names;
    QSet<QString> lanes;
    for (const teams::Member& member : team.members) {
        names.insert(folded(member.name));
        if (member.role == teams::role::developer && !member.lane.isEmpty())
            lanes.insert(folded(member.lane));
    }
    for (const NewMember& member : added) {
        if (names.contains(folded(member.name)))
            return teams::TeamError{
                QStringLiteral("invalid_payload"),
                QStringLiteral("The team already has a member named %1.").arg(member.name)};
        if (lanes.contains(folded(member.lane)))
            return teams::TeamError{
                QStringLiteral("invalid_payload"),
                QStringLiteral("The team already has a lane named %1.").arg(member.lane)};
        names.insert(folded(member.name));
        lanes.insert(folded(member.lane));
    }
    return std::nullopt;
}

} // namespace

QJsonObject SessionHost::addTeamMembers(const qint64 teamId, const QJsonObject& payload) {
    const teams::Team* team = teams_->team(teamId);
    if (!team)
        return failure(QStringLiteral("team_not_found"),
                       QStringLiteral("The team no longer exists."));
    // T-23: "role" adds a developer (the default) or a team's second reviewer.
    const QString role = payload.value(QStringLiteral("role")).toString(teams::role::developer);
    if (role != teams::role::developer && role != teams::role::reviewer)
        return failure(QStringLiteral("invalid_payload"),
                       QStringLiteral("A member added to a team is a developer or a "
                                      "reviewer."));
    const bool reviewer = role == teams::role::reviewer;
    const bool several = payload.contains(QStringLiteral("developers"));
    QList<NewMember> added;
    if (several) {
        const QJsonArray developers = payload.value(QStringLiteral("developers")).toArray();
        if (reviewer || developers.isEmpty() || developers.size() > teams::maximumDevelopers)
            return failure(QStringLiteral("invalid_payload"),
                           QStringLiteral("Name one to five developers to add."), role);
        for (const QJsonValue& value : developers) {
            const QJsonObject developer = value.toObject();
            const QString name = developer.value(QStringLiteral("name")).toString().trimmed();
            const QString lane = developer.value(QStringLiteral("lane")).toString().trimmed();
            added.append({.name = name, .lane = lane.isEmpty() ? name : lane});
        }
    } else {
        const QString name = payload.value(QStringLiteral("name")).toString().trimmed();
        const QString lane = payload.value(QStringLiteral("lane")).toString().trimmed();
        added.append({.name = name.isEmpty() && reviewer ? QStringLiteral("Reviewer 2") : name,
                      .lane = lane.isEmpty() ? name : lane});
    }
    // Every member is checked before any conversation starts: all or nothing.
    for (const NewMember& member : std::as_const(added))
        if (const auto invalid =
                teams::rules::validName(member.name, reviewer ? QStringLiteral("a reviewer")
                                                              : QStringLiteral("a developer")))
            return failure(*invalid, several ? role : QString{});
    if (several)
        if (const auto taken = distinct(*team, added))
            return failure(*taken, role);
    const int room = teams::maximumDevelopers - team->count(teams::role::developer);
    if (!reviewer && room < added.size())
        return failure(QStringLiteral("team_full"),
                       room <= 0 ? QStringLiteral("The team has five developers.")
                       : room == 1
                           ? QStringLiteral("The team has room for one more developer.")
                           : QStringLiteral("The team has room for %1 more developers.").arg(room));
    if (reviewer && team->count(teams::role::reviewer) >= teams::maximumReviewers)
        return failure(QStringLiteral("role_taken"),
                       QStringLiteral("The team has two reviewers, the most a team has."), role);
    // T-2: a member added without an agent starts with the team's provider, at its default
    // model; a model is not carried over from another member.
    QString provider = payload.value(QStringLiteral("provider")).toString();
    if (provider.isEmpty())
        provider = team->provider;
    const QString model = payload.value(QStringLiteral("model")).toString();
    const QString effort = payload.value(QStringLiteral("effort")).toString();
    if (!teams::rules::knownProvider(provider))
        return failure(QStringLiteral("provider_unavailable"),
                       reviewer ? QStringLiteral("Choose the reviewer's agent.")
                                : QStringLiteral("Choose the developer's agent."),
                       role);
    // ketplus#295: a new Codex reviewer takes the team's Full access; Claude keeps its own.
    const QString reviewerPolicy = reviewer::teamReviewerPolicy(provider, team->executionPolicy);
    if (reviewer && !reviewer::supported(provider, reviewerPolicy))
        return failure(QStringLiteral("reviewer_unsupported"),
                       reviewer::unsupportedMessage(provider), role);
    if (provider != QStringLiteral("codex") && !availableProvider(provider))
        return failure(QStringLiteral("provider_unavailable"), missingProviderMessage(provider),
                       role);
    const QString repository = team->repository;
    const QString policy = reviewer ? reviewerPolicy : team->executionPolicy;
    QList<teams::Member> members;
    QStringList started;
    const auto discardStarted = [this, &started] {
        for (const QString& id : std::as_const(started))
            discardMemberConversation(id);
    };
    for (const NewMember& member : std::as_const(added)) {
        QString problem;
        // A Claude reviewer keeps its own mode; its review turns are read-only (ADR 0114 §2).
        const QString sessionId = createMemberConversation(repository, provider, model, effort,
                                                           member.name, &problem, policy);
        if (sessionId.isEmpty()) {
            discardStarted();
            // T-4: none of them joined, and the reply says whose conversation could not start.
            return failure(QStringLiteral("member_unavailable"),
                           added.size() == 1
                               ? problem
                               : QStringLiteral("No developer was added: %1 could not start. %2")
                                     .arg(member.name, problem)
                                     .trimmed(),
                           role);
        }
        started.append(sessionId);
        members.append(
            reviewer ? teams::Member{.sessionId = sessionId, .role = role, .name = member.name}
                     : teams::Member{.sessionId = sessionId,
                                     .role = role,
                                     .name = member.name,
                                     .lane = member.lane});
    }
    const auto result = teams_->addMembers(teamId, members, QStringLiteral("user"));
    if (!result.ok()) {
        discardStarted();
        return failure(*result.error, several ? role : QString{});
    }
    const teams::Team* changed = teams_->team(teamId);
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("team"), changed ? teams_->teamJson(*changed) : QJsonObject{}}};
}

} // namespace ketplus::daemon
