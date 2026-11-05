# Chat pagination

Status: bounded local daemon windows, active-window eviction and block-virtualized
pathological assistant items implemented.

## Local daemon window v1

Desktop discovers session metadata without replaying every conversation when the
daemon advertises `transcript_window_version: 1`. Only the selected session loads
its newest three turns, additionally bounded to 128 events and 512 KiB of page
data. Older pages use an exclusive event cursor, including within a giant turn.
Snapshot cursor plus projection revision fence compaction races; an invalidated
page retains the reading window and offers an explicit **Reload latest** action.
See [ADR 0005](decisions/0005-bounded-local-transcript-windows.md) for exact wire limits.

The first window publishes one daemon page, without waiting for a multi-page
batch. Read-ahead projects and measures only the adjacent page at each loaded
edge, using the same adaptive turn limit as scrolling. It resumes when a window
publication moves that edge, rather than walking through the whole conversation
while it is idle. A later load reuses body heights when key, text, Markdown mode,
width and font still match. Explicit continuation loads retain five-page batching
and viewport buffer filling. Switching conversations fences pending projection,
measurement publication and read-ahead callbacks as well as transport callbacks.

History is projected without replaying lifecycle or historical approvals. Current
state and pending approvals are separate. Live events newer than the initial
snapshot are briefly buffered with count/byte limits; overflow fails visibly
instead of silently dropping content. Inactive sessions keep lifecycle/approval
state and mark their retained history as needing catch-up on content changes.
On selection, forward pages merge from the last delivered transcript cursor (or
the newer edge of a trimmed window) to a fresh snapshot. Its head is read without
publishing over the reader or releasing the live buffer, since ordinary item
completions can compact deltas and advance the projection revision. Already delivered live
deltas are not read again. Live events arriving during catch-up stay buffered
until the missing pages are installed, then current approvals and live delivery
resume. Switching away cancels publication and retains the last installed edge,
including the cursor of events buffered during that canceled load.

Catch-up keeps the reader's rows within the temporary scrolling source limits.
A page error or buffer overflow retains the window and offers a retry; reaching
the source limit offers an explicit reload. A daemon without forward paging, a
missing usable cursor, or a projection fence that keeps changing after three
retries requires a fresh newest
window. The retained rows remain visible until that replacement is ready.

An already projected assistant row continues accepting live deltas after a
running conversation is opened, even though history projection does not replay
its original item-start notification. New unclassified items still require a
public assistant phase. Presentation updates address canonical source indexes;
their bounds use the source message count, since collapsed activity groups and
split Markdown blocks make the displayed row count differ.

Older history is requested on upward user scrolling near the top, with the manual
button retained. Automatic loading is limited to one page per scroll gesture.
Wheel/touchpad events less than 400 ms apart share the same gesture, including
momentum; a new drag starts a new gesture. Only new input can re-arm the gate:
page completion, prepend/anchor geometry changes, and the quiet timer itself
never request another page. Input during an in-flight page is consumed rather
than queued for its completion. The explicit button remains available for retry
or loading another page without scrolling.
Incomplete assistant prefixes are explicitly marked and rendered
as plain text until earlier context or authoritative final text arrives. Prefix
merges preserve stable keys and carry a presentation-only UTF-16 prefix offset.
Unambiguous anchor repair is bounded; arbitrary Markdown source-to-render mapping
is not claimed.

Selected daemon history has a hard 300-message bound and a 16 MiB UTF-8 source
target. Paging older retains the historical edge and evicts complete newer
turns, except while a turn runs: the live turn is appended after those rows, so
the page grows the window instead and the idle trim then bounds it around the
reader. A pathological turn larger than 300 messages is cut at a
message boundary to preserve the hard bound. Jump to latest refetches a fresh
fenced window from the durable daemon projection. Eviction never deletes local
journal data. One indivisible message may exceed the byte target; its assistant
body is still block-virtualized.

