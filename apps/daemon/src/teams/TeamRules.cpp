#include "teams/TeamRules.h"

#include <QSet>

namespace ketplus::daemon::teams::rules {
namespace {

TeamError refusal(const QString& code, const QString& message) { return {code, message}; }

} // namespace

bool reviewerProviderSupported(const QString& provider) {
    return provider == QStringLiteral("claude");
}

std::optional<QStringList> normalisedPaths(const QStringList& paths) {
    QStringList result;
    for (QString path : paths) {
        path = path.trimmed();
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (path.endsWith(QLatin1Char('/')))
            path.chop(1);
        if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.size() > 512)
            return std::nullopt;
        const QStringList parts = path.split(QLatin1Char('/'));
        for (const QString& part : parts)
            if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral(".."))
                return std::nullopt;
        if (!result.contains(path))
            result.append(path);
    }
    if (result.size() > 32)
        return std::nullopt;
    return result;
}

bool pathUnder(const QString& path, const QString& prefix) {
    return path == prefix || path.startsWith(prefix + QLatin1Char('/'));
}

std::optional<TeamError> laneOverlap(const Team& team, const QString& exceptSession,
                                     const QStringList& paths) {
    for (const Member& member : team.members) {
        if (member.sessionId == exceptSession || member.role != role::developer)
            continue;
        for (const QString& mine : paths)
            for (const QString& theirs : member.paths)
                if (pathUnder(mine, theirs) || pathUnder(theirs, mine))
                    return refusal(
                        QStringLiteral("lane_overlap"),
                        QStringLiteral("%1 is already in the lane of %2.")
                            .arg(mine, member.name.isEmpty() ? member.lane : member.name));
    }
    return std::nullopt;
}

std::optional<TeamError> validName(const QString& name, const QString& what) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return refusal(QStringLiteral("invalid_payload"),
                       QStringLiteral("%1 needs a name.").arg(what));
    if (trimmed.toUtf8().size() > 120)
        return refusal(QStringLiteral("invalid_payload"),
                       QStringLiteral("The name of %1 is longer than 120 bytes.").arg(what));
    for (const QChar character : trimmed)
        if (character.category() == QChar::Other_Control)
            return refusal(QStringLiteral("invalid_payload"),
                           QStringLiteral("The name of %1 has a control character.").arg(what));
    return std::nullopt;
}

std::optional<TeamError> validBranch(const QString& branch) {
    if (branch.isEmpty())
        return std::nullopt;
    const auto invalid = [&branch] {
        return refusal(QStringLiteral("invalid_payload"),
                       QStringLiteral("\"%1\" is not a branch name.").arg(branch));
    };
    if (branch.startsWith(QLatin1Char('-')) || branch == QStringLiteral("@") ||
        branch.endsWith(QLatin1Char('.')) || branch.contains(QStringLiteral("..")) ||
        branch.contains(QStringLiteral("@{")) || branch.toUtf8().size() > 255)
        return invalid();
    for (const QChar character : branch)
        if (character.category() == QChar::Other_Control || character == QLatin1Char(' ') ||
            QStringLiteral("~^:?*[\\").contains(character))
            return invalid();
    for (const QString& component : branch.split(QLatin1Char('/')))
        if (component.isEmpty() || component.startsWith(QLatin1Char('.')) ||
            component.endsWith(QStringLiteral(".lock")))
            return invalid();
    return std::nullopt;
}

