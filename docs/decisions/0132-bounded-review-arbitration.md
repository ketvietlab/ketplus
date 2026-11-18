# ADR 0132: Bounded content review and evidence-backed finding resolution

Status: proposed; implemented and locally validated, awaiting independent review.
Date: 2026-11-17

## Context

A lost checkout is not evidence of a reviewer changing it. Counting infrastructure
failures as content review, automatically retrying missing source, or accepting a
free-form review URL can respectively exhaust a review budget, loop indefinitely,
or bypass material findings. This decision supersedes the corresponding retry,
manual acceptance and execution-budget behavior in ADR 0114. ADR 0129 still owns
initial routing; a task with review history cannot reclassify itself through it.

## Findings

A reviewer may include exactly one `ketplus-findings-v1` fenced JSON object before
the existing verdict line. Version 1 has `version: 1` and `findings`, at most 32
unique entries. Each entry requires:

| Field | Values |
| --- | --- |
| `id`, `close_when` | Bounded lowercase machine codes |
| `severity` | `p0`, `p1`, `p2`, `nit`, `unknown` |
| `impact` | `requirement`, `required_check`, `behavior`, `preference`, `unknown` |
| `scope` | `task`, `outside_task`, `unknown` |
| `evidence` | A 64-character lowercase SHA-256 reference; explain evidence in the review |
| `mandatory_rule` | Explicit boolean |

The daemon binds these facts to the reviewer, content round and reviewed full
commit in task `review_findings`. Resolution fields are server-owned. Legacy,
malformed, contradictory or unknown findings remain blocking. A new independent
review may clear earlier findings; a contradictory `no findings` verdict with a
blocking structured finding cannot make the task ready. A preference nit is
nonblocking only when its scope is known and it violates no mandatory rule.

P0/P1 and unknown material findings cannot be closed or deferred by leader
verification. P2 violations of requirements or required checks must be fixed.
Only a nonmandatory P2 outside task scope with known behavioral/preference impact
can be deferred, with an owner and reason. Deferral still requires verified low
risk, completed acceptance and an authoritative receipt; severity is not proof.

## Leader decision contract

The existing coordinator `acceptReview` entry point receives a versioned JSON
decision in its `evidence` string and a bounded reason code in `reason`. A URL or
free-form assertion grants no exception. Agent/UI adapters must preserve this
contract and display daemon refusals. No reviewer verdict is manufactured.

```json
{
  "version": 1,
  "action": "resolve_findings",
  "confidence": 99,
  "expected_team_revision": 21,
  "expected_task_revision": 20,
  "head_commit": "cccccccccccccccccccccccccccccccccccccccc",
  "evidence_id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  "resolutions": [{"id": "f1", "status": "fixed", "reason": "bounded_fix_verified"}]
}
```

`status` is `fixed` or `deferred`; only `deferred` includes an `owner` code. Confidence must be an integer above 90 for resolution and is only a supplementary
signal, never authority. Unknown fields, duplicate IDs, fractional revisions and caller risk/check assertions are
refused. A same-team leader, distinct from the task owner, must supply exact team
and task revisions. The daemon independently reads the clean current Git HEAD,
checks acceptance, dependencies, integration target and pending user decisions.
An active/queued review cannot be cancelled by this decision. A committed decision
changes the revision fence, preventing replay. Ready is distinct from merged/done.

The trusted host receipt must bind team/task/task revision, the reviewed commit
and the current fix commit. `VerifiedEvidence.reviewedRevision` and
`localFixVerified` extend ADR 0129 with fail-closed defaults. All mandatory risk
domains, independently selected required checks, scope/rollback evidence, approval
and unresolved-failure gates still apply. Targeted verification never replaces the
full matrix required by `docs/testing.md`. Raw output, credentials and transcripts
are not stored in the new structured audit.

**Supported host recipe:** initial-routing `prose_reflow_v1` remains round-zero
only. The separate `prose_line_width_fix_v1` recipe is selected server-side from
the current `Task.reviewFindings`, never a caller recipe or pass/risk assertion.
It supports only open, nonmandatory P2/nit findings with known behavioral/preference
impact, task scope and `close_when: prose_line_width_80`. Mandatory requirements,
required-check findings, P0/P1, legacy records and other closure conditions require
independent review; the generic consumer model does not imply a producer for every
P2. Nonblocking preference nits may remain in the record without being resolved.

