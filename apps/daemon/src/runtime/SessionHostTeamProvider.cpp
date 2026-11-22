// T-2: team.provider.set moves a team's members to another provider. Nothing is interrupted:
// each member keeps a pending switch in the team, and its next turn takes it through the
// provider handoff of startTurn (a fresh native context with the conversation's transcript).
#include "runtime/SessionHost.h"

#include "agents/ReviewerPolicy.h"
#include "teams/TeamRules.h"

#include <QSet>

namespace ketplus::daemon {
namespace {

QJsonObject failure(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

QString providerTitle(const QString& provider) {
    if (provider == QStringLiteral("codex"))
        return QStringLiteral("Codex");
    if (provider == QStringLiteral("claude"))
        return QStringLiteral("Claude");
    if (provider == QStringLiteral("cursor"))
        return QStringLiteral("Cursor");
    if (provider == QStringLiteral("copilot"))
        return QStringLiteral("Copilot");
    return provider;
}

// What a provider reported last that keeps every turn from starting (ADR 0110). A reached
// limit is left out: it resets on its own, and a turn says so when it still holds.
QString blockingState(const QString& state) {
    if (state == QStringLiteral("signed_out"))
        return QStringLiteral("it is signed out");
    if (state == QStringLiteral("expired"))
        return QStringLiteral("its sign-in has expired");
    if (state == QStringLiteral("key_rejected"))
        return QStringLiteral("its API key was rejected");
    return {};
}

} // namespace

QJsonObject SessionHost::providerNotReady(const QString& provider) const {
    if (!teams::rules::knownProvider(provider))
        return failure(QStringLiteral("invalid_payload"),
                       QStringLiteral("Choose the agent the team works with."));
    if (availableProvider(provider) == nullptr)
        return failure(QStringLiteral("provider_unavailable"), missingProviderMessage(provider));
    if (const QString reason = blockingState(readiness_.reportedState(provider)); !reason.isEmpty())
        return failure(QStringLiteral("provider_not_ready"),
                       QStringLiteral("%1 is not ready: %2. Fix it in Settings → Providers, then "
                                      "try again.")
                           .arg(providerTitle(provider), reason));
    return {};
}

QJsonObject SessionHost::providerSwitchBlocker(const Session& session, const QString& role,
                                               const QString& provider,
                                               const QString& teamPolicy) const {
    // A handoff starts a fresh native context; in a switched worktree only providers that
    // support worktree switching can take it (startTurn, session.worktree.switch).
    if (session.stored.contextRevision > 1 && provider != session.stored.adapter &&
        provider != QStringLiteral("codex") && provider != QStringLiteral("claude"))
        return {{QStringLiteral("error_code"), QStringLiteral("context_switch_unsupported")},
                {QStringLiteral("reason"),
                 QStringLiteral("%1 cannot continue a conversation that moved to another "
                                "worktree.")
                     .arg(providerTitle(provider))}};
    if (role == teams::role::reviewer &&
        !reviewer::supported(provider, reviewer::teamReviewerPolicy(provider, teamPolicy)))
        return {{QStringLiteral("error_code"), QStringLiteral("reviewer_unsupported")},
                {QStringLiteral("reason"), reviewer::unsupportedMessage(provider)}};
    return {};
}

QJsonObject SessionHost::setTeamProvider(const QJsonObject& payload) {
    const qint64 teamId = payload.value(QStringLiteral("team_id")).toString().toLongLong();
    const teams::Team* team = teams_->team(teamId);
    if (!team)
        return failure(QStringLiteral("team_not_found"),
                       QStringLiteral("The team no longer exists."));
    const teams::ProviderSwitch target{
        .provider = payload.value(QStringLiteral("provider")).toString(),
        .model = payload.value(QStringLiteral("model")).toString().trimmed(),
        .effort = payload.value(QStringLiteral("effort")).toString().trimmed()};
    if (const QJsonObject refused = providerNotReady(target.provider); !refused.isEmpty())
        return refused;
    // Without "members" every member moves, and the provider becomes the team's own.
    const bool everyone = !payload.contains(QStringLiteral("members"));
    QStringList chosen;
    if (everyone) {
        chosen = team->memberSessions();
    } else {
        for (const QJsonValue& value : payload.value(QStringLiteral("members")).toArray()) {
            const QString sessionId = value.toString();
            if (!team->member(sessionId))
                return failure(QStringLiteral("not_member"),
                               QStringLiteral("Choose members of this team."));
            if (!chosen.contains(sessionId))
                chosen.append(sessionId);
        }
        if (chosen.isEmpty())
            return failure(QStringLiteral("invalid_payload"),
                           QStringLiteral("Choose at least one member."));
    }
    // A reviewer that could not review on the new provider refuses the whole change: the team
    // keeps its permissions, and Full access is the user's own choice, asked for separately.
    for (const QString& sessionId : std::as_const(chosen)) {
        const teams::Member* member = team->member(sessionId);
        if (member->role == teams::role::reviewer &&
            !reviewer::supported(target.provider, reviewer::teamReviewerPolicy(
                                                      target.provider, team->executionPolicy))) {
            QJsonObject refused = failure(QStringLiteral("reviewer_unsupported"),
                                          reviewer::unsupportedMessage(target.provider) +
                                              QStringLiteral(" Give the team Full access first, "
                                                             "or keep the reviewer's agent."));
            refused.insert(QStringLiteral("role"), teams::role::reviewer);
            refused.insert(QStringLiteral("session_id"), sessionId);
            return refused;
        }
    }
    QStringList switching;
    QJsonArray skipped;
    QJsonArray results;
    for (const QString& sessionId : std::as_const(chosen)) {
        const teams::Member* member = team->member(sessionId);
        QJsonObject result{{QStringLiteral("session_id"), sessionId},
                           {QStringLiteral("name"), member->name},
                           {QStringLiteral("role"), member->role}};
        const auto session = sessions_.constFind(sessionId);
        QJsonObject blocker;
        if (session == sessions_.cend())
            blocker = {{QStringLiteral("error_code"), QStringLiteral("session_not_found")},
                       {QStringLiteral("reason"),
                        QStringLiteral("The member's conversation no longer exists.")}};
        else
            blocker = providerSwitchBlocker(*session, member->role, target.provider,
                                            team->executionPolicy);
        if (!blocker.isEmpty()) {
            for (auto it = blocker.constBegin(); it != blocker.constEnd(); ++it)
                result.insert(it.key(), it.value());
            result.insert(QStringLiteral("status"), QStringLiteral("skipped"));
            skipped.append(QJsonObject{
                {QStringLiteral("session_id"), sessionId},
                {QStringLiteral("name"), member->name},
                {QStringLiteral("error_code"), blocker.value(QStringLiteral("error_code"))},
                {QStringLiteral("reason"), blocker.value(QStringLiteral("reason"))}});
            results.append(result);
            continue;
        }
        // Already there, with nothing else to change: nothing waits, and an earlier pending
        // switch is withdrawn.
        if (session->stored.adapter == target.provider && target.model.isEmpty() &&
            target.effort.isEmpty()) {
            result.insert(QStringLiteral("status"), QStringLiteral("unchanged"));
            results.append(result);
            continue;
        }
        switching.append(sessionId);
        result.insert(QStringLiteral("status"), QStringLiteral("pending"));
        // A running turn, or one that waits for an approval, finishes on its provider first.
        result.insert(QStringLiteral("applies"), agentConversationOf(*session).idle
                                                     ? QStringLiteral("next_turn")
                                                     : QStringLiteral("after_current_turn"));
        results.append(result);
    }
    const auto changed = teams_->setProvider(teamId, target, chosen, switching, skipped, everyone,
                                             QStringLiteral("user"));
    if (!changed.ok())
        return failure(changed.error->code, changed.error->message);
    const teams::Team* now = teams_->team(teamId);
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("team"), now ? teams_->teamJson(*now) : QJsonObject{}},
            {QStringLiteral("results"), results}};
}

