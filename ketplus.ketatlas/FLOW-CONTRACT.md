# Flow corrections and implementation contract

This is a synthetic KetAtlas prototype, not a native feature release. It resolves the
design-review P1/P2 findings and adds Archive/Delete and Settings-based restore.

## Identity and continuity

- Repository + stable worktree identity owns a conversation registry. Display titles and
  branches must not become production identity keys. Fixtures use explicit scope IDs.
- New conversations and newly created Atlases receive distinct IDs. Worktree changes select
  that worktree's previous conversation. Background turns keep their owning scope.
- Text drafts, attachment metadata, requested flow and reading position survive navigation.
  Input is cleared only after acknowledgment; credentials and attachment bytes never enter
  URLs. URL-fragment snapshots are a prototype-only state transport, not the native storage design.
- A malformed or oversized version-1 fragment is discarded before UI rendering. The mock
  validates scope and conversation shape, route ownership and known provider IDs; URL
  prototype property names cannot select undeclared catalogs. This guard does not replace
  native persistence or authorization.
- The pending synthetic operation carries its original scope/conversation. Switching the
  visible screen must not apply its acknowledgment to a different session.
- The execution-worktree picker announces its pending switch and holds the dialog open
  until the simulated acknowledgment, so closing the picker cannot hide an in-flight change.

## Independent lifecycle states

Connection, folder availability, history loading and turn status are separate concepts.
Answer → submitting → working; Stop → stopping → interrupted; reconnect → reconcile.
Missing folders disable send and new terminals, but do not delete history or disable Stop.
Steering uses the existing turn, not a new request. Production requests need explicit
identity, epoch/generation fencing, idempotency and stale-response rejection.

The model uses a single pending synthetic operation and fixed data. It does not emulate
the daemon scheduler. Production parallel scheduling and bounded replay remain native tasks.

## Transcript reading and performance

Returning to a cached conversation retains its measured row sizes and reading position. Recreating an offscreen text/image row must not replace its known height with a temporary placeholder; mixed portrait/landscape attachments keep their aspect ratios. Only a completed layout at the current width can replace that measurement.

Loading another history page is not conversation restoration. Keep the reader’s current anchor and any open image preview while a page arrives; only switching conversation closes scoped previews. Motion during the request belongs to the reader and must not be rewound to the request-time position.

The `history-journey` demonstrates loading/error, partial turn, earlier history requiring
the Mac, new-content indication and Latest. Earlier fixtures prepend with stable item IDs;
the demo has only nine earlier entries, explicitly labeled sample exhaustion, not a native
retention limit. Native initial-three-turn loading also needs byte/item limits and within-turn
continuation. Render virtualization must be paired with a bounded data/parse/media cache.

The visible turn is split into the user's request, structured activity, and the final answer.
Intermediate reasoning and commentary prose are intentionally absent. Actual actions are never
hidden: file reads/edits show their target, command rows preserve the exact command, and dedicated
rows cover search, browser navigation, app/tool use, and image review. Command output is initially collapsed in this mock and opens on its exact command row.
The displayed output is a deterministic local fixture.

Adjacent activities may be grouped for scanning, but opening a group must reveal every original
item in order. `Viewed 4 images` expands to four independently openable local previews. A recorded
website retains its safe URL detail. Unknown future activity types render a generic expandable row
with sanitized metadata instead of being silently dropped. Rich preview loading is local to the
selected detail. Actual native Markdown, code, diff, Mermaid and media rendering, profiling and
frame/memory budgets are not implemented here.

Before native acceptance, benchmark 1k/10k messages, a multi-MiB turn, streaming while reading
above, prepend, font/width changes, Latest and repeated switching across 20 conversations.
Verify no item loss/duplication, anchor drift or unbounded memory growth. DOM state coverage
is not a performance benchmark or a substitute for real-device iOS measurements.

## Connections

- Signed out: one Sign in button, then username/password dialog. Demo accepts nonempty
  input only; it never stores credentials, calls a service or grants real access.
- Every paired phone supports Details and Revoke. Remove it only after acknowledgment;
  failed revocation leaves the device listed. Pairing close/Decline cancels its ticket.
- Cancellation failure is not reported as revoked: show pending expiry/retry. Closing a
  confirmation invalidates the old operation so it cannot later add a device.
- Sign-out, token expiry, offline and pairing expiry are different states. Sign-out is not
  a substitute for revoking an untrusted device. Local Agent sessions remain running.
- Full history lives on the Mac. Recent content is synchronized through the cloud with a
  maximum 24-hour TTL; paired clients may cache it. Do not imply history never leaves the Mac.

## Desktop to headless Linux

