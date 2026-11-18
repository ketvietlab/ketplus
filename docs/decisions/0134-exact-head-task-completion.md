# ADR 0134: Acceptance is consumed by Ready and verified completion

Status: proposed (implementation validated; independent review pending)
Date: 2026-11-17

## Context

The route decision introduced in ADR 0129 was an audit record without a Ready
consumer. Separately, ADR 0118 allowed a merged PR or a leader-provided URL or
hexadecimal string to finish a task without enforcing its review requirement.
Neither a route label nor a string supplied by an agent proves a merge.

## Decision

A successful host-verified `developer_leader` decision moves the exact assessed
head to Ready. `developer_reviewer` follows the existing independent review queue;
unsupported verification goes directly to that queue rather than requiring an
unproductive leader pass. Advice remains independent of tasks and worktrees.
Confidence supplements the mandatory daemon checks and grants no user permission.

The daemon persists a version-1 `completion_acceptance` with the accepted head,
semantic context digest, source, actor, timestamp and an opaque origin identifier.
Sources are `leader_policy`, `independent_review` and
`leader_targeted_verification`. Only their validated daemon paths write acceptance.
The original short-lived receipt is consumed before the state transition; no
receipt revision is rewritten or reused to authorize a later completion.

The durable context binds the task, delta, owner, requirements, findings, route,
dependencies, execution repository and policy/membership fences. It normalizes
transport revisions, note/audit history, the acceptance itself and merge UI
bookkeeping. Merge decisions do not change the assessed delta or replace the
separate user merge permission. Other material context changes invalidate Ready,
withdraw its merge offer and leave an actionable blocked task. Its owner can
resume work and obtain fresh proof or review. Resuming Ready also revokes the
previous acceptance deliberately. Independent review can establish Ready while
dependency completion is pending; those dependencies must be Done before merging
or completion. Criterion text remains bound, but submitter checkbox bookkeeping
is not substituted for the reviewer verdict. Leader policy acceptance continues
to require every criterion and dependency before Ready.

Automatic completion requires current acceptance and independent host facts:

- Git-only completion must contain the same accepted branch head, with a known
  pinned origin and a currently readable, clean checkout. Recheck those live
  facts when applying the asynchronous merge answer. A later head is never
  finished merely because an older probe said it was already merged.
- PR completion must match the accepted head, branch, repository identity and
  configured integration target. The host reads PR facts, the clean current
  checkout and the repository origin. Squash/rebase merge commits remain distinct
  from the accepted source head. Missing, stale or mismatched facts fail closed.
- The origin is read from Git at acceptance and checked again at completion.
  Store only its canonical digest, never a URL containing credentials. A fork
  with the same commit and branch is not the accepted execution repository.
- `team_task_close` requests this verification for Ready work; its argument is
  a reference to check, not proof. Offline or unknown results leave work unfinished.
  Neither a tool call nor a generic state assertion marks Ready or Done.

Only a trusted local user command can deliberately override completion:
`team.task.move` with `state: done`, boolean `user_override: true` and a nonempty
note. It records `user_completion_override`, explicitly without claiming a
verified merge or reviewer verdict. Agent tools do not expose this authority.
The existing paired command allowlist is unchanged; no new paired override UI
is implied. Ordinary user merge decisions and merge policy remain separate.

## Compatibility and verification

Old documents omit acceptance and still deserialize. A legacy Ready task is not
silently grandfathered into proof: its owner can resume and submit current work
for review, or the user can make the distinct intentional override. Unknown
acceptance versions and malformed or rebound records confer no exception.
This supersedes the unconditional completion rules in ADR 0118, not its bounded
remote probe, fairness, merge-commit audit or dependency accounting.

Regressions must cover the actual producer/host/tool consumer reaching Ready,
mandatory-review bypasses, changed heads and policies, independent review and
finding closure, replay/serialization, wrong-repository PR facts, origin changes,
offline/unknown results and explicit user authority. These changes must receive
independent review; their own policy cannot approve their implementation.
