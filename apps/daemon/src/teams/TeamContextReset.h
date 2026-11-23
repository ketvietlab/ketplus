#pragma once

// T-1: when a turn the team starts gets a fresh native context, and the short briefing that
// turn begins with instead of the context it no longer has. Pure functions of the team.
#include "teams/Team.h"

namespace ketplus::daemon::teams::context_reset {

inline const QString taskChanged = QStringLiteral("task_changed");
inline const QString thresholdReached = QStringLiteral("threshold");
inline const QString contextSize = QStringLiteral("context_size");
inline const QString cacheCold = QStringLiteral("cache_cold");
// A context this large (input, cache read and cache write of the last request) is dearer to
// write back into a cold cache than the briefing that replaces it. Measured over three days of
// local transcripts: after an idle of more than 60 minutes, 184 of 243 Claude requests rewrote
// a cache of about 190 thousand tokens on average, and Codex's cached share of input fell from
// 99.3% (under 5 minutes) to 48.2% (over 60).
inline constexpr qint64 minimumColdContextTokens = 50000;
// The most a briefing takes of the first prompt after a reset.
inline constexpr qsizetype maximumBriefingBytes = 4096;
inline constexpr qsizetype briefingOpenTasks = 20;
inline constexpr qsizetype briefingHistory = 5;

// The task a turn the team starts for `member` is about: `hint` (what the team's message
// named) when it is one of the team's, else the developer's task in progress or the task the
// reviewer reviews now. Empty for the leader, who works across tasks.
[[nodiscard]] QString memberTask(const Team& team, const Member& member, const QString& hint);

// Whether that turn starts a fresh context: the reason (taskChanged, thresholdReached,
// contextSize, cacheCold, the first that holds in that order), or empty. `mark` is the member's
// current context, or nullopt when the team has none; it is updated to what the team keeps when no
// reset happens: a context the team has not seen yet (a new member, a provider handoff) is adopted
// as it is, never reset.
[[nodiscard]] QString decide(const Team& team, const Member& member,
                             std::optional<ContextMark>& mark, const QString& task,
                             const QString& thread, qint64 turnSequence,
                             const ThreadActivity& activity = {});
// Whether the provider's cache of the thread has gone cold and its context is large: the
// thread rested `team.contextResetIdleMinutes` since its last turn ended, and its known
// context holds minimumColdContextTokens or more.
[[nodiscard]] bool cacheWentCold(const Team& team, const ThreadActivity& activity);

// The briefing, at most maximumBriefingBytes of UTF-8: the goal, the member's role and lane,
// its task (state, round, criteria and the last history entries) or, for the leader, the
// first open tasks, and where to read more.
[[nodiscard]] QString briefing(const Team& team, const Member& member, const QString& task);

} // namespace ketplus::daemon::teams::context_reset
