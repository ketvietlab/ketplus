# 0130: Host-verified agent routing tools

Status: proposed (implementation under review)
Date: 2026-11-16

## Context

ADR 0129 deliberately defaults to no trusted evidence. Exposing a decision tool
alone would leave low-risk decisions unusable; accepting caller-submitted risk or
check JSON would turn the tool into an authority bypass. Advisory work also does
not need a fabricated developer/reviewer task.

## Decision

Expose authenticated agent tools for host verification and leader route decisions.
Bind the actor to the existing agent token, never a payload session or the `user`
alias. Forward strict decision fields through the existing coordinator policy.
Local/remote user clients cannot adopt a leader's identity.

Use a small independent, read-only host recipe for a provable mechanical subset:
restricted plain-prose paragraph reflow with equivalent word/punctuation/paragraph
content in existing regular documentation blobs. Paths only narrow the search;
the content proof covers the actual whole delta from the recorded task base.
Unsupported semantics, scope, modes, provenance, checks, findings or approvals
remain unknown and require reviewer routing. Extending the policy domain list
requires explicit validator coverage; a new domain is not automatically clear.

The daemon chooses and executes the required Git/semantic checks, then mints an
immutable bounded-lifetime receipt bound to exact commit, task/team revisions and
scope. No repository scripts or provider calls run. The protected in-memory store
retains proof metadata and opaque references, without transcripts, secrets or raw
outputs. Caller IDs only look up receipts. State changes/expiry/restart invalidate
lookup; the existing policy rechecks the clean live head before recording a route.
All proof operations disable Git replacement objects. Diff/status operations
force submodule visibility so repository-local ignore settings cannot hide
gitlinks from mode/scope validation.

Explain routes in agent instructions: leader answers advisory work directly;
developers submit evidence once; verified low risk can be assessed by the leader;
high/unknown risk or an explicitly reasoned conservative request uses a reviewer.
Confidence is supplementary and never creates authority. Existing user approval
is respected, while an action outside it still needs the user.

Route decisions remain historical audit and do not transition task state, merge,
enqueue/drop reviews or manufacture reviewer verdicts. A future workflow consumer
must revalidate current evidence, revisions, findings, acceptance and permissions.
This implementation itself follows the existing independent reviewer process.

## Verification

The runtime regression uses temporary real Git repositories, the real producer,
host receipt lookup and coordinator policy with fake provider infrastructure.
It covers advisory, valid mechanical low-risk, unsupported/high-risk semantics,
forged fields/IDs, wrong roles, dirty/stale scopes and receipt invalidation. Prompt
fixtures and task projections keep leader conclusions distinct from reviewer
verdicts and actual workflow state. See the fixed recipe and failure boundaries in
[team-risk-routing.md](../team-risk-routing.md).
