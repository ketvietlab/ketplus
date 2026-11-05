#pragma once

namespace ketplus::daemon::transcript_window_queries {

inline constexpr auto latestTurn =
    "SELECT turn_sequence FROM replay_events WHERE session_id = ? AND generation = ? "
    "AND cursor < ? ORDER BY cursor DESC LIMIT 1";
inline constexpr auto previousTurn =
    "SELECT turn_sequence FROM replay_events WHERE session_id = ? AND generation = ? "
    "AND turn_sequence < ? ORDER BY turn_sequence DESC, cursor DESC LIMIT 1";
inline constexpr auto turnStart =
    "SELECT cursor FROM replay_events WHERE session_id = ? AND generation = ? "
    "AND turn_sequence = ? ORDER BY cursor LIMIT 1";
inline constexpr auto window =
    "SELECT cursor, turn_sequence, event_type, payload, sent_at, item_id FROM replay_events "
    "WHERE session_id = ? AND generation = ? AND cursor >= ? AND cursor < ? "
    "ORDER BY cursor DESC LIMIT ?";
inline constexpr auto firstTurnAfter =
    "SELECT turn_sequence FROM replay_events WHERE session_id = ? AND generation = ? "
    "AND cursor > ? AND cursor <= ? ORDER BY cursor LIMIT 1";
inline constexpr auto nextTurn =
    "SELECT turn_sequence FROM replay_events WHERE session_id = ? AND generation = ? "
    "AND turn_sequence > ? AND cursor <= ? ORDER BY turn_sequence, cursor LIMIT 1";
inline constexpr auto windowAfter =
    "SELECT cursor, turn_sequence, event_type, payload, sent_at, item_id FROM replay_events "
    "WHERE session_id = ? AND generation = ? AND cursor > ? AND cursor < ? "
    "ORDER BY cursor LIMIT ?";
inline constexpr auto hasEventAfter =
    "SELECT 1 FROM replay_events WHERE session_id = ? AND generation = ? "
    "AND cursor > ? AND cursor <= ? LIMIT 1";
inline constexpr auto previousItemContext =
    "SELECT 1 FROM replay_events WHERE session_id = ? AND generation = ? AND item_id = ? "
    "AND turn_sequence = ? AND cursor < ? "
    "AND event_type IN ('assistant.delta', 'item.started') LIMIT 1";

} // namespace ketplus::daemon::transcript_window_queries
