# 0129: Evidence-gated team routing

Status: accepted
Date: 2026-11-16

## Context

Explanations, proposals, synthesis and coordination should not require a fabricated
code task, worktree or commit. Changes still need a review path appropriate to
risk. A leader's confidence or a submitter's severity label cannot establish that
security, data, release or compatibility boundaries are safe.

## Decision

Model three routes in the daemon teams module:

| Route | Purpose | Conditions |
| --- | --- | --- |
| `leader_direct` | Non-mutating explanation/proposal/synthesis/coordination | No task, code revision or operation authorization |
| `developer_leader` | A developer's verified low-risk change | Same-team leader, complete acceptance and trusted exact-revision evidence |
| `developer_reviewer` | High-risk, unknown, incomplete or explicitly conservative change | Default for legacy/missing evidence; existing reviewer flow remains available |

The foundation records the least demanding safe route for `route: auto`. Explicit
`developer_leader` is refused if that route is not safe. Explicit reviewer routing
may always remain more conservative. No task is silently reclassified: the daemon
requires an explicit leader call; a task in review, with a counted review or review
history cannot use this API to replace its review/finding path.

### Trusted evidence and authority

`TeamCoordinator::decideRoute` checks actual team membership/leader role, exact
`expected_team_revision` and clean, readable current task commit. Its mutation
commits through the existing journal transaction; replaying an accepted request
has a stale revision. Caller-supplied actor, check booleans, risk assertions or
raw output are not accepted fields. Change decisions require an existing task in
progress. Fast-pass requires nonempty completed AC, completed dependencies, a
developer owner distinct from the leader and integration target `integration`.

Evidence comes from `TeamHostPort::routingEvidence`, not request JSON. An immutable
receipt is bound to team ID, task ID, assessed task revision and exact full commit.
The host must establish the required check set and verify every check receipt and
semantic risk coverage. It must not mark something low risk solely from a filename,
self-declared severity, confidence or successful tests. The default adapter returns
no evidence; it enables no fast-pass. The runtime receipt producer and agent/UI
transport bindings are follow-up work, not advertised live capabilities here.

All eleven domains need explicit verified coverage. Any affected or unknown domain
requires a reviewer: security, authentication, secrets, billing, data deletion,
migration, automatic cleanup, signing, updates/releases, protocol compatibility,
and large blast radius. Unknown material risk, unresolved findings/P0/P1/failures,
missing/failed/stale checks, absent scope or rollback evidence, and missing required
user approval also require reviewer. Confidence above 90 is an additional signal,
never a replacement for these gates. New/unrecognized coverage is conservative.

### Persistence and audit

Task `routing_decision` and team `last_routing_decision` retain a versioned structured
record. Missing/invalid/unknown-version data grants no exception, so old teams and
tasks retain reviewer defaults. A decision records its server-derived ID, team/task identity, route, risk, work kind, reason
code, gate reason, confidence, actual actor, exact revision, team/task revision,
server UTC time, opaque evidence references, scope/rollback references and follow-up.
Task history and the durable team update record use kind `risk_route`, never a
reviewer verdict. Free-form tool output, transcripts, secrets and credential-bearing
URLs are not evidence fields. Receipt references are 64 lowercase hex characters;
reason is a bounded machine code. Producers must keep raw evidence in their protected
store and resolve references under the existing permission boundary.

A stored decision is historical metadata, not a reusable authorization token. Any
future consumer changing workflow state must revalidate current revision, evidence,
AC, findings and approvals. `decideRoute` itself does not transition ready/done,
queue/drop reviews, merge, dispatch a tool, or grant user permissions. Merge and
acceptance are independent steps under the existing integration/release policy.
This foundation must itself receive ordinary independent reviewer review.

## Contract and verification

See [team-risk-routing.md](../team-risk-routing.md) and the synthetic fixtures in
`tests/daemon/fixtures/team-risk-routing-v1.json`. `ketplus_team_risk_policy_tests`
uses a real temporary journal and a fake trusted host, without a provider, real
account or filesystem worktree. Regression coverage includes allowed low-risk,
mandatory/unknown domains, missing evidence, failures, stale/replayed revisions,
wrong actors, incomplete AC, review history, advisory calls and legacy persistence.

## Consequences

The default adapter makes this safe to land before receipt/tool/UI integration:
changes continue through reviewers until the host can prove an exception. Receipt
production is a trust boundary and must be reviewed with its own semantic coverage
and required-check tests. This feature neither implements automatic resource cleanup
nor changes signing, deployment, user approvals or release behavior.
