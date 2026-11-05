# 0016: Conversation fork

Status: accepted
Date: 2026-09-25

## Context

Codex (`thread/fork`) and the Claude Agent SDK (`forkSession`) can copy a
conversation into a new one that continues on its own. Users want to try another
direction from the same point without losing the original. A KetPlus
conversation has two histories that must stay in step: the provider's native
thread, which the model resumes, and the daemon journal, which Desktop renders.
Cursor's ACP has no fork operation.

## Decision

- The daemon adds `session.fork` (protocol feature `session.fork`, minor 1). It
  targets an existing session generation and accepts an optional
  `fork_session_id` so a retried request cannot fork twice. The result is
  stored as a command result like other mutations.
- A source may be idle or running. The fork snapshots history written so far;
  only the copy settles inherited active rows, and the source turn continues.
- The provider forks its own thread: Codex `thread/fork` with `threadId`,
  `cwd` and `ephemeral: false`; Claude through the sidecar's `thread/fork`,
  which calls the SDK's `forkSession` without calling the model. Providers
  without the new `threadFork` capability (ADR 0015), today Cursor, get
  `unsupported_provider_operation`.
- After the provider answers, one journal transaction creates the fork's session
  row and copies the source generation's `events`, `replay_events`,
  `replay_projection_state` and large-event rows at the same cursors. The fork
  then appends `session.forked` (`from_session_id`, `from_session_generation`,
  `through_cursor`). Media references are copied so deleting either
  conversation keeps the other's images.
- The fork keeps the source's worktree, provider, model, effort, execution
  policy and mode, and is titled "<title> (fork)". It does not keep the
  source's threads with other providers: switching the fork to one of them
  starts a new thread from the handoff transcript instead of sharing one.
- A fork does not own the worktree (ADR 0010). Deleting it never offers to
  delete the worktree, and deleting the source does not affect the fork.
- Desktop maps `thread/fork` to `session.fork`, registers the new session as a
  discovered daemon session and selects it. "Fork this conversation" appears in
  the conversation name's context menu (Agent Workspace sidebar and the native
  sidebar) and in the transcript's right-click menu. It is enabled only for a
  started, daemon-backed conversation whose provider reports
  `threadFork`.

## Consequences

- A forked conversation replays exactly like its source up to the fork, then
  continues on its own provider thread. Neither conversation sees the other's
  later turns.
- Journal size grows by a copy of the source history per fork.
- The provider transcript files (Codex rollouts, Claude session JSONL) are
  owned by the provider. Deleting a KetPlus conversation still leaves them, as
  it does today.
- Forking from an earlier message (Codex `lastTurnId`, SDK `upToMessageId`) is
  not offered yet. It needs a journal cut point that matches the provider's.

## Alternatives considered

- **Replay the transcript into a new provider thread.** Costs a model call and
  loses tool state the provider keeps. The native fork is free and exact.
- **Share the source's journal rows by reference.** Avoids the copy, but every
  journal query and deletion would have to follow the reference. Copying keeps
  sessions independent.
- Mobile uses the signed, generation-scoped `session.fork` command through the
  relay. The daemon acknowledges asynchronously, then publishes the new session
  and its independently encrypted history. Replays use the same command and fork
  identifiers so a lost response cannot produce a second conversation.
