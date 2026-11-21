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
It contains no command, arguments, stdout or tool-result body.

Since 2026-11-13 (T-91) a summary also carries `latest`, so a running group's heading can
name the step it is on as that step's own row words it ("Checking Git status", "Using
github · list_issues"). `latest` is `{item, completed}`: the newest state of the group's last
member, the one that joined it last and has not left it. Its `item` drops `aggregatedOutput`,
`output`, `formattedOutput`, `result`, `contentItems`, `content`, `stdout`, `stderr`, `diff` and
`unified_diff`, keeps at most 512 characters of any string and 20 entries of any list, and is
cut 6 levels deep.

This is an accepted exception to the rule above (leader decision, 2026-11-13), and it holds
only while all of the following stay true:

- `latest` is a single step's command or tool label, with the arguments that label needs,
  truncated as above. It never carries output, a result body, stdout, stderr or a diff.
- It travels over local IPC only: an authenticated `daemon.hello` on the daemon's local socket
  that opted in with `transcript_activity_groups`. The relay and mobile protocol do not carry
  summaries, and an SSH machine's `DaemonClient` (`SshMachineBackend`) does not opt in, so
  neither Relay nor SSH sends `latest` to another device.
- Neither the daemon nor the Desktop logs it.

A Desktop that predates the field ignores it, and a daemon that predates it sends none, so the
heading keeps "<summary> · Running".

Since 2026-11-09 a summary also carries `files`, so the Desktop can show a turn's changed-files
card without opening its groups. `files` lists the files the group's completed `fileChange`
items changed: `path`, `destination` (a move's target, otherwise empty), `operation`
(`Created`, `Edited`, `Deleted` or `Moved`), `added`, `removed` and `known`. A file is merged
by path in the order first changed, ordered by each item's first journal cursor and then by
its changes, so a completion delivered again or a fork lists the same files in the same
order. Counts are known only when every change to it was counted. A later move or delete
decides a path's operation; a created file stays `Created` through later edits; a file
created and then deleted within the group is left out; one deleted and created again is
`Edited`. At most 50 files are listed; `files_total` counts them all. Failed changes are
left out, and no diff text is included. The rows live in `activity_files`, written in the
same transaction as the canonical event, and are copied by fork and removed with the session
generation. A journal indexed before this table existed lists its groups' files once, from
the completion events it retained, streamed one at a time; the table is created in that
backfill's transaction, so an interrupted backfill runs again on the next open. Clients that predate the field ignore it. Every retained
output/status change advances the watermark even when the count stays equal.
Summary metadata describes the current group head; the envelope cursor remains
the original durable event cursor for existing replay/window fencing.

Groups contain consecutive commands, file changes, web searches and text-only
KetPlus browser steps in one turn. A first eligible tool already has a stable
group identity based on its first cursor. A late completion keeps that identity;
it does not join a newer group across an intervening message or approval.
Assistant items, approvals, screenshots and visual comparisons break the run.

Since 2026-11-09 groups also take the steps a local Desktop group takes, so both
read the same. Families are:
- `command`, `edit` and `webSearch`;
- `read`, `codeSearch` and `fileSearch` for tool reads and searches, and `read`,
  `codeSearch` and `list` for Codex commands whose actions are all reads, searches
  or listings (`list:<n>` for a command that lists `n` folders, so a group counts
  its steps once and its folders in full);
- `webFetch`, `toolsLoaded`, `skill` and `taskStopped` for those text-only tool steps;
- `browser` for KetPlus browser steps;
- `tool:<server>` for any other MCP server's step (`ketplus_agents`, `node_repl`), with
  Claude's `mcp__server__tool` names read as that server;
- `subagent` for a spawned subagent and `subagent.step` for later messages and
  waits on one;
- `wait`, which includes Claude's `Monitor`;
- `thought` for reasoning items, which Desktop shows as "Thought · 14 s" without their
  text.

Tool calls without a server still break the run: some carry data a summary drops, and
Claude's `Write`/`Edit` would lose their changed files.

A family is fixed by the item's first event, before its result shows whether it returns
images, and images are evidence that Desktop shows outside groups. Since 2026-11-09 a
`tool:<server>` step therefore joins an open group but never opens one, so taking it out
again never leaves a group empty. One that completes with images in its result
(`result.content[].type == "image"`) leaves the group:
- its `activity_items` row keeps the group's cursor under the family `evicted`;
- its family's count, and what it counted as running or failed, come off the group's
  summary. An emptied family keeps its row at count 0 and is not listed; its cursor holds
  the group's watermark, which never goes back;
- later starts or completions of that item change neither the counts nor the membership;
- the group's page no longer lists it, and the run ends there, as evidence ends a local
  group;
- summaries carry `membership_cursor`, the cursor at which a step last left the group. A
  client holding the group's details drops them and reads the group again when it
  advances, because loaded details only ever grow.

A step that completes with images without having started never joins. Its events project
as themselves. The completion also carries the group's new summary as
`left_activity_group`, so a live client updates that group's line from the same event. A
second event cannot do this: it would repeat the cursor, which live clients drop and
history pages reject. History needs nothing more, because summaries are computed when they
are read.

Summary `families` are listed in the order the group first used them. Groups indexed earlier keep their families: their
`search` covers web, code and file searches alike, so Desktop words it as "searched",
and their other tool calls stay outside. `browser_capture`, `visual_compare` and browser
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
