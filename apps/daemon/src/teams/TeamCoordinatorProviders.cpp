// T-2: team.provider.set. The coordinator keeps what each member moves to; SessionHost applies
// it when the member's next turn starts (SessionHostTeamProvider.cpp).
#include "teams/TeamCoordinator.h"

namespace ketplus::daemon::teams {
namespace {

QString agentText(const ProviderSwitch& target) {
    QString text = target.provider;
    if (!target.model.isEmpty())
        text += QStringLiteral(" ") + target.model;
    if (!target.effort.isEmpty())
        text += QStringLiteral(" (") + target.effort + QStringLiteral(")");
    return text;
}

} // namespace

TeamResult TeamCoordinator::setProvider(const qint64 teamId, const ProviderSwitch& target,
                                        const QStringList& chosen, const QStringList& switching,
                                        const QJsonArray& skipped, const bool teamDefault,
                                        const QString& actor, const bool fullAccess) {
    Team* team = mutableTeam(teamId);
    if (!team)
        return {.error = TeamError{QStringLiteral("team_not_found"),
                                   QStringLiteral("The team no longer exists.")}};
    const Team before = *team;
    // The newest request is the one that holds.
    for (const QString& sessionId : chosen)
        if (Member* member = team->member(sessionId))
            member->pendingSwitch.reset();
    QStringList names;
    for (const QString& sessionId : switching) {
        Member* member = team->member(sessionId);
        if (!member)
            continue;
        member->pendingSwitch = target;
        member->pendingSwitch->requestedAt = nowIso();
        names.append(member->name);
    }
    if (teamDefault)
        team->provider = target.provider;
    // T-3: Full access is the team's; a running turn keeps the policy it started with.
    const bool granted = fullAccess && team->executionPolicy != QStringLiteral("full");
    if (granted) {
        team->executionPolicy = QStringLiteral("full");
        for (Member& member : team->members)
            if (member.role != role::reviewer)
                member.pendingPolicy = QStringLiteral("full");
    }
    QStringList refused;
    for (const QJsonValue& value : skipped) {
        const QJsonObject entry = value.toObject();
        refused.append(
            QStringLiteral("%1 (%2)").arg(entry.value(QStringLiteral("name")).toString(),
                                          entry.value(QStringLiteral("reason")).toString()));
    }
    QString text =
        say(team->language, QT_TRANSLATE_NOOP("Team", "Agent changed to %1 for: %2."))
            .arg(agentText(target), names.isEmpty()
                                        ? say(team->language, QT_TRANSLATE_NOOP("Team", "(none)"))
                                        : names.join(QStringLiteral(", ")));
    if (!refused.isEmpty())
        text += say(team->language, QT_TRANSLATE_NOOP("Team", " Not changed: %1."))
                    .arg(refused.join(QStringLiteral("; ")));
    const QJsonObject data{{QStringLiteral("provider"), target.provider},
                           {QStringLiteral("model"), target.model},
                           {QStringLiteral("effort"), target.effort},
                           {QStringLiteral("members"), QJsonArray::fromStringList(switching)},
                           {QStringLiteral("skipped"), skipped},
                           {QStringLiteral("team_default"), teamDefault}};
    QList<Update> updates;
    if (granted)
        updates.append(
            {.kind = QStringLiteral("execution_policy_changed"),
             .text = say(team->language,
                         QT_TRANSLATE_NOOP("Team", "Permissions changed to Full access. Each "
                                                   "member takes it when its next turn starts.")),
             .data = QJsonObject{{QStringLiteral("execution_policy"), team->executionPolicy},
                                 {QStringLiteral("previous"), before.executionPolicy}}});
    updates.append({.kind = QStringLiteral("provider_set"), .text = text, .data = data});
    if (auto failed = commit(*team, before, actor, updates))
        return {.error = failed};
    return {.teamId = teamId};
}

void TeamCoordinator::finishPolicy(const QString& sessionId) {
    Team* team = mutableTeamOf(sessionId);
    Member* member = team ? team->member(sessionId) : nullptr;
    if (!member || member->pendingPolicy.isEmpty())
        return;
    const Team before = *team;
    member->pendingPolicy.clear();
    if (auto failed = commit(*team, before, QStringLiteral("daemon"), {}))
        qWarning("Unable to record the policy of %s: %s", qUtf8Printable(sessionId),
                 qUtf8Printable(failed->message));
}

std::optional<ProviderSwitch> TeamCoordinator::pendingSwitch(const QString& sessionId) const {
    const Team* team = teamOf(sessionId);
    const Member* member = team ? team->member(sessionId) : nullptr;
    return member ? member->pendingSwitch : std::nullopt;
}

void TeamCoordinator::finishProviderSwitch(const QString& sessionId, const QString& reason) {
    Team* team = mutableTeamOf(sessionId);
    Member* member = team ? team->member(sessionId) : nullptr;
    if (!member || !member->pendingSwitch)
        return;
    const Team before = *team;
    const ProviderSwitch target = *member->pendingSwitch;
    member->pendingSwitch.reset();
    const QString text =
        reason.isEmpty()
            ? say(team->language, QT_TRANSLATE_NOOP("Team", "%1 now works with %2."))
                  .arg(member->name, agentText(target))
            : say(team->language,
                  QT_TRANSLATE_NOOP("Team", "%1 stays with its agent; %2 was not applied: %3"))
                  .arg(member->name, agentText(target), reason);
    const QJsonObject data{{QStringLiteral("session_id"), sessionId},
                           {QStringLiteral("provider"), target.provider},
                           {QStringLiteral("model"), target.model},
                           {QStringLiteral("effort"), target.effort},
                           {QStringLiteral("applied"), reason.isEmpty()},
                           {QStringLiteral("reason"), reason}};
    if (auto failed = commit(*team, before, QStringLiteral("daemon"),
                             {{.kind = reason.isEmpty() ? QStringLiteral("provider_switched")
                                                        : QStringLiteral("provider_switch_dropped"),
                               .text = text,
                               .actionable = !reason.isEmpty(),
                               .data = data}}))
        qWarning("Unable to record the provider switch of %s: %s", qUtf8Printable(sessionId),
                 qUtf8Printable(failed->message));
}

} // namespace ketplus::daemon::teams