A second narrow recipe, `prose_preference_deferral_v1`, supports P2 outside-task
preferences explicitly marked nonmandatory with
`close_when: owner_accepts_outside_preference`. Current HEAD must equal the reviewed
commit; the complete task delta must still satisfy the same strict prose proof.
The host emits `reviewed_preference_nonblocking`, not a local-fix assertion. The
consumer requires that independently passed required check, the same reviewed/current
revision, a leader reason and a follow-up owner. Behavior/requirement/mandatory or
unknown deferrals remain unsupported. This does not label an unfixed issue fixed.

The producer enumerates the union of all paths in base→reviewed, reviewed→HEAD and
base→HEAD. Every version must be a regular existing plain-prose document, with the
same paragraphs and words under the strict initial-routing grammar. For the
line-width recipe, the reviewed version must demonstrate a line longer than 80 Unicode scalar values; every line
of each resulting document must be at most 80. The reviewed commit must match the
server record/task head, be a descendant of base and an ancestor of current HEAD.
Any unsupported delta, mode, missing object or unproven condition falls back to
reviewer. This proves a mechanical correction, not general semantic bug fixing.

All Git proof/lookup calls disable replacement objects. Diff/status operations
include submodules regardless of local ignore configuration; enumerations disable
relative filtering. The same bounded host execution, final branch/clean/HEAD fences,
receipt TTL, scope and revision binding apply. The line-width receipt adds
`reviewed_prose_equivalence` and `reviewed_prose_line_width_80` checks and attests
`reviewedRevision`/`localFixVerified` only after that independent proof. The generic
runtime adapter returns the producer's recipe and uses the same store/lookup; it
neither accepts caller evidence facts nor grants authorization by minting an ID.

`TeamFindingRoutingTest` exercises real immutable objects, authenticated agent
verification, runtime receipt retention/lookup and leader resolution. The Gitlink
ignore and replacement-tree attacks must also be refused through that path.
Fake-host tests separately exercise denial/check/freshness transitions; they do
not substitute for the real producer integration or required native matrix.

Successful resolution records `findings_resolved`: route, risk, actor, reason,
reviewed/current revisions, receipt, per-finding disposition, server time and
follow-up. It does not write `review_accepted` or replace the reviewer's verdict.
Open/unknown findings block merge offers and merge completion checks.

## Bounded rounds and arbitration

Persisted `max_rounds` values 1–5 remain readable, but automatic execution uses at
most two content rounds (one when configured). Changing a setting or moving a task
between states does not authorize a third pass. At the limit the task blocks for
leader arbitration. The leader can resolve eligible findings, change the work's
scope through the ordinary task process, or explicitly request one further
independent review with a reason and exact revision fences:

```json
{
  "version": 1,
  "action": "request_review",
  "expected_team_revision": 21,
  "expected_task_revision": 20,
  "head_commit": "cccccccccccccccccccccccccccccccccccccccc"
}
```

This records `review_arbitrated` and binds `arbitrated_round`/`arbitration_head` to
one further content pass, without erasing the total round count or findings.
Another findings verdict blocks again; it never automatically authorizes another
pass. A changed head invalidates the authorization. Existing explicit voiding of
an incorrectly counted verdict retains its reason/audit and does not clear its
findings into acceptance.

## Infrastructure and source integrity

Unavailable local snapshots produce `review_interrupted` in broker request state,
owner notice, task history and review block. They consume no content round and
infer no reviewer edit. Readable differing source snapshots produce
`review_tampered` and remain blocking; they establish a change, not its author.
Neither event automatically retries. Restore/inspect the source, then explicitly
retry the review; stale callbacks cannot consume another round. Interrupted turns
also roll back their content-round reservation. Missing verdicts retain their
separate bounded retry, followed by a leader block.

This does not implement checkout leases or cleanup. Source must still remain
available during queued/active review, until the verdict and explicit reviewer
release required by the resource workflow.

## Verification

Policy/serialization fixtures cover unknown and legacy records, P0/P1 and mandatory
impact, safe nits, malformed/forged resolution inputs and baseline/check freshness.
Coordinator fixtures cover authority, replay, acceptance and explicit arbitration.
Fake-provider broker/team fixtures cover missing checkout versus readable source
changes through actual notices/request states, without provider quota.

The required full Pro matrix and review-guard sidecar tests remain release gates.
Tests being present or passing syntax checks do not establish execution results.