Once scrolling has been idle for 1.5 s the window is trimmed around the reader.
The view reports the row being read whenever its reading anchor changes, and not
only when a scroll stops. That covers Latest, find and keyboard navigation too,
and the report is empty while following the tail. Block keys are reduced to their
source message. When the reported message is not in the window, the trim is
skipped rather than centred on the newest rows. While a turn runs, rows newer than
the reader are never evicted, because that is where live events are appended. The
count bounds each edge as above. The 16 MiB byte target then evicts whole turns
from whichever edge holds more bytes, but never the reader's own turn and, while
a turn runs, never the newer edge. When the protected rows alone exceed the
target, the window stays over it. Applied as a plain edge trim, the byte target
once evicted the newest rows from under a reader following the tail, and later,
with the reader in the older half of a running conversation, the live tail and
the rows being read. If a live row still arrives while the window ends before the newest turns,
the latest window is loaded instead of appending the row after a gap.

A reading anchor whose row is no longer laid out moves to the row that now shows
its message. That can be the group row when an older page makes the anchored
leader a collapsed member, or the nearest block when an answer is re-chunked. The
anchor is dropped only after the model has settled with the message gone.

An older page that takes over a group is reported as a removal and then an
insertion, and the geometry is published after each. Between the two, the view's
row count still includes the removed row. The reading row is therefore bounded by
the geometry's own row count. Otherwise a reader near the bottom could anchor to
the "row" just past the last one, which has no key. The insertion would then have
nothing to hold, and the reader would jump by the height of the whole page.

A giant (512K+) assistant row that is re-rendering on a worker does not reset the
model when rows are appended, prepended or the window is replaced. Pending worker
renders are rescheduled by key. Only a whole-window rebuild in flight still
resets.

When the loaded rows do not fill the viewport, idle prefetch loads older pages
even while following the tail, one page per idle pass, until the viewport is
full or there is no older history. Rows that short cannot be scrolled, so no
reader input would otherwise reach the history above them. A newest page made
mostly of a compaction and collapsed activity would otherwise show as a
near-empty transcript.

The shared transcript view lays out again whenever its geometry binding changes,
not only when its window model does. A warm (cached) conversation publishes no
geometry change of its own, and the two bindings re-evaluate in no set order. The
view could keep the previous conversation's extent and park the tail far below
the rows, which showed as a blank transcript.

Leaving a conversation while one of its older or newer pages is loading drops
that page but keeps the loaded daemon window and its cursors, because a cursor
advances only when its page is applied. Returning shows the same window at the
saved reading position and paging continues from there. Previously the session
was marked unloaded and reopened at its newest turns, which dropped the
reader's anchor. Only a canceled attach or first window is still reloaded.

A layout-fingerprint change (width, font, zoom) discards measurements but keeps
the old heights as estimates until rows are measured again. Unrealized rows no
longer collapse to the provisional height on every resize.

Inactive session data and warm view models each have a soft eight-conversation /
16 MiB **estimated text** budget. Active views, loading/copying sessions and pending
questions are protected. Evicting a warm view also releases its message model;
switching back reconstructs it from session data or a bounded daemon window.
These are not heap/RSS limits: nested payloads, layout/GPU allocations and active
history are not fully budgeted.

Cached offscreen text does not reparse on every body/media revision. The current
reading anchor can also pin the displayed source of a live assistant message
while tail-following is off, including through completion. The canonical model
continues receiving all content. Latest or leaving that row releases the pin;
explicit history-prefix loading is exempt. This prevents a late Markdown reference
or fence closure from reflowing the text under a stationary reader. A row with
selected text likewise holds its rendered document until the selection is cleared,
so a streaming answer can be selected and copied.

The mobile/cloud `session.transcript_page` contract is unchanged. An older daemon
without the advertised local capability retains the compatibility replay behavior
below; upgrading the matching daemon is necessary to get bounded cold init.

Mobile and remote clients use the canonical daemon transcript page rather than Codex-specific
`thread/turns/list`: three newest turns on first render, then three older turns per upward page. The
response includes a snapshot cursor so live replay starts after the exact history snapshot.

## Shared behavior and legacy/direct-Codex compatibility

