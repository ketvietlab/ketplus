# ADR 0015: Stable conversations with versioned execution contexts

Status: daemon, isolated relay and mock-reviewed desktop/iOS switch UI implemented; not deployed.
Date: 2026-09-30.

A conversation keeps its ID, provider thread, generation and transcript when its
default worktree changes. Worktree identity is not conversation identity. A turn
may still use tools outside its default directory; default context is not proof
of a resource's actual origin.

## First slice

The local authenticated IPC advertises `worktree_context_version: 1` in
`session.list`. This slice supports Codex only, between idle turns:

- `session.worktrees` takes the usual session ID and generation, refreshes the
  current repository's Git worktree list, and returns the active path,
  `context_revision`, `switch_supported`, available worktrees and context history.
- `session.worktree.switch` takes `worktree_path` and
  `expected_context_revision` in its payload. Paths are local IPC data, not cloud
  inputs. The target must be a canonical registered worktree root in the same
  Git common directory. No checkout, stash, merge, file copy or process kill runs.
- Pending turns, stopping and pending approvals reject switching with
  `session_busy`. Unsupported providers, foreign repositories, missing worktrees
  and stale revisions have explicit errors. Selecting the current worktree is
  a no-op.
- After the first switch, start/steer requires `expected_context_revision`.
  Unswitched conversations retain compatibility with existing clients. Inputs
  captured before a switch cannot silently execute in the new directory.
- The existing Codex thread is resumed with the selected cwd. Resume errors fail
  the turn, without silently creating a new thread. Each switched turn also gets
  an execution-context note; the visible user message is preserved.

SQLite adds `sessions.context_revision` and immutable `session_contexts`, keyed
by conversation, generation and context revision. There is no history backfill.
Existing sessions retain a default pointer but acquire recorded context only on a
new submission or switch, starting after the current cursor. Switch pointer, context history, normalized
`conversation.context_changed` event and successful idempotency result commit in
one transaction. A stale stored session cannot overwrite a later context. Since
2026-11-09 the event also names the target's `branch` and `worktree_name`, the folder's
name, so Desktop reads "Moved to worktree feat/x · The agent now works in name". The
path itself stays in `session_contexts` and never enters the event.

New user-message and queued-turn events pin their default `context_revision` at
submission. Codex provider events with a known turn ID reuse that turn's explicit
context; after restart the mapping is read from newly recorded `turn.started`
events, never inferred from older events or current selection. Missing mappings
produce `provenance: unknown`. Other provider adapters remain outside this slice.
File-change events receive `resource_refs` only for existing absolute paths whose
canonical path is inside the recorded root; each ref has `context_revision` and
relative `path`. Relative, deleted, symlink-outside and unproven tool paths remain
unknown. A default turn context alone is not a resource origin.

## Remote compatibility boundary

Implementation keeps the new behavior in `SessionHostWorktrees.cpp` and
`LocalJournalContexts.cpp`. Small hooks remain in the existing large SessionHost
and LocalJournal translation units because dispatch, schema migration and the
event/projection transaction already belong there; duplicating those paths would
weaken atomicity. This slice does not perform an unrelated monolith refactor.

Remote switch uses signed opaque target IDs and encrypted context-fenced commands.
Switched conversations reject legacy cloud file operations and unfenced controls with
`context_upgrade_required`; Stop and existing pending approval/question handling
remain available. Explicit file requests may carry encrypted
`resource_context_revision`; the daemon resolves that recorded root and checks the
command's authenticated repository/worktree IDs and content revision against it.
Unknown contexts fail closed; async completion rechecks session generation and
recorded context rather than requiring the root still be active.
The isolated relay change additionally requires an active catalog and explicit
`workspace.files.read` target-worktree grant for historical reads, alongside the
conversation grant. It checks at enqueue, lease and result retrieval. Discovery
never grants this capability automatically. Mobile request encoding supports the
field. Full provenance-to-link routing remains pending: after switching, mobile refuses unscoped transcript file links and desktop refuses relative unscoped links rather than resolving them against a different root.
Scope is checked again at the daemon, even if a stale relay catalog accepted a
request. No provider quota is consumed by this slice's automated tests.

## Isolation and verification

Develop and build in an independent worktree. All new tests use temporary Git
repositories, SQLite databases and fake providers. The relay boundary test runs
production HTTP serialization and encryption against both an in-memory transport
and a private HTTP relay fixture on a random loopback port. Its network transport
rejects every destination except that fixture. Both variants use an in-memory
credential store instead of Keychain.
No running desktop, user daemon, existing relay, live credentials or user session
is restarted or used. Do not enable authenticated/native smoke flags for this lane.

Tests cover A → B → A and same-thread cwd forwarding, busy/approval rejection,
stale requests, no-op selection, foreign and non-root directories, unchanged dirty
files, migration/restart, stale writer fencing, atomic failure rollback and the
legacy relay file boundary. The loopback relay fixture is not the deployed Control
API service and does not qualify a production remote switch. These prove the adapter contract with a fake provider,
not the behavior of a quota-consuming Codex session.

## Follow-up slices

