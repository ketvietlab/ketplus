# ADR 0115: Team messages are bounded by the team, not by its age

Status: accepted (2026-11-11). Extends ADR 0114.

## Context

The daemon sends every local message in one frame of at most 1 MiB
(`protocol::maximumFrameBytes`). Before this decision, `LocalIpcServer::send` dropped the
client whenever a frame was larger than that.

Every team message carried the whole team document: each task's full history, the held
messages and every decision. `team.get` also carried 200 Updates.

A real team grew past the limit in one day:
- A lane check against a stale base flagged about 3,000 paths on each task (fixed in #545).
- Each flag stored the list on the task, in the history and in the Updates.
- The document reached 8 MB and the Updates 6 MB.

The effects:
- `team.list` failed, and the desktop's directory never received the teams. The sidebar
  showed the members as loose conversations.
- Every `team.changed` event dropped every connected desktop.
- Opening the team page sent `team.get` (5.4 MB), and the app lost its daemon connection
  again.

The bounds in #545 shrink stored entries, but a team that runs long enough still outgrows
one frame, because history, Updates and decisions only grow.

## Decision

1. **A message too large for one frame never costs the client its connection.**
   `LocalIpcServer::send` replaces it using `protocol::oversizedReplacement`:
   - A reply becomes a failure of the same request (same `correlation_id`), with error
     code `response_too_large` and the size in `bytes`.
   - An event keeps its scalar fields (ids, revision) and gains `truncated: true`;
     `team.changed` also keeps `team_id` and `revision`. The client reads the state
     again with a request.
   - A client is still dropped when more than `maximumQueuedBytes` is already waiting,
     meaning it stopped reading.
   - Feature: `transport.oversized/1`.

2. **Every team message has a size that follows the team, not its age.** Feature:
   `teams.summary/1`.
   - **Directory form**, used by `team.list` and `team.changed`: the team and its members,
     `tasks_total`, `held_count`, and `directory: true`, with no tasks and no decisions.
     Its size depends only on the members, so the sidebar always receives every team.
   - **Summary**, used by `team.get` and by the answers to team requests that change
     state:
     - every task without its history, listing at most 20 outside paths plus
       `outside_lane_total`;
     - the open decisions and the 20 newest answered ones, plus `decisions_total`;
     - `held_count` instead of the held messages.
   - **Updates:** `team.get` sends the newest 50 and `updates_has_more`.
     `team.updates.list {team_id, before_id, limit}` reads older ones, at most 200 per
     page, newest first, and returns `has_more`.
   - **History:** `team.task.get {history_limit, history_before}` returns one page of the
     task's history, oldest first. The page holds the newest `history_limit` entries
     (default 50, at most 200) before index `history_before`, plus `history_total` and
     `history_start`. Answers to task changes and the agents' `team_tasks` tool return
     the newest page.
   - **Decisions:** `team.decision.list {state, before_id, limit}` is paged the same way.
   - The stored document keeps everything; only what is sent is cut.

3. **The desktop reads the rest when it needs it.**
   - When a directory-form team arrives, `TeamDirectory` keeps the tasks and decisions
     the page last read for that team. The page's next `team.get` replaces them.
   - A truncated `team.changed` makes it read `team.list` again.
   - The team page shows **Show older** for Updates.
   - The task dialog reads its history with `team.task.get` and shows **Show earlier**.
     It falls back to a history embedded in the team for an older daemon.

4. **The sidebar never scatters a team.** When the daemon goes away, `TeamDirectory`
   keeps the teams it last read and marks them stale. The sidebar keeps grouping by them
   while requests fail, and the list is read again once the daemon is back. A daemon
   that comes back without `teams/1` clears them.

## Consequences

- One heavy team cannot break the connection, the sidebar or the other teams. At worst,
  a request too large to answer fails on its own and the page says so.
- The worst case is covered by a test (`TeamPayloadTest`). The team has 200 tasks, each
  with 40 history entries of the largest bounded size and 200 outside paths, plus 300
  decisions, 60 held messages and 400 Updates; the stored document is over 10 MB. Its
  directory form stays under 16 KB, its summary under 1 MiB, and every page under
  1 MiB.
- A summary still grows with the number of tasks and the length of their descriptions.
  A team with thousands of tasks would need tasks paged as well. If that ever happens,
  layer 1 turns the failure into an error on the team page; it does not drop the
  connection.

## Alternatives considered

- **Raising the frame limit.** This only moves the cliff, and a larger frame costs memory
  for every client.
- **Fragmenting large replies, like `transcript.large_event/1`.** It would deliver 8 MB
  that no screen shows. The page needs a summary and pages of detail.
- **Bounds on stored entries alone (#545).** They are kept as defence in depth, but they
  do not stop history, Updates and decisions from growing.
