# Team risk routing contract (version 1)

The daemon entry point is
`TeamCoordinator::decideRoute(teamId, taskId, request, actorSession)`.
`actorSession` comes from the authenticated caller binding; never take it from a
payload. This is a C++ foundation contract. No agent tool, wire method, runtime
receipt producer or UI capability is exposed by this change.

## Requests

All calls require `expected_team_revision` and a bounded machine `reason` matching
`[a-z][a-z0-9_]{0,99}`. `route` defaults to `auto`; supported explicit values are
`leader_direct`, `developer_leader`, `developer_reviewer`. Unknown fields/types/kinds
are rejected. Only the same team's current leader can call, including advisory
calls; developer, reviewer, unrelated leader and the `user` alias cannot fast-pass.

Advisory: empty task ID, `kind` one of `explanation`, `proposal`, `synthesis`,
`coordination`. No `head_commit`, `evidence_id` or `confidence`. It records
`leader_direct` without creating a task/worktree. A leader may still answer normally
without calling this API; the API is not a prerequisite for conversation replies.

Change: existing in-progress task ID, `kind: change`, exact 40-character lowercase
hex `head_commit`, optional opaque 64-character lowercase hex `evidence_id`, and
integer `confidence` (91–100 required for the low-risk exception). Missing evidence
routes `auto` to reviewer; explicitly demanding `developer_leader` returns
`review_required`. Dirty/unreadable/head-changed tasks fail `stale_revision`.

## Trusted host receipt adapter

`routingEvidence(team, task, evidenceId)` returns `optional<risk::VerifiedEvidence>`.
Absence is unknown. The default port deliberately returns `nullopt`.

A producer must verify immutable evidence independently before returning:

- matching `id`, `teamId`, `taskId`, `taskRevision`, full `revision`;
- exactly the mandatory domain set (`risk::mandatoryDomains`), each clear/affected/
  unknown; new or omitted coverage does not become clear;
- `materialRiskKnown`, `lowRiskVerified` based on semantic scope and evidence;
- complete required check names and pass receipts for that revision, without
  duplicate names, missing results, failed/stale checks or raw outputs;
- scope/rollback reference IDs, required user approvals satisfied, and explicit
  zero unresolved findings/P0/P1/failures. Unknown counters default to -1, not zero.

The adapter is trusted daemon code, not an assertion deserialized from an agent or
UI request. Tool/UI integration must not convert arbitrary submitted JSON into a
`VerifiedEvidence`. It needs its own verified receipt producer and permission tests.
For material risk that cannot be reliably established, return unknown and use reviewer.

## Results, freshness and follow-up

Errors use existing `TeamResult`: `role_forbidden`, `stale_team`, `invalid_payload`,
`task_not_found`, `review_locked`, `stale_revision`, `stale_evidence`, `review_required`.
Accepted calls commit a `risk_route` audit and version1 decision in task/team JSON.
No existing state or reviewer queue is changed. Advisory follow-up is `answer_only`;
low-risk is `merge_then_acceptance`; conservative routing is `review_required`.

`team_revision` is the revision containing the decision. `task_revision` is the
assessed pre-decision task revision from the receipt; the decision's own audit update
may advance the task's changed revision. The exact commit plus assessed revision and
receipt identify what was considered. Reusing an unchanged payload fails its expected
team fence. Stored decisions are historical and may become stale after any task/head/
AC/risk/check/approval change. A downstream workflow mutation must revalidate; it
cannot treat deserialization as renewed approval.

Legacy JSON without decisions and malformed/unsupported records deserialize with no
exception. Existing task/team behavior is unchanged. Provider calls, merge permissions,
cleanup authority and user approval are outside this contract. Fixtures are synthetic;
never include real accounts, transcripts, machine paths, tokens or private service data.
