# Destructive lifecycle guards

Date: 2026-09-12

KetPlus treats archive, restore, and delete as different operations:

- Archive is reversible metadata and remains available while a turn runs. Archiving does not
  interrupt the daemon session.
- Restore is fail-closed. A worktree must still be an existing, registered Git worktree and match
  the archived Git administration-directory identity; a
  conversation's worktree must still exist. Failed validation leaves the archived marker intact.
- Conversation delete requires an idle turn, explicit typed confirmation, daemon acknowledgement,
  and a transactional purge of the KetPlus journal projections. Remote deletion additionally uses
  a server-issued two-minute challenge bound to owner, device, scope, generation, lifecycle
  revision, and action. HTTP 202 means accepted only: the cloud lifecycle moves from revision N to
  pending N+1, and only a successful daemon result creates the durable N+2 tombstone. The daemon
  rechecks the signed scope, generation and idle state, writes its tombstone in the same transaction
  that purges local payloads, and can replay the same final result after a crash. Cloud finalization
  revokes session keys and redacts retained ciphertext before publishing the tombstone. Tombstones
  are consumed before session caches and fence stale heartbeat/discovery events and ABA reuse.
  Provider-owned Codex history is not deleted.
- Worktree delete is offered only for a linked, non-main, idle worktree with no modified editor in
  that directory. Typed DELETE confirmation is required; the same preflight is repeated after
  confirmation and non-force Git removal performs the final dirty, untracked, membership and locked
  checks. Child conversations are archived only after Git acknowledges deletion, and the branch is
  retained. Remote/mobile worktree deletion remains unavailable until the control plane can prove
  the native editor/session preflight truthfully; it never falls back to arbitrary filesystem
  deletion.
- Sign-out removes the local enrolled-machine account pointers and daemon credential, fences late
  authentication replies, and stops cloud polling. Installation identity and device cryptographic
  keys remain local so a later sign-in can safely re-enroll. Sign-out is not device revocation.

These rules preserve local/provider ownership boundaries and avoid presenting a destructive action
as complete before its owning subsystem acknowledges it.

Visual acceptance remains a separate per-screen, per-state, per-viewport, per-theme gate with at
most 5% raster mismatch. A missing or extra structural control, clipping, broken interaction, or an
accessibility failure is a hard failure regardless of the raster score. No pixel-parity pass is
claimed for this slice because native capture permission was unavailable.
