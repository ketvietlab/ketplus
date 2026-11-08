# ADR 0076: Commit local turn acceptance before publishing it

Status: accepted for local turn state and its acceptance events; whole-command
idempotency and crash recovery remain separate hardening work.

SessionHost committed user.message before turn.queued. A failure of the second
write rejected the command but retained and published the prompt. Configuration
changes supplied with the turn added another independent session write; a failed
configuration event was ignored. Original regressions reproduce eleven failures
and six control/setup passes, including visibility from an independent reader.

LocalJournal now owns one transaction for the starting session/configuration,
pinned context, user.message, turn.queued and optional session.configuration_changed.
The existing event/projection/activity mutation path can join that owning
transaction without beginning or committing a nested transaction. Standalone
append and worktree-switch callers retain their existing ownership. Large-item
append cannot enter this nested path: acceptance has two or three ordinary bounded
events. Its receipt is read inside the transaction and returned only after commit.
Errors roll back state, counters, events and projections and preserve diagnostics.

SessionHost drains prior accepted stream data first and refuses a new turn while
its recovery queue is blocked. It restores only draft state/configuration after a
failed acceptance, preserving counters advanced by that prior stream. No provider
call is started for a rejected acceptance. Exhausted configuration revisions are
rejected before increment. On success, in-memory acceptance state is complete
before the first eventReady signal; question-expiry and agent callbacks run after
the acceptance events are published. Existing protocol event names and defaults
stay unchanged. Subsequent provider failure remains a separate persisted turn
outcome, not a failed local acceptance.

The focused Qt suite passes 26/26 using temporary databases and fake Codex:
abort/ignored writes for both raw events and queued projection, session/configuration
writes and configuration events, deferred-FK commit failure, no published rejected
prompt, unchanged counters/state, repair/retry, reopen, duplicate command replay,
integrity and independent-reader visibility. Additional cases cover cursor/turn
and configuration-revision exhaustion, the last two/three valid cursor slots and
oversized prompt/configuration rollback. No provider quota or user journal is used.

New transaction and host-publication behavior live in small owning-module files.
LocalJournal.cpp and SessionHost.cpp already exceed 1,000 lines; their changes are
limited to transaction ownership and the acceptance call site, avoiding a broad
unrelated split. Shared headers require the full Pro matrix and independent
exact-head review before integration. No UI implementation is changed.

The bound is on acceptance event count/payloads, not on every existing projection
or activity query. Whole-command result persistence, pre-acceptance attachment/
provider side effects, process/power-loss recovery, general reentrant subscriber
ordering and multi-GB/Windows acceptance are not established by this package.