- Resume attaches with `excludeTurns: true`, then requests 20 recent turns using
  `thread/turns/list`, descending order and `itemsView: full`.
- Older-daemon resume reconstructs the transcript from compact replay pages.
  Completed assistant items use their final `item.completed` payload instead of
  replaying every streaming delta; incomplete items retain their deltas. The
  daemon materializes this compact projection as events arrive, avoiding a full
  journal scan for every page. Pages are limited by count and encoded bytes and
  yield between UI updates so restore does not monopolize the GUI thread.
- In that older-daemon compatibility path, the selected conversation is restored first. Other cold conversations continue
  in the background; selecting one reprioritizes its replay and shows a loading
  state only over the chat pane, leaving navigation available.
- Load older messages explicitly from the top boundary, with loading and retry
  states. One page request per conversation is allowed at a time. Callbacks keep
  their session identity; background loads update that conversation's cache only.
- Pages are reversed into chronological order and overlapping turns are skipped.
  Live turns already owned by the session take precedence over page snapshots.
  Inserting a page adjusts live item indexes without resetting the transcript.
- AgentMessageModel uses row insertions and emits only changed data roles. Each
  conversation keeps its own model, so unchanged warm switches do not reset the
  model. Message keys survive updates and prepends.
- Per-conversation models are retained in an application-wide LRU with a soft
  cap of eight and five-minute inactive expiry. The QML viewport is shared, so
  switching models rebuilds delegates while restoring the saved reading position.
  `TranscriptWindowModel` serializes range changes raised by delegate signals:
  an insert/remove transaction completes before the latest requested range is
  applied. This prevents nested diffs from invalidating row indexes during a
  history reset. Showing an already visible Agent Workspace at the current
  workspace path does not reactivate the previous conversation before the sidebar
  selects its target. Returning from retained history for a missing worktree
  restores the view's path before publishing the current workspace's conversation.
- Scroll restoration uses a message key plus a rendered text position and its
  pixel offset from the viewport top. This keeps the first visible text line
  anchored when streaming Markdown tables or a width change reflows earlier
  content inside the same message. A short in-memory text sample validates the
  saved position; replaced text and non-text cards fall back to the message's
  pixel offset. Anchors are captured from the displayed view's own model.
  Cached delegates can finish geometry during ListView polish, after callLater
  restoration. A small
  QQuickItem schedules compensation after that layout pass, before scene sync.
  Changing contentY inside a delegate's yChanged reenters ListView layout and can
  overlap rows, so yChanged only schedules the post-layout correction.
  Restoration preserves fractional offsets and rechecks the result once in the
  same callback: ListView's synchronous extent fixup can round the first write
  to a whole pixel. Deferring that correction can expose a shifted native frame.
- Wheel-up immediately disables tail following, including movements within the
  former 28-pixel bottom threshold. Scrolling down to the end or Jump to latest
  reenables it. Scrollbar dragging and conversation changes also reset gestures.