std::optional<TeamError> canCreate(const Team& team, const QList<Candidate>& candidates) {
    if (auto invalid = validName(team.name, QStringLiteral("The team")))
        return invalid;
    if (team.count(role::leader) == 0)
        return refusal(QStringLiteral("role_required"), QStringLiteral("A team needs a leader."));
    if (team.count(role::leader) > 1)
        return refusal(QStringLiteral("role_taken"), QStringLiteral("A team has one leader."));
    if (team.count(role::reviewer) == 0)
        return refusal(QStringLiteral("role_required"), QStringLiteral("A team needs a reviewer."));
    if (team.count(role::reviewer) > 1)
        return refusal(QStringLiteral("role_taken"), QStringLiteral("A team has one reviewer."));
    const int developers = team.count(role::developer);
    if (developers < 1)
        return refusal(QStringLiteral("last_developer"),
                       QStringLiteral("A team needs at least one developer."));
    if (developers > maximumDevelopers)
        return refusal(QStringLiteral("team_full"),
                       QStringLiteral("A team has at most five developers."));
    if (team.mergePolicy != role::leader && team.mergePolicy != QStringLiteral("user"))
        return refusal(QStringLiteral("invalid_payload"),
                       QStringLiteral("Who merges tasks is \"leader\" or \"user\"."));
    QSet<QString> seen;
    Team growing = team;
    growing.members.clear();
    for (const Member& member : team.members) {
        if (member.role != role::leader && member.role != role::developer &&
            member.role != role::reviewer)
            return refusal(QStringLiteral("invalid_payload"),
                           QStringLiteral("Unknown role %1.").arg(member.role));
        if (seen.contains(member.sessionId))
            return refusal(QStringLiteral("already_member"),
                           QStringLiteral("A conversation has one role on a team."));
        seen.insert(member.sessionId);
        Candidate candidate;
        for (const Candidate& entry : candidates)
            if (entry.sessionId == member.sessionId)
                candidate = entry;
        if (candidate.sessionId.isEmpty())
            return refusal(
                QStringLiteral("invalid_payload"),
                QStringLiteral("The conversation of %1 does not exist.").arg(member.name));
        if (auto refused = canAddMember(growing, member, candidate))
            return refused;
        growing.members.append(member);
    }
    return std::nullopt;
}

std::optional<TeamError> canAddMember(const Team& team, const Member& member,
                                      const Candidate& candidate) {
    if (auto invalid = validName(member.name, QStringLiteral("a member")))
        return invalid;
    if (candidate.teamId != 0 || team.member(candidate.sessionId))
        return refusal(QStringLiteral("already_member"),
                       QStringLiteral("This conversation is already on a team."));
    if (candidate.repository != team.repository)
        return refusal(QStringLiteral("other_repository"),
                       QStringLiteral("A member works in the team's repository."));
    if (member.role == role::leader && team.count(role::leader) > 0)
        return refusal(QStringLiteral("role_taken"), QStringLiteral("The team has a leader."));
    if (member.role == role::reviewer && team.count(role::reviewer) > 0)
        return refusal(QStringLiteral("role_taken"), QStringLiteral("The team has a reviewer."));
    if (member.role == role::reviewer && !reviewerProviderSupported(candidate.provider))
        return refusal(QStringLiteral("reviewer_unsupported"),
                       QStringLiteral("The reviewer must be a Claude conversation, the provider "
                                      "that keeps a review read-only."));
    if (member.role == role::developer) {
        if (team.count(role::developer) >= maximumDevelopers)
            return refusal(QStringLiteral("team_full"),
                           QStringLiteral("The team has five developers."));
        const auto paths = normalisedPaths(member.paths);
        if (!paths)
            return refusal(QStringLiteral("invalid_payload"),
                           QStringLiteral("A lane's paths are relative to the repository root."));
        if (auto overlap = laneOverlap(team, member.sessionId, *paths))
            return overlap;
    }
    return std::nullopt;
}

std::optional<TeamError> canRemoveMember(const Team& team, const QString& sessionId) {
    const Member* member = team.member(sessionId);
    if (!member)
        return refusal(QStringLiteral("not_member"),
                       QStringLiteral("This conversation is not on the team."));
    if (member->role != role::developer)
        return refusal(
            QStringLiteral("role_required"),
            QStringLiteral("A team keeps its %1; delete the team instead.").arg(member->role));
    if (team.count(role::developer) <= 1)
        return refusal(QStringLiteral("last_developer"),
                       QStringLiteral("A team keeps at least one developer."));
    return std::nullopt;
}

} // namespace ketplus::daemon::teams::rules
