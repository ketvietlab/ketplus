# ADR 0022: daemon activity summaries with on-demand details

Status: accepted for the local daemon/Desktop transcript in 0.1.30.

## Decision

The journal remains the canonical history authority. A Desktop opts in with
`transcript_activity_groups: true` in its authenticated minor-1 `daemon.hello`;
the daemon advertises `transcript.activity_groups/1`. Other clients retain the
existing wire events and client-side grouping. The relay hot replica and mobile
protocol are unchanged; this capability is not a claim of a mobile cutover.

For an opted-in connection, live `session.event` and the ordinary local replay,
transcript-page and transcript-window responses replace eligible tool events with
`activity.group.summary`. A summary carries `group_id`, `item_id`, `count`,
`families`, `running_count`, `failed_count`, `revision` and `last_change_cursor`.
It contains no command, arguments, stdout or tool-result body. Every retained
output/status change advances the watermark even when the count stays equal.
Summary metadata describes the current group head; the envelope cursor remains
the original durable event cursor for existing replay/window fencing.

Groups contain consecutive commands, file changes, web searches and text-only
KetPlus browser steps in one turn. A first eligible tool already has a stable
group identity based on its first cursor. A late completion keeps that identity;
it does not join a newer group across an intervening message or approval.
Unknown MCP tools, assistant/reasoning items, approvals, screenshots and visual
comparisons break the run. `browser_capture`, `visual_compare` and browser
observations requesting `includeScreenshot` stay outside even while running.

The incremental metadata index is updated in the same SQLite transaction as the
canonical event. Per-family counters make output-only notifications independent
of the size of the group. On an old journal the index streams retained history
once; subsequent startup seeks only an unindexed suffix. Fork copies the index
with its history and deletion removes it with the session generation.

## Opening and live changes

`session.activity_group_page` is an authenticated session/generation-scoped read:

```json
{"group_id":"42","after_cursor":0,"event_limit":128}
```

The initial read chooses a consistent `snapshot_cursor` and
`projection_revision`. The result returns `events`, `next_cursor`, `has_more`,
the group/session identities and the fence. Each page is limited to 128 events
and 512 KiB; existing bounded large-event references are resolved on demand.
Continuation supplies the same snapshot/revision with its exclusive cursor.
An append after the snapshot is fetched in the next delta read. If canonical
compaction invalidates that fence the daemon returns `snapshot_changed`; Desktop
clears the partial detail snapshot and restarts. Canonical item IDs and the normal
history merger upsert completed/status changes and concatenate only output deltas.

Desktop keeps one request in flight per open group. A summary only advances its
desired watermark; after the current page returns it fetches the outstanding
suffix. Closing releases details and advances a generation fence. Reopening waits
for any old request to finish before reading a fresh snapshot. A late reply cannot
reopen or populate a closed group. Disconnection is visible with an explicit
Retry control; an expanded group is also resumed when its transcript is restored.
The existing transcript mutation anchor and follow-tail policy own outer scrolling.

The normal transcript retains one summary row while details render inside that
expanded row. Collapsed groups retain no detail list. Full conversation copy is an
explicit independent raw read (`raw_activity_details: true`), preserving exports
without hydrating every command during ordinary transcript navigation.

## Delivery and limits

The 0.1.30 daemon is required for summary transport; a new Desktop attached to an
older daemon continues its existing local grouping. Packaging uses the staging
Control URL. This change does not deploy a Linux server, install a phone app or
replace/restart the user's active app or daemon.

The staging delivery uses `KETPLUS_AUTO_UPGRADE_DAEMON=OFF` so replacing Desktop
can retain a compatible older daemon. The ordinary build default remains `ON`.
No running daemon is upgraded as part of producing this artifact.

## Validation

Implementation tree `508e83a575be917499858208d0032c52928d701a` was checked
on macOS 26.2 ARM64, Qt 6.11.1, Release with
`KETPLUS_AUTO_UPGRADE_DAEMON=OFF`. The full private suite passed 66/66 targets;
a post-review selection of seven activity, upgrade-policy and transcript-window
targets also passed. These use synthetic journals/providers and isolated IPC,
not live provider calls or the user's daemon.

The production Qt group was rendered through the authenticated IPC fixture at
1100 × 900 and 520 × 900 logical pixels. The fixture exercised restore, expand,
a new step while expanded, collapse and full-conversation export. Loader tests
cover single-flight/coalescing, late replies, snapshot invalidation and explicit
retry. Journal tests cover output-only updates, evidence exclusion, old-journal
backfill, terminal status, fork and deletion.

The static Atlas fixture was exercised in Chromium at 1440 × 1000 in light mode:
loading, error, Retry and the expanded updated state; no JavaScript errors were
reported. KetAtlas 0.6.1 validation and strict audit passed. The existing mock
is a prototype, not a second implementation of the native Qt networking.

A new Linux runtime deployment, mobile-device installation, real relay pairing,
Windows/Intel builds and signed/notarized release acceptance were not exercised
by this change. The relay/mobile protocol remains unchanged.