- Copy conversation (footer button or the transcript's context menu) fetches unloaded pages
  separately, with cancellation and failure handling, without adding them to the
  visible model. It copies the server-retained transcript, not original history
  removed by compaction.
- Unsupported paged RPCs fall back to `thread/read` with `includeTurns: true`.
  This compatibility path cannot provide the memory benefit of pagination.

Already loaded pages and per-conversation message models remain cached. The
Desktop coalesces Codex streaming presentation snapshots at 100 ms, reducing
whole-document Markdown parse/layout and scroll restoration calls. Bodies over
32 Ki characters use 250 ms and over 256 Ki characters use 500 ms. The timer is
not restarted by incoming deltas, so continuous input still makes progress.
Canonical session state receives every delta immediately; only the rendered view
lags by the batching interval. Completion/replacement supersedes pending prefixes.
Stop/error, append, prepend and scope switches flush pending presentation updates
before changing state or row indices. Multiple pending rows share one scroll
preservation transaction. Final text is never truncated.

Completed assistant bodies above 16 Ki characters are split at safe top-level
Markdown boundaries into stable rows of the outer ListView. Segmentation runs on
the shared thread pool with generation fencing; the retained edge is published
first and older placeholder rows arrive in bounded batches. Each rich text
document is therefore bounded and an 8 MiB response remains virtualized. Complete
blocks retain Markdown, links, images and Mermaid. A pathological fragment with
no safe boundary, such as a huge open fence, is rendered as bounded plain text.
The authoritative source stays exact for conversation copy/export. Selection is
currently per rendered block rather than continuous across block boundaries.

An individual completed assistant/tool payload may exceed the event and frame
caps without being discarded. The daemon stores up to 32 MiB atomically as
96 KiB cloud chunks plus a terminal manifest, while Desktop resolves the
manifest through 128 KiB local fragment pages and publishes it only after full
SHA-256 and schema validation. See
[ADR 0006](decisions/0006-bounded-large-transcript-events.md). The resulting
source is handed to the bounded block renderer; lossless transport and bounded
presentation remain separate validated layers.

### Regression checks

- Fake server tests cover the 20-turn request contract, chronological ordering,
  overlapping pages, retry, duplicate load requests, background loads, live item
  index shifts, full copy/cancel, and fallback to a legacy full-history server.
- QAbstractItemModelTester validates row insertions; a body delta emits only the
  changed role, not a model reset.
- Native Qt/Metal tests check 1000- and 600-pixel windows, key/offset preservation,
  wheel-up near the bottom, conversation switching, and loading/retry controls.
- Reflow tests keep the first visible table row anchored at full and partial
  line offsets, resize the transcript, and verify Jump to latest resumes tail
  following. Continuous mouse and synthesized touchpad events run while new
  messages and deltas arrive. Touchpad fixtures include a scroll-begin phase
  and event timestamps so Qt exercises its pixel-scrolling path.
- Optional pixel comparisons: `KETPLUS_PAGINATION_CAPTURE=/tmp/pages` captures
  20 frames per width while growing a cached message above the reader after a
  prepend; `KETPLUS_CHAT_CAPTURE=/tmp/stream` checks 36 frames in each of four
  long-message streaming cases. RGB differences up to 2 allow glyph rounding.
  `KETPLUS_REFLOW_CAPTURE=/tmp/reflow` saves before/after images for the table
  cases in `streamingTableKeepsReadingParagraph`.

## Original investigation

The following describes the code before the pagination implementation.

- `ensureThread` and `hydrateConversation` use `thread/resume` with
  `excludeTurns: false`. `hydrateHistory` converts all returned history to messages.
- `messages()` constructs a QVariantList; QML clears/refills its ListModel on
  conversation switches. This work scales with the amount of loaded history.
- ListView already virtualizes delegates, reuses items, and caches one viewport.
  It does NOT instantiate every message delegate simultaneously.
- Each assistant message is one Markdown TextEdit. A giant message remains a
  giant QTextDocument even when its containing ListView is virtualized.
- Streaming replaces the accumulated body role on each delta. Markdown parsing,
  layout, and paragraph formatting can revisit the entire growing message.
- Existing scroll anchors use numeric indexes, which prepend/eviction invalidates.

These paths establish potential costs, not proof that pagination explains all
CPU usage. Previous process sampling also found animations and background index scans.

## Server support

The installed Codex 0.153.4 generated schema exposes `thread/turns/list`:

```json
{"threadId":"...","cursor":null,"limit":20,"sortDirection":"desc","itemsView":"full"}
```

Responses contain `data`, optional `nextCursor`, and optional `backwardsCursor`.
Treat cursors as opaque. Explicitly request full items: the default is summary.
The limit counts turns, not messages or bytes; one turn can still be very large.

Resume with `excludeTurns: true` to attach without loading all history; fetch
history separately and merge with live events. Context compaction is not paging:
it changes model context, whereas paging only changes what the client loads.

## Current giant-message renderer

The production model now separates canonical source items from bounded render
rows. Assistant bodies at or above 512 KiB are segmented on a worker into stable
Markdown-aware blocks targeting 12 KiB and capped at 16 KiB. The newest block is
published first, then older blocks are prepended in batches of 32 so the first
usable viewport does not wait for complete materialization. Unsafe fragments
(for example an open fence without a safe boundary) render literally instead of
silently changing Markdown meaning.

Streaming materialization is bounded to one active worker and one replaceable
pending batch. The pending batch keeps the newest source revision per row, so
interleaved giant messages remain lossless while 50–100 Hz source deltas cannot
create an unbounded worker queue. Terminal text is flushed authoritatively and
the model does not reset during these row updates. Viewport delegates retain
estimated geometry and only materialize rich Markdown inside the active render
margin.

Copy/export reads the canonical source item rather than concatenating visible
blocks, so it is complete even while older render blocks are still publishing.
Selection remains block-local, unsafe partial Markdown is literal, and native
Metal frame/RSS evidence is still required; offscreen timings are not a visual
or physical-device approval.

## Historical design and follow-up work

1. Replace the copied QML ListModel with an incremental QAbstractListModel. Retain
   stable `threadId/turnId/itemId` keys from both live events and history. Reconcile
   temporary local user-message IDs when the server echoes those messages.
2. Start with 20 recent turns (a benchmark starting point), reverse the descending
   page into chronological display order, and expose a load-older boundary action
   with loading/error/retry states. Prepend with beginInsertRows, never model reset.
3. Keep one request in flight per direction/conversation. Tag requests with a
   generation; discard stale responses after switching, reconnecting, or clearing.
4. Deduplicate page/live overlap by stable IDs. Preserve live unfinished bodies
   over stale page snapshots and treat completion events as authoritative.
5. Add an LRU byte budget for inactive conversations and bound active history
   using both older/newer boundary controls and backwardsCursor-aware loads.

Initial active-window budget to benchmark: 100 turns or 8 MiB of source text.
This is a soft limit: retain the reading anchor, selection, and active turn.
A giant single message uses the block renderer described above, never silent
truncation.

### Scroll preservation

Capture `{itemKey, pixelOffset, followTail}` before prepend/eviction. Find that
same item afterward and restore its offset without animation. Use the same stable
key for conversation restoration. Never reenable followTail because new text
arrived. Show jump-to-latest/new-message state while the user reads older content.
Avoid repeated forceLayout calls during streaming. Do not evict selected text.

### Remaining long-message work

Measure whole-message layout separately. Evaluate coalescing deltas every 33-50 ms,
flushing immediately on completion and preserving order when changing threads.
Pagination alone will not fix a single huge Markdown document.

The current safe-boundary block renderer closes the UI-thread and unbounded-job
findings. Cross-block native selection and a source-to-render position map for
arbitrary Markdown remain follow-up work. Copy/export, links, images and Mermaid
retain their canonical source behavior; incomplete unsafe blocks stay literal.

## Compatibility and validation

- On unsupported paged RPCs, explicitly fall back to the current history loader.
- Conversation-wide copy/export must fetch unloaded pages on demand with cancel
  and error handling; it must not silently copy just the visible page. Future
  full-history search needs the same treatment.
- Keep compacting/compacted lifecycle rows in the normal model. Verify what the
  server retains after compaction; do not promise unavailable original history.
- Test duplicate events, page/live races, stale callbacks, retry, empty pages,
  reconnect, cancellation, images, and changing conversations during page loads.
- Benchmark 1,000/10,000 messages AND one giant response. Measure first-content
  latency, GUI stalls, instantiated delegates/documents, RSS after repeated
  switches, and streaming/idle CPU with identical fixtures and build settings.
- Native Metal screenshots and scroll measurements must verify stable reading
  positions after prepend/eviction. Offscreen tests alone are insufficient.

Implemented delivery order was incremental model + stable anchors + backend
paging, then cache budgets/delta batching, then bounded block-level Markdown.

Reference: [Codex App Server](https://learn.chatgpt.com/docs/app-server). Concrete
pagination fields were checked against locally generated protocol schemas.
