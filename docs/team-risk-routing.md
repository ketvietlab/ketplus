# Team risk routing contract (version 1)

The daemon entry point is
`TeamCoordinator::decideRoute(teamId, taskId, request, actorSession)`.
`actorSession` comes from the authenticated caller binding; never take it from a
payload. The authenticated agent tools expose this contract; desktop and paired
user clients cannot impersonate a leader by supplying a session/actor field.
Existing clients keep their existing methods and reviewer behavior.

## Agent tools and host evidence (version 1)

`team_task_route_decide` takes the request below plus optional `task_id`. Only
the authenticated same-team leader can decide. The result includes
`routing_decision`, `conclusion_source: leader_policy`, `reviewer_verdict: false`
and `authorization_granted: false`. Neither a route nor confidence completes a
task, queues a reviewer, changes merge eligibility or grants user permission.
Developers submit evidence once; the leader selects the route and records a
concrete reason for requesting optional conservative review. Advice can be
answered directly without any tool call, task, worktree or developer/reviewer turn.

`team_task_route_verify` accepts **only** `task_id`, `expected_team_revision` and
`head_commit`, from the task's developer or the same-team leader. The deferred
MCP transport runs the host's fixed recipe on the existing Git worker. It accepts
no check names, commands, pass booleans, risk coverage or serialized evidence.
The synchronous internal broker entry does not execute verification as a fallback.
At most four verifications can be queued at once.

The first recipe, `prose_reflow_v1`, supports a deliberately narrow mechanical
change: reflow of existing plain-prose Markdown files under `docs/`. The path is
only a prefilter. Both old and new Git blobs must consist entirely of unindented
prose paragraphs in a restricted grammar, with identical words, punctuation and
paragraph boundaries after collapsing ordinary spaces/soft line wraps. Markdown
constructs, code, links, directives, hard breaks, unknown syntax, additions,
deletions, executable/symlink modes and semantic changes are unsupported. A heading
or a general code/documentation change therefore needs a reviewer. This recipe
does not claim a general semantic analyzer or safety from filenames alone.
Tasks with review history are ineligible for this recipe. Finding closure needs
separate reviewed-baseline-to-fix-head attestation; a round-zero prose receipt
does not provide it. The distinct `prose_line_width_fix_v1` producer provides
that attestation only for its bounded, nonmandatory line-width condition on a
current structured review record; it checks the full three-revision scope. The
separate `prose_preference_deferral_v1` recipe attests only an unchanged reviewed
head and a nonmandatory outside-task preference on the same restricted task delta;
it does not assert a local fix. See
[ADR 0132](decisions/0132-bounded-review-arbitration.md) for supported closure,
leader decision/arbitration and the mandatory reviewer fallbacks. Neither recipe
accepts a caller's recipe selection or risk/pass assertions.

The daemon establishes all evidence itself:

| Evidence | Independent source |
| --- | --- |
| Identity/freshness | Actual branch, clean Git status and full HEAD, team/task revisions and host-scoped task context |
| Scope | Entire diff from the task's recorded base, at most eight existing regular files; each blob at most 64 KiB |
| Semantic coverage | Restricted prose grammar and paragraph/word equivalence over both immutable blobs; any uncovered policy domain fails closed |
| Checks | Fixed `git_clean_revision`, `prose_reflow_equivalence`, `scope_modes`, `git_diff_check` recipe results at that full commit, including final HEAD/clean recheck |
| Rollback | Existing ancestor base and immutable before-objects; no rollback or write is performed |
| Findings/approval | Zero findings/failures in this mechanical proof; first plan approved and assignment not held; no review history, outside-lane flag or pending team/task user decision; the recipe only reads Git |

The proof concerns this exact delta, not the correctness of the repository's
baseline. Old tasks whose recorded base includes unrelated changes fall back to
review rather than silently narrowing the assessed diff. Git calls have bounded
output/time and disable external diff, textconv, fsmonitor and inherited Git
repository overrides. Every operation disables replacement-object resolution,
including base/tree/blob reads and receipt lookup. Diff enumeration, whitespace
checks and clean-status fences explicitly include submodules regardless of local
ignore configuration; a changed gitlink is an unsupported mode, never omitted
from the proof. No repository script, build or provider is executed.

Opaque receipt IDs and per-check/scope/rollback references are minted by the host.
The host's in-memory store holds at most 128 immutable receipts, at most 32 per
team, for ten minutes measured by a monotonic clock. Admission prunes all expired
entries first, then replaces only the requesting team's oldest receipt by insertion
order. At global capacity a new team with no existing slot receives
`verification_busy`; it cannot evict another team's evidence. Duplicate IDs are
refused without overwriting or renewing a live receipt.

Lookup binds a host-computed semantic revision: the task document except note
history and its transport revision, relevant decision/dependency state, settings
and membership fences, policy/permissions, full commit, base, worktree, branch,
repository and integration target. Reporting the proven head in a round-zero
task is equivalent to its previously empty head field; a different reported head
or a reviewed baseline is not. Unrelated task activity and result notes do not
invalidate proof. Requirements, acceptance, assignment, findings, policy, members,
scope and relevant approval changes do. Restart, expiry and eviction invalidate
lookup. Receipts keep their original proof task revision; after semantic and live
checkout validation, host lookup projects the current task transport revision for
the coordinator's fence. Verify replies distinguish `proof_task_revision`,
`task_revision` and the opaque `semantic_revision` hash.

`expected_team_revision` remains mandatory at verification admission and every
route decision. Completion revalidates the authenticated role and semantic inputs
rather than requiring an idle team. Deferred replies are consumed synchronously
in their own broker continuation and removed even if that actor has expired;
duplicate concurrent requests each receive their own result. The coordinator also
rechecks the current clean head before accepting a decision. A decision itself
advances revisions, so its receipt cannot authorize a later action. A caller's
invented or stale ID never becomes evidence. Raw file content/command output is
transient and is not persisted in team audit.

The default port remains `nullopt` for other hosts. The real runtime positive
regression goes through producer → host store/lookup → `decideRoute`; fake port
injection is confined to the foundation unit tests. The agent tool contract is
the same for local and remotely hosted agent sessions; a paired **user** is not
that agent identity. `team.route.verify` / `team.route.decide` from local or paired
user clients return `role_forbidden`, even with a supplied leader session ID.
Unknown methods/fields on legacy clients grant no capability.

Compact task rows and paired task projections include the validated versioned
`routing_decision` when present, `conclusion_source: leader_policy` and
`routing_status: historical`. Actual `state`, `round` and reviewer history retain
their meaning. Digests name the recorded actor, route, revision, receipt reference
and actual task status, explicitly distinguishing the leader audit from a reviewer
verdict or user approval. Legacy tasks without a decision retain their prior shape.

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