- **Connections** owns both directions: phones access this Mac, while this desktop
  uses trusted headless Linux machines. Add chooses the type. The Linux setup,
  identity check, host details and recovery render within the Linux machines
  section beside the phone list. Sign-in returns to Connections. The sidebar has
  one Connections entry and a Machine select for This Mac or a trusted Linux
  machine. It shows only that machine's repositories, ordered by newest
  conversation activity. In Agent Workspace, switching machines opens its
  newest conversation without stopping the previous machine's turns. Choosing
  a machine or opening a repo does not change recency. An offline remote
  repository cannot start a new conversation.
- The mock assumes the Linux daemon already runs and makes an authenticated
  outbound relay connection, consistent with `docs/product-architecture.md`.
  Pairing uses a short-lived code and a six-digit identity comparison on both
  machines. No inbound port, SSH password or live remote command is simulated.
- A malformed code stays on the form with a focused error. Expired and unavailable
  codes require a fresh attempt. Identity mismatch never adds trust. The Trust
  button is disabled until the user checks the comparison box.
- A machine appears only after simulated connection acknowledgment. Disconnect
  preserves trust; reconnect can fail without removing it. Forget needs a centered
  confirmation, retains the record while pending, and removes it only on
  acknowledgment. A failed removal keeps the record and offers retry.
- The submitted code is never written into `#demo=`, a query string, a log or a
  retained fixture. The host rows in `#demo=` are bounded and validated before
  rendering. All names and IDs are synthetic; permissions, relay behavior, token
  expiry and daemon enforcement need native implementation and acceptance tests.

## Atlas control and external viewer

- Every conversation header exposes the Atlas control. Its badge counts all
  managed viewers running across worktrees, so switching conversations or
  worktrees does not hide one. The right pane lists the selected worktree's
  Atlases and a compact Running in other worktrees section with Stop controls.
  Bundle ID and resolved worktree path, not title, identify the Atlas. The list supports empty, filter
  empty, starting, error, running and multiple-running states.
- Each bundle row offers Edit and Open in browser as separate actions. Edit
  opens the existing Atlas Builder map and Agent chat inside KétPlus for the
  selected bundle, without starting a viewer process. Back returns to the same
  conversation and Atlas list. Incomplete edits retain the last valid map;
  a missing bundle offers return to the list. The sidebar has no Atlas entry.
  An offline Linux host cannot open Edit until it reconnects.
- Selecting Open in browser starts a managed KetAtlas viewer outside KétPlus.
  Once running, Open browser returns to that viewer and Stop in the right pane
  terminates its process. The badge clears only after the stop acknowledgment.
  Closing the last external browser tab stops the viewer automatically and
  clears the badge; closing one of several tabs does not. Start failure offers
  Retry without inventing a running process. An offline Linux host disables
  opening or creating its Atlases while keeping the list readable.
- KetAtlas 0.6.1 supplies a managed CLI contract and browser-presence signal.
  Native KétPlus must own process IDs and asynchronous state, reconcile process
  exit and browser closure, and avoid treating a stale event from an earlier
  launch as the current viewer. The static mock models these states; it does not
  launch a CLI, control a browser window or prove native lifecycle behavior.
- Atlas creation begins as an Agent request in the current conversation. A
  generated bundle remains discoverable by its worktree. `@Flow` is preferred
  Agent context, not a sandbox; shared assets may affect several flows. Native
  code must validate a completed bundle and preserve the last valid revision
  while intermediate writes are incomplete. Stable bundle/flow identity,
  missing-bundle recovery, bounded viewer resources, native accessibility and
  visual/performance checks from the retired in-window screens remain open
  requirements for the external-viewer implementation.

## Archive, Delete and Restore

| Operation | Files / history | Running turns | Result |
| --- | --- | --- | --- |
| Archive conversation | Retained | Continue; visible in Archived | Hidden from main conversation list |
| Archive worktree | Retained; descendant conversations hidden | Continue; monitor/Stop in Archived | Hidden from main worktree list |
| Restore from Settings → Archived | Original ownership retained | Does not start/rerun a turn | Stay in Settings; restored item returns to its original list |
| Delete conversation | Remove KetPlus-owned record/history only after acknowledgment | Block until stopped | Not restorable from Archived; provider-owned history is separate |
| Delete worktree | Remove clean linked folder; keep branch; archive conversations | Block until all stopped | History stays bound to original missing folder |

Delete requires typed DELETE, never force-removes dirty files, and blocks the main worktree.
Recheck guards on acknowledgment as well as before submission. Failed operations leave data
unchanged. Restore of a deleted/missing folder is blocked until its original identity/path is
recovered; do not silently reattach conversations elsewhere. Archive and Delete have separate
confirmation copy; Archive is the reversible option.