1. Extend explicit resource provenance to more tool forms and route scoped refs through mobile links.
3. Async/bounded Git catalog reconciliation and recovery when the old root is missing.
4. Provider-specific integration verification for Codex, Claude and Cursor;
   explicit runtime handoff for providers that cannot preserve a thread.
6. Atlas live sessions pinned to their source context, over the separately planned
   E2EE WebSocket tunnel.

Deployment and real-provider verification are separate from these isolated builds. The running remote-access app must remain untouched.

## Model activity and large catalogs

`conversation_worktree_activity` records only new evidence, scoped by session ID and generation. Turn-start pins the execution root; structured command cwd and absolute existing file-change paths resolve to the most specific registered Git root in the same repository. Text mentions, discovery and selecting a worktree alone do not mark it. This is usage provenance, never deletion ownership or a grant.

`session.worktrees` refreshes Git catalog, filters branch/folder, sorts touched-first then pages (20 default, 100 maximum). Its remote encrypted result exposes opaque IDs, branch and folder labels, never absolute paths. Discovered catalog entries are published before the result. The current execution root stays separate from the `conversation_navigation` anchor, which preserves desktop conversation placement and drafts across restarts without reconstructing old event history.

Desktop removes the composer PR/worktree rail and offers a paged picker from its header. iOS uses the same daemon page and keeps pending/failed durable switch state. No UI selection mutates Git files or running Atlas sessions.

## Conversation configuration

Model, effort and execution policy are persisted with a separate configuration revision, scoped to conversation and generation, not worktree. Session listing and configuration-change events include the effective values. Desktop projects acknowledged daemon settings into its local cache; mobile keeps an account-scoped encrypted cache and refreshes on change events and reconnect polling. Cached values are never automatically written back. Stale writes are rejected and clients refresh.

Remote settings and attachment commands after a worktree switch must carry the current resource or expected context revision; when both are supplied both must match. Desktop settings use the owner-only local IPC selection path, with expected configuration revision. Its model catalog is supplied by the desktop picker, just as for local turn submission; cloud clients cannot opt into this trusted selection path. Model-only changes omit permission. Only explicit policy changes update permission. Compatible provider/model selections preserve the current permission in the desktop picker.

### Catalog latency and settings reads

Provider catalogs are runtime-local and shared across conversations, with a 60-second fresh window, background refresh, one in-flight request per provider, 20-second fetch deadline and 10-second failure backoff. The last good catalog can be served for at most 15 minutes through transient failures. Provider stop/reload and Codex account-change notifications invalidate the corresponding cache; no provider credentials or catalogs are persisted by this daemon cache. Fetches start concurrently. A conversation-scoped request waits only for its own provider; optional providers populate subsequent cached catalogs. Unscoped/new-conversation reads aggregate available providers.

`session.configuration.read` with `effective_only: true` bypasses provider queries and returns `{ok, effective_only, revision, effective, adapter}` under the same generation/context fences. Clients without a cached catalog use the full read. New clients tolerate old daemons returning a full response. Mobile applies configuration events directly, ignores stale revisions, polls only effective values for recovery, and refreshes the catalog on entry or after five minutes. Cached choices remain interactive during refresh; pending writes still lock the picker and use revision fences. Catalog refresh does not reset permission. The existing SessionHost dispatch owns aggregation; cache lifecycle and concurrency are isolated in ProviderCatalogCache.

### Message acknowledgement and steer media

`user.message` includes the originating `command_id`, as `turn.steered` already does. Mobile records that ID before posting and hides its optimistic row when the matching event arrives, regardless of receipt timing; another client’s message cannot consume the pending row. Retry retains the original command payload and idempotency key. Codex steering converts local/staged images to inline `image` data URLs before dispatch so queued guidance does not depend on temporary file lifetime. Claude already embeds the image bytes in sidecar input. Unreadable images reject the steer rather than silently sending text alone.

Transcript output selectors (session/generation/turn/item) and hash-addressed media are independent of the currently selected worktree; switching worktree must not reject those reads with `context_upgrade_required`. Relative file paths remain context-bound. The mobile read deadline includes submission and polling, and stale presence hints do not prevent explicit reads.

## Cross-repository conversation selection (2026-10-16)

The approved Repository → Worktree flow extends explicit Codex selection across
registered Git repositories. Trusted local IPC validates an existing exact Git
root. Its catalog additionally exposes recorded repository roots locally; cloud
projection removes that list, context paths and absolute worktree paths.

A phone selects only opaque catalog targets in the same workspace and machine.
Both relay enqueue and lease require the conversation capability plus an active
independent target catalog entry and `session.create` target grant. Command and
result AAD bind both target IDs. Encrypted payload paths cannot opt into local
repository browsing or override catalog resolution. The matching service change
is owned by KétViệt; an older relay safely rejects cross-repository switches.

The stable conversation/navigation anchor is unchanged. Native clients project
its acknowledged execution root separately into header, Files/Source Control,
file search and sidebar grouping. An explicit repository selection promotes that
group. Old file/Atlas provenance remains captured. A running turn keeps its
original root; next-turn preparation uses the newly selected root. Startup,
shutdown and pending approvals remain fenced, and unsupported providers remain
explicitly unavailable. Qt rejects stale context read responses; iOS retains
verified result metadata until the relay reports an equal/newer context revision.