bool SessionHost::takeTeamProviderSwitch(Session& session, QJsonObject& options) {
    if (!teams_ || !teams_->isLoaded())
        return false;
    const auto pending = teams_->pendingSwitch(session.stored.id);
    if (!pending)
        return false;
    const teams::Team* team = teams_->teamOf(session.stored.id);
    const teams::Member* member = team ? team->member(session.stored.id) : nullptr;
    if (!member)
        return false;
    // What held when the switch was asked may not hold now: the member may have moved to a
    // task worktree, or the provider may have signed out. It then stays where it is.
    QJsonObject blocker =
        providerSwitchBlocker(session, member->role, pending->provider, team->executionPolicy);
    if (blocker.isEmpty())
        if (const QJsonObject refused = providerNotReady(pending->provider); !refused.isEmpty())
            blocker = {{QStringLiteral("reason"), refused.value(QStringLiteral("error_message"))}};
    if (!blocker.isEmpty()) {
        teams_->finishProviderSwitch(session.stored.id,
                                     blocker.value(QStringLiteral("reason")).toString());
        return false;
    }
    options.insert(QStringLiteral("provider"), pending->provider);
    // A model or effort chosen for the previous provider does not carry over.
    for (const auto& [key, value] : {std::pair{QStringLiteral("model"), pending->model},
                                     std::pair{QStringLiteral("effort"), pending->effort}}) {
        if (value.isEmpty())
            options.remove(key);
        else
            options.insert(key, value);
    }
    if (pending->provider != QStringLiteral("codex") &&
        pending->provider != QStringLiteral("claude"))
        options.insert(QStringLiteral("compactWindow"), QJsonValue(QJsonValue::Null));
    return true;
}

void SessionHost::finishTeamProviderSwitch(Session& session) {
    const auto pending = teams_->pendingSwitch(session.stored.id);
    const teams::Team* team = teams_->teamOf(session.stored.id);
    const teams::Member* member = team ? team->member(session.stored.id) : nullptr;
    if (!pending || !member)
        return;
    // Empty is the provider's default; startTurn applies a non-empty one from the options.
    session.stored.model = pending->model;
    session.stored.effort = pending->effort;
    // ketplus#295: a Codex reviewer takes the team's Full access (checked above); any other
    // reviewer keeps a conversation's default. The team's own policy never changes here.
    if (member->role == teams::role::reviewer) {
        const QString policy =
            reviewer::teamReviewerPolicy(pending->provider, team->executionPolicy);
        session.stored.executionPolicy =
            policy.isEmpty() ? StoredSession{}.executionPolicy : policy;
    }
    // Codex is told the member's name once its thread starts, as for a new member.
    if (pending->provider == QStringLiteral("codex"))
        session.pendingProviderTitle = session.stored.title;
    teams_->finishProviderSwitch(session.stored.id, {});
}

} // namespace ketplus::daemon