Native implementation must define the archive/tombstone records and daemon/client sync
contract before wiring these buttons. Conversation deletion requires a scoped acknowledged
operation across KetPlus-owned journal/cache/cloud projections and reconnect tombstones;
this prototype does not promise provider deletion or cryptographic erasure. Worktree removal
belongs to the owning Git/workspace implementation, with identity/dirty/active checks and no
unrequested branch removal. These are explicit implementation dependencies, not existing APIs.

## Updates

Updates are offered only by a build that embeds a signed feed URL and its public
EdDSA key; otherwise Settings → Updates says so and never contacts a server. A
check never installs by itself: the user chooses Install and relaunch, and with
background download enabled still chooses when to restart. Download progress can
be cancelled without side effects. Skip this version suppresses only that version.
Restart to update relaunches the window process only. Agent turns belong to the
background daemon; an old daemon keeps serving its active turns and the new
daemon replaces it after they finish (ADR 0003 handshake). Later keeps an
**Update available** reminder in the sidebar until the user opens it. Scheduled
checks that find an update while KetPlus is not focused show the same reminder
instead of an alert (Sparkle gentle reminders).

## Verification scope

`tests/design/ketatlas-flows.test.cjs` and `tests/design/ketatlas-remote.test.cjs`
share a jsdom harness with local scripts, no external resources, and a synthetic clock.
They cover addressable states and model/DOM
interactions; it does not render pixels, run an actual browser, or change native data.

The current browser review and its limitations are recorded in REVIEW-2026-09.md.
Native acceptance remains separate in `atlas.progress.json` and `tasks/desktop-refactor/`.
Passing mock tests never completes a native acceptance check.


## September 2026 implemented-feature alignment

Codex, Claude and Cursor are now represented as supported daemon providers. Model,
effort and mode follow the selected conversation. KétRouter stays unavailable.
Active Codex/Claude accept text guidance; Cursor does not advertise unsupported
steering. Configuration and attachments are locked during active turns.

Context & limits is a non-modal popover above the lower toolbar's far-right ring.
Opening it neither navigates nor disables the workspace; Escape, Close or an outside
click dismisses it. Account limits appear here only, not in the sidebar. Missing
current-window data says Not reported; another provider's limits are never substituted.

New conversations choose an existing base worktree and optionally create a branch
before acknowledgment. Idle Codex/Claude can fork, preserving provider/model/worktree
without owning the source worktree or starting a turn. Cursor/active forks are blocked.
Success and failure acknowledge the original scope even if selection changes.

The mock keeps the established conservative worktree-deletion fixtures. Native
conversation deletion also offers optional force-removal of owned linked worktrees;
that additional destructive branch is not modeled in this review.

Demo snapshot JSON is transported in `#demo=…`, keeping it out of HTTP requests
and Referer headers while supporting opaque iframe navigation. Legacy `?demo=`
inputs remain readable. This is transport, not encryption or secret storage.

## Context menus

The source inventory, action order, target identity, capability gates and known
implementation differences are maintained in [CONTEXT-MENUS.md](CONTEXT-MENUS.md).
Right-clicking a background record must not select it or retarget actions to
the active conversation. A changed-file menu takes precedence over the transcript
fork menu. Menu commands recheck their target before dispatch.

A history page may finish after the reader moves. Native window trimming must keep the current reading row (not the request-time row); if both edges are trimmed, both paging cursors continue without gaps. No page completion may select the beginning of a long transcript.

## Markdown and CM terminal
Opening Markdown in the editor defaults to Preview; a floating Preview / Source control stays inside its content surface. The preview reading column is capped at 960 logical pixels, retaining its rendered document and scroll position across mode switches. Editing marks the preview dirty; switching back refreshes only a changed document. Source Control starts in Diff and offers Diff / Preview / Source with a read-only representation of the compared revision (index for staged changes). It must not substitute the working copy for staged content. Terminal consumes CM's terminal core/UI with search, selection, clear and restart actions; hiding it retains the shell.

The Markdown mode control floats at the horizontal center of the document viewport, about 100 px above its bottom edge.

Execution worktree picker: 24px outer padding, 12px section gaps, padded rows. One scrollable searchable list, no Previous/Next controls. Fetch more as the user approaches the list end. Selecting any row, including Current, changes only Files/Source Control context; it never sends session.worktree.switch and remains available while the agent runs. Loading history must settle or expose retry after a failed request; live events must not strand the loading indicator.

`questionMode=confirm` shows direct Có/Không actions; `question=2` covers free text.
Only explicit binary questions use confirmation; arbitrary prompts are not inferred.
