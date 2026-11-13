# 0121: Prepare a session before staging a conversation’s first attachment

Status: accepted; additive Relay contract approved, implementation in progress
Date: 2026-11-13

## Context

The Machine workspace uploads attachments for existing conversations through
session/generation-scoped `attachment.stage.begin/chunk/commit/cancel` operations
([ADR 0119](0119-remote-machine-composition.md#remote-attachment-staging)). A new
conversation has no such scope. It currently creates a session by submitting its
first turn, so attaching before that turn cannot use the existing upload contract.

The relevant current behavior is explicit in the public code:

- `MachineWorkspaceView::createConversation` sends a worktree and a nonempty prompt.
- `DaemonMachineBackend::createSession` maps that operation to `session.start_turn`.
- `RelayMachineBackend::createSession` rejects an empty prompt and issues the
  worktree-scoped `session.create` command.
- `CloudControlWorktreeCommands.cpp` also requires a prompt and maps creation to
  `session.start_turn`; `SessionHost::startTurn` starts provider work.
- `ControlV2Content.h::worktreeScoped` recognizes a fixed set of command kinds.
  Attachment commands instead require the existing session’s authorization and key scope.

There is no existing remote operation that allocates an inactive session without
starting a turn. Sending a synthetic prompt to obtain an ID would consume provider
work, create misleading transcript content and complicate cancellation.

## Decision

Choose a pre-created inactive session, rather than a separate pending-file store.
The additive Relay operation and coordinated hosted support are approved. Shipping
requires both sides to implement the lifecycle and advertise the capability.

When the first file is attached, prepare a session for the selected enrolled
worktree without starting a provider, recording a user message or charging a turn.
The daemon persists the inactive session and its initial generation before replying.
The client then uses the existing session-scoped attachment commands unchanged and
submits the real first message through the existing turn-start operation with the
committed attachment IDs. An attachment-only first message follows the same path.

Use one backend preparation contract across the Machine workspace transports:

| Surface/transport | Preparation and staging |
| --- | --- |
| Existing local Agent Workspace | Preserve its existing local first-message attachment path. |
| This Mac in Machine workspace | Prepare through local daemon IPC, then use the existing attachment commands. |
| SSH Machine workspace | Carry the preparation operation through authenticated daemon IPC over SSH; keep files on the selected host. |
| Relay Machine workspace | Add an explicit worktree-scoped `session.prepare` operation, then reuse session-key-protected staging and turn commands. |

The Relay operation uses the `session.prepare/1` capability. Preparation and its
prepared-only cancellation are additive; normal session deletion must not be used
to discard a draft that may have become active. Both the daemon and hosted Relay
must advertise support before the client enables first-turn attachment. SSH needs
the daemon capability alone. The Relay operation must have an advertised capability. A client must not infer
support from existing-conversation attachment support or silently fall back to a
dummy turn on older hosts. Unsupported hosts keep first-message attachment disabled
with a clear explanation; existing-conversation uploads continue to work.

The new operations are:

- `session.prepare`, scoped to the verified worktree, with `client_request_id` and
  initial provider `options`. It returns `session_id`, `session_generation` (1),
  `state` (`prepared`) and `expires_at_ms`. Local IPC/SSH additionally supplies the
  canonical `worktree_path`; Relay resolves the enrolled worktree on the host.
- `session.prepare.cancel`, scoped to that session and generation. It discards only
  an inactive allocation, is safe to retry, and refuses an activated conversation.

The existing `attachment.stage.*` and `session.start_turn` commands retain their
wire shapes. The first turn adopts the prepared worktree and options; a request
that attempts to retarget them is rejected.

### Ownership, retries, cancellation and failure

Preparation uses the selected machine’s verified worktree, catalog/lifecycle
revision guards and existing creation authorization. Persist one stable operation
identity before delivery. Retries of that identity must return the same session,
not allocate another one; reusing it for a different scope must fail. Apply the
existing session-creation quotas and keep at most one preparation active per draft.

Bind the result and every upload to machine, worktree, session and generation.
A late result after a machine/worktree switch must not attach files or replace the
new view’s draft. Reject stale generations and changed/deleted worktrees before
allocating or accepting bytes. The Relay must establish the ordinary authorized
session/key scope before accepting any attachment command.

Keep the text and selected files visible while preparation runs; disable Send until
preparation and all commits succeed. On preparation or upload failure, retain the
draft and show retry/removal. A failed turn keeps ready attachment IDs, as existing
conversation sends do. Do not consume them on a queued Relay acknowledgement.

Prepared sessions are private to the authenticated subject and creating device. They
have no provider turn or transcript entry and are excluded from ordinary session,
repository, team, phone and other-subject listings before the first real turn.
Only the owner can stage, start the first turn, or discard a prepared session;
knowing an ID or sharing the same workspace is insufficient. Routing metadata may
identify this private scope, but attachment contents remain encrypted.

Removing a file cancels its existing staging operation. Discarding the draft also
cancels preparation/staging and removes the prepared session and staged bytes,
including when a queued preparation completes late. Cancellation is atomic against
activation: it cannot delete a session whose real first turn already started.

An abandoned prepared session expires **24 hours after its original allocation**.
Retries and heartbeat do not renew that deadline. The daemon cleans expired prepared
sessions and staged files on startup and periodically while running. Persist the
owner, request binding and deadline so daemon restarts do not bypass expiry or
allocate duplicates. Retain terminal idempotency receipts under the existing command
receipt policy; retrying an expired/discarded preparation within the retained retry
window must not recreate it.
Keep terminal receipts for at least seven days after the original preparation
deadline, covering the command retry window. Each cleanup pass handles at most
128 records. Bound allocations to 128 active drafts per owner and 2,048 per daemon,
and retain at most 32,768 receipts; a capacity failure retains the client's draft.
Preparation options are bounded to 8 KiB. These limits supplement the existing
attachment byte quotas; they do not charge or start provider work.
If expiry wins against Send, report the expired draft and retain local text/selected
files for an explicit new preparation. Prepared sessions count toward existing
ownership/workspace checks and normal resource quotas.

### Protocol and security impact

**This is an approved additive Relay wire-contract change.** The new
worktree-scoped command kind, its authenticated/idempotent result and capability
must be understood by the client, host and Relay allowlist/validation path. Reusing
an optional field of `session.create` would still change its prompt-required
semantics; it is not a way to claim protocol compatibility.

The local IPC/SSH preparation contract also needs an explicit inactive lifecycle.
Do not weaken the existing session requirement for `attachment.stage.*`, introduce
an unauthenticated pending upload area, or send remote host paths as local files.
Retain the existing per-file/draft quotas, digest validation, generation checks,
bounded encrypted chunks, key wrapping and expiry. No provider credentials, file
bytes or plaintext attachment paths belong in Relay routing metadata or logs.

## Consequences

Preparation adds an inactive-session lifecycle and cancellation/retry states, but
keeps one authorization/key scope and one attachment store. Existing-conversation
uploads and their wire format remain unchanged. The hidden prepared-session lifecycle, cleanup and activation must be tested rather
than hidden behind a fabricated turn.

Implement and verify backend and offscreen GUI coverage
for first-turn staging over local IPC, SSH and Relay; attachment-only Send; queued
preparation completion; duplicate delivery; remove/discard during preparation and
upload; stale scope/generation; denied or failed preparation; upload/commit failure;
and a failed first turn retaining ready files. Re-run the existing attachment
backend and panel suites. The ADR alone does not establish runtime verification; the implementation and test
results must accompany it before release.

## Alternatives considered

- **Pending staging adopted by the first turn:** avoids creating an empty session,
  but requires a new worktree/device-bound upload identity, encrypted chunk/key
  scope, quota/expiry store and atomic adoption contract. It also changes Relay
  commands and has more authorization and orphan-cleanup paths. Rejected in favor
  of reusing the existing session-bound attachment mechanism.
- **Create-and-start with a placeholder prompt:** rejected because it starts work
  the user did not request and makes failure, history and cancellation misleading.
- **Inline files in the first create command:** rejected because it bypasses the
  bounded chunk/commit contract, exceeds existing envelope limits and changes the
  Relay payload and attachment validation semantics.
