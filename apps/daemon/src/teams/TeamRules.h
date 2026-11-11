#pragma once

#include "teams/Team.h"

#include <optional>

namespace ketplus::daemon::teams {

// A refusal with its protocol error code (ADR 0112, "Protocol").
struct TeamError final {
    QString code;
    QString message;
};

// What the rules need to know about a conversation that is, or would become, a member.
struct Candidate final {
    QString sessionId;
    QString repository;
    QString provider;
    // The team it is on already, 0 for none.
    qint64 teamId{0};
};

// Membership rules, without side effects. Each returns the first rule the change breaks.
namespace rules {

// Only Claude enforces the read-only review guard (ADR 0067).
[[nodiscard]] bool reviewerProviderSupported(const QString& provider);
// A lane's prefixes, normalised: relative, '/' separated, no "." or "..", no trailing '/'.
// Nothing when a prefix is invalid.
[[nodiscard]] std::optional<QStringList> normalisedPaths(const QStringList& paths);
// Whether `path` is `prefix` or below it.
[[nodiscard]] bool pathUnder(const QString& path, const QString& prefix);
[[nodiscard]] std::optional<TeamError> laneOverlap(const Team& team, const QString& exceptSession,
                                                   const QStringList& paths);
// A new team: exactly one leader and one reviewer, one to five developers, all distinct
// conversations of `repository` on no team, a Claude reviewer, lanes that do not overlap.
[[nodiscard]] std::optional<TeamError> canCreate(const Team& team,
                                                 const QList<Candidate>& candidates);
[[nodiscard]] std::optional<TeamError> canAddMember(const Team& team, const Member& member,
                                                    const Candidate& candidate);
[[nodiscard]] std::optional<TeamError> canRemoveMember(const Team& team, const QString& sessionId);
[[nodiscard]] std::optional<TeamError> validName(const QString& name, const QString& what);

} // namespace rules
} // namespace ketplus::daemon::teams
