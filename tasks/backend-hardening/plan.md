# Backend hardening execution plan

Updated: 2026-11-08. Owner: conversation #2739. Independent reviewer: #7705 (reassigned by the user on 2026-11-08).
Baseline: integration `90388ea772bf75267e66233ee66566bc0e2e18e8` (PR #212).

This is the durable execution queue for finishing the existing non-UI domain
review. It is not a claim that the open domains have passed. Update this file
with each completed work package, including evidence, remaining work and the
next ready item. Do not close a mixed UI/backend issue on backend evidence alone.

## Scope and boundaries

- Work in the public ketplus repository, isolated branches from integration; PRs
  target integration. Preserve public/private ownership and existing protocols.
- No UI implementation: no QML, widgets, layouts, themes, design-system changes,
  settings pages, transcript rendering, sidebar banners, mocks or screenshots.
  Running existing UI tests when the repository matrix requires them is allowed.
- Backend portions of mixed issues are explicit below. UI-only criteria remain
  out of this execution plan and stay open with their owning UI workstream.
- Keep SQLite. JSONL/blob separation and PostgreSQL were discussion, not an
  approved backend migration. Do not add that migration to this queue.
- Use synthetic journals/fake providers/loopback fixtures. No paid provider calls,
  user-journal mutation, installed-app replacement, release, relay deployment or
  device installation. Build/staging checks are distinct from deployment.
- Do not turn hardening into new product features. Media/archive work below is
  the existing #128 backend requirement; any step needing new UI stays deferred
  and must not silently change destructive defaults or old-client behavior.

## Execution rules

1. Resume from the first ready unchecked item, after verifying checkout, branch,
   instructions and current code. An interrupting question does not cancel it.
2. For each item: inspect the production path and existing tests; reproduce a
   concrete gap; implement the smallest owning-module fix; add meaningful
   regression; run the required matrix; record the final commit and evidence.
3. A review finding loops back to the same item. A PASS only unlocks the merge
   gate; it does not complete merge, cleanup, packaging or deployment.
4. Send one complete exact-head request to #7705. Do not poll the peer or bypass
   a held dispatch. The 2026-11-08 closure audit supersedes the previous rule to
   start another independent audit while waiting: freeze new audit branches,
   finish the already-started migration-copy package and drain the existing
   review queue by issue. Review/merge the #71 fixes and finish that issue's
   acceptance before starting the next domain; do not wait for all drafts to
   merge before preparing its closure. Allow at most one new unreviewed package
   needed for the current issue. Resolve its findings before advancing.
   Record external gates separately from unperformed work owned by #2739.
5. After exact-head tests and review pass, merge using the existing authorization.
   Address all P0/P1/P2 findings; do not self-accept a risk to empty the checklist.
   Lower-severity findings are either fixed or explicitly accepted by the user.
6. After pushing/opening a PR, review or merge, remove the task's linked worktree
   only when clean, pushed and not used by another conversation/process. Keep the
   main checkout. Save evidence first; source removal is the final command group
   of that turn. Recreate an isolated checkout for any follow-up.
7. A final response must state the next ready item and owner. No generic
   "continue?" gate. This file is the resume record; it is not a background
   scheduler or a replacement for the application's turn-execution mechanism.

## H0 — Inventory and established evidence

- [x] Verify integration base, repository ownership and non-UI scope.
- [x] Reconcile open domain issues with completed review streams. Browser #90–94,
  remote #84/#86–89 and Git/filesystem #63 are closed on GitHub; inherit their
  prior evidence, recheck only when a change affects those boundaries.
- [x] PR #212 merged at the baseline above after exact-head author and independent
  6/6 daemon targets passed; retention 123/123. Production GC predicate uses all
  four transfer-index keys; 25 lookups under 1 ms in the synthetic 300k-row probe.
- [x] Preserve limits of that evidence: no Windows, live journal, multi-GB index
  startup or release/install/deploy verification. Synthetic 9-GiB vacuum evidence
  from #202 does not prove full size-pressure/media acceptance.
- [x] Start the next storage audit. Static inspection finds global orphan cleanup
  in initialize/delete/every 256 command results, separate from the bounded
  detail lane. Record the reproduction below; do not treat #212 as fixing it.

## H1 — Storage, retention and media (#68, backend #128)

- [ ] H1.1 Bound global orphan GC and other retention sweeps by work, not only by
  final row count. Exercise raw and retained backlogs, many candidates, repeated
  item IDs and busy readers/writers. Inspect production correlated query plans;
  retain shared references and ACK/generation fences. **Current item.**
  - [x] Reproduce global-GC blocking in actual Qt reopen and command-result paths.
  - [x] Implement bounded transactional global metadata/chunk GC and focused regressions.
  - [x] Global GC #220: full Pro build; 90/92 first parallel run plus 2/2 sequential
    adapter-timeout retries without source changes. Exact-head #6212 PASS; merged
    `35ab06f7bccf01c3dfd7dc347d991bd8020f7bee`. Generated build removed; source cleanup
    is tracked separately until verified.
  - [x] Reproduce the separate Qt ACK backlog path (7,183 ms) and implement bounded
    durable queued raw cleanup; focused ACK/protocol/transcript regressions pass.
  - [x] ACK #223: final-head full Pro 93/93 and independent 9/9 affected targets;
    merged `1bdaee5b608da4ef0a958b5888e83859dd920dee`. Source/build cleanup is
    tracked separately until verified.
  - [x] Reproduce command/audit backlog DELETE and clock-rollback result loss.
  - [x] Implement bounded command/tombstone/audit/cursor/task lanes with rollback,
    actual-query, byte-budget, reopen and orphan-generation regressions (ADR 0050).
  - [x] Metadata #228: final-head full Pro 94/94 and independent wrap-delta review
    PASS; merged `eca3bad52d61c51f250df4d51594bad03f2b0226`. Source/build cleanup
    remains separately tracked until verified.
  - [x] Reproduce staged-attachment expiry backlog and invalid-input maintenance;
    implement bounded retirement/chunk cleanup with identity-reuse and fault tests.
  - [x] Staged attachment #230: final head `ceb5476a` passed full Pro 95/95 and
    independent exact-head 4/4 targets; merged `8f0858bc67b0eba99b9b5529bf5d123f2b179678`.
    Source/build cleanup remains separately tracked.
  - [x] Reproduce global deletion-completion sweeps and implement bounded completed
    work GC plus indexed pending pages; malformed/ignored completion regressions.
  - [x] Completed-deletion #235: final-head affected Release/headless 10/10 and
    independent exact-head review PASS; merged `f0d73d30d49d06dab8290edf01971e1a3a06fa18`.
    Generated build removed; source cleanup remains tracked separately.
  - [x] Cloud cache #237: exact invalidation, per-envelope receipt and scoped
    recovery cursor; final head `4e654c1a` full Pro 96/96 and independent PASS.
    Merged `0f6f39e05a007d889064760ffc64b6f1673851ac`. Generated build removed;
    source cleanup remains separately tracked.
  - [x] Reproduce explicit attachment cancellation bounds and preservation gaps;
    implement shared 1-MiB/128-candidate cancellation and retirement fencing.
  - [x] Cancellation #240: final head `b4d49e6b`, affected Release/headless 12/12
    (98.81 s), independent exact-head PASS after unsupported revision projection fix;
    merged `0455bd89919a28c7b17f832ed4ef97bb8bef80bf`. Cleanup remains tracked separately.
  - [x] Session deletion transaction recovery #241: final head `71607352`,
    affected Release/headless 12/12 (55.64 s), independent exact-head PASS;
    merged `187566a6bad4626f1d5380cd51ab1e24795291b6`. Cleanup remains tracked separately.
  - [ ] Finish explicit deletion work bounds; cloud-event cleanup #237 is merged.
  - [ ] Close H1.1 only when those remaining work bounds have evidence.
- [ ] H1.2 Verify migration/reopen safety: old schema, repeat open, partial failure,
  unknown/newer formats, one-time index creation and low-space conditions. Add
  explicit compatibility handling only after reproducing the actual gap.
- [ ] H1.3 Audit transactions and failure paths for append, ACK, compaction, delete,
  fork and blob cleanup. Inject write/commit failure and process interruption;
  assert no committed history loss, false success or wrong-session deletion.
- [ ] H1.4 Audit media paths, traversal/symlink handling, owner-only permissions,
  decode/pixel/byte limits, shared references and orphan cleanup. Resource-bearing
  and unsupported shapes must remain recoverable, not silently emptied.
- [ ] H1.5 Complete the approved #128 media/archive backend contract and tests:
  bounded image preview/caption retention, safe large-payload handling above the
  current lane, authoritative archive state before expiry, and old-client
  compatibility. Keep unsupported/destructive options inactive until their
  contract and consumer behavior are safe; UI controls remain out of scope.
- [ ] H1.6 Verify pressure ordering across oldest eligible turns, expired before
  recent detail, free pages first, no-limit/Forever/off semantics, and revision
  changes during a sweep. Preserve running/latest-20/pending/ACK guards.
- [ ] H1.7 Run representative synthetic multi-GB reopen/index/retention/reclaim and
  crash-recovery acceptance. Record retained-row hashes, integrity, idempotence,
  peak temporary disk/memory, pass latency and cleanup. Test actual payload mix;
  do not claim a free-page-only fixture proves bounded storage in steady state.

## H2 — Daemon lifecycle and local IPC (#65–67, #69)

- [ ] H2.1 Socket/token permissions, peer authorization, single instance, stale
  socket handling, version handshake and controlled shutdown/restart.
- [ ] H2.2 Partial/oversized/malformed/slow frames, bounded outbound queues and
  reassembly, disconnect cleanup, request timeouts and reconnect backoff.
- [ ] H2.3 Turn transitions under crash/cancel/restart, late events, duplicate
  input, two clients, handoff and worktree switch; no lost pending input or stuck
  running state. Bound windows/ephemeral reads without touching rendering.
- [ ] H2.4 Approval/question authorization, one-shot resolution and turn fences;
  timeout/cancel cleanup; fork/delete isolation and execution-context guards.
- [ ] H2.5 Agent-message identity/reply matching, durable delivery/restart,
  hop/rate/queue limits, expiry, caller authentication and child-process secret
  isolation. Use fake agents; never spend provider quota for these tests.

## H3 — Provider adapters and credentials (#71–75)

- [ ] H3.1 Common framing: huge/no-newline/binary/invalid-UTF8 input, stderr drain,
  bounded buffering, version-probe timeout/cache and honest capability flags.
- [ ] H3.2 Claude sidecar framing/crash/resume/worktree identity, bounded background
  tasks, pinned packaging and absence of runtime network installs.
- [ ] H3.3 Credential/keychain failure handling, sign-out clearing, request
  timeouts and no secrets in logs/journal/child environment. Exclude settings UI.
- [ ] H3.4 Codex request-ID matching, unknown/late responses, restart/zombie
  cleanup, thread/fork/worktree identity and defensive rate-limit parsing.
- [ ] H3.5 Cursor unknown/malformed events, cancellation/process cleanup and
  browser-session ownership. Real-provider acceptance remains separate.

## H4 — Sync, privacy and remote compatibility (#85; backend #77/#78)

- [ ] H4.1 Verify projection redaction for paths/content/secrets and oversized or
  malformed events; test what actually crosses the transport boundary.
- [ ] H4.2 Offline queue/batch bounds, reconnect/backoff, idempotent restore,
  stale revision/generation rejection, catch-up ordering and deduplication.
- [ ] H4.3 Audit nonvisual session-sync/paging boundaries only. Do not edit Qt
  presentation models, rendering, notifications or viewport/layout behavior.
- [ ] H4.4 Reconcile relay challenge-v2 readiness evidence: target migration,
  running revision on every replica, legacy flag, v2 round-trip, cutoff and owner.
  Client/relay code merge is established; rollout is **waiting on relay operator
  evidence**, not complete. Continue other local items while this waits.

## H5 — Nonvisual process and tunnel boundaries (#97/#103/#105)

- [ ] H5.1 PTY/process backend only: shutdown, zombie cleanup, environment/cwd,
  bounded buffering and signals. Coordinate ownership before edits; skip terminal
  view/panel/clipboard interaction and all other UI criteria in mixed #97.
- [ ] H5.2 Atlas process launch/version/arguments, executable resolution, loopback
  exposure and lifetime. Exclude Atlas tabs/viewers/builder UI (#104).
- [ ] H5.3 Atlas tunnel path confinement, codec/frame limits, authorization,
  revoke/disconnect/teardown and malformed input. Recheck browser boundaries only
  if these fixes affect the already reviewed integration.

## H6 — Build, dependency and release-path hardening (#107–110)

- [ ] H6.1 Dependency/action pins, integrity and licenses; Lite/Pro ownership;
  warnings policy and reproducible sidecar/runtime packaging.
- [ ] H6.2 Update-feed/signature/channel/downgrade logic and publish-script failure
  handling, with fixtures/dry runs. No update settings UI or real publication.
- [ ] H6.3 Staged bundle/dependency/entitlement checks and Linux runtime checks.
  Record signing/notarization or OS prerequisites that cannot run locally; never
  equate a successful build with a signed package or installed device.
- [ ] H6.4 Workflow permissions, fork-secret isolation, pinned actions, promotion
  guard bypass cases and test-matrix coverage. Do not weaken CI to make it green.

## H7 — Integration, review and completion gates

- [ ] Every in-scope issue has a source audit and disposition of every finding.
  Split natural ownership boundaries when needed; avoid unrelated refactors.
- [ ] Focused tests plus directly affected targets pass at final head. Shared
  header/shell changes require full Pro; core changes require Lite then Pro.
  Release performance probes assert query/work bounds, not only loose timeouts.
- [ ] Run final relevant integration matrices (daemon, sidecar/browser when
  affected, Lite/Pro per scope); report actual per-target results and skips.
- [ ] Every implementation PR has independent exact-head review, resolved
  findings, a verified merge and safe worktree/build cleanup.
- [ ] Update project status/testing/architecture for real behavior changes and
  link durable evidence. Retain open UI criteria on mixed parent issues.
- [ ] Provide a completion ledger separating code, tests, build, staging,
  signing/package, deployment and installation. The last two are not authorized
  by this plan; missing external verification remains visibly outstanding.

## Evidence and current queue

The [closure ledger](closure-ledger.md) is the current dispatch order and issue
disposition. The historical package rows below are evidence, not completion of
their parent issue. No issue is closed solely because several PRs or tests pass.

| Item | State | Evidence / remaining work | Next action / owner |
| --- | --- | --- | --- |
| H0 | Complete | integration inventory; #212 merge/review and inherited limits reconciled | None |
| H1.1 | In progress | Global GC #220, ACK #223, metadata #228, staged expiry #230 and deletion work #235 and cloud #237 merged; transaction recovery #241 merged; cancellation #240 merged; explicit session-delete bounds still open | Audit explicit session deletion and tombstone enumeration / #2739 |
| H1.2 | In progress | Startup commit failure #250 merged; other migration/read/crash cases remain | Continue remaining failure-path audit / #2739 |
| H1.4 | In progress | Cleanup #251 and dedup #252 merged; publication #253 and fork #254 full suites PASS; review dispatch held; preview #255 affected matrix PASS | Restart exact-head review / user and #6212; continue H3.1 / #2739 |
| H1.5–H1.7 | Not started | No claim that #128 backend acceptance is complete | Continue storage acceptance / #2739 |
| H1.3 | In progress | Delete/ownership #241–243, append #244–245 and fork #246–247 merged; stream recovery #248 merged; replay/activity #249 merged; startup #250 merged | Continue caller atomicity/read failures / #2739 |
| H2–H3 | Partially started | H2.3 volatile retry subset #248 merged; full lifecycle and provider audits remain | Continue remaining daemon/provider checks / #2739 |
| H4.1–H4.3 | Not started | Existing remote fixes are not a full #85 closure | Audit local transport/privacy with fixtures / #2739 |
| H4.4 | Waiting, external | Relay code merged; no deployment/readiness evidence supplied | Relay release operator, unassigned; #2739 tracks gate |
| H5–H6 | Not started | Nonvisual subsets explicitly scoped | Audit after earlier safety work / #2739 |
| H7 | Pending | Requires all applicable packages and evidence | Integrate and close only verified criteria / #2739, review #6212 |

### Initial H1.1 audit (baseline commit above)

`LocalJournal::initialize()` calls `pruneRetention()`. That routine also runs
following session deletion and every 256 command results. Its orphan scan selects
all large-payload candidates; the raw-event arm has no per-pass work limit. The
whole-generation ACK guard added in #212 belongs to the detail lane and does not
cover this global path. This is a candidate performance finding, not yet a
verified data-loss bug. A Qt regression and failure/rollback audit are next.

The isolated Python/SQLite 3.43.1 probe extracted the SQL predicate from production
source at this baseline, with the same raw-event and replay-transfer indexes.
It used 32 candidate metadata rows, no replay references, and either zero or
100,000 raw events (each a ~1 KiB JSON payload) in the same session/generation.
The candidates had no matching raw reference. Three SELECT-only repetitions:

| Raw rows | Candidate rows | Elapsed ms |
| --- | --- | --- |
| 0 | 32 | 0.06 / 0.04 / 0.03 |
| 100,000 | 32 | 4435.48 / 4701.39 / 4719.78 |

The plan scans candidates and seeks raw events only by session/generation before
checking each JSON payload. This demonstrates unbounded candidate × raw-backlog
work in this synthetic query, not measured latency of the Qt production process.
No user data was read or changed; temporary databases were removed. Raw results:
[evidence/h1-global-orphan-scan.json](evidence/h1-global-orphan-scan.json).

### H1.1 global GC implementation

The production Qt Release regression on the original source reproduced 7,172 ms
at reopen and 7,066 ms on the 256th command result. The bounded implementation
reduced the same fixtures to 22 ms and under 1 ms in the latest focused run;
300,000 retained rows sharing an item ID took 12 ms in the earlier focused run.
The 45-MiB chunk fixture completed in 45 passes, each deleting at most 1 MiB.
Focused tests: 21/21, including ignored writes, commit failure and rollback,
persistent cursors, generation/shared-reference fences, busy writer and WAL reader.
These are synthetic macOS/Qt results, not Windows/live-journal/multi-GB acceptance.
Raw measurements and mutation outcomes:
[evidence/h1-global-gc-qt.json](evidence/h1-global-gc-qt.json).

The next sweep audit confirms full scans in command/audit age pruning and global
cap DELETEs. In a preliminary in-memory SQLite fixture, 100,000 command rows make
the cap delete remove 90,000 rows in one statement (about 2.44 million VM steps),
versus zero removals for 10,000 rows. This is SQL-only evidence, not Qt acceptance;
[evidence/h1-other-retention-sweeps.json](evidence/h1-other-retention-sweeps.json)
records plans and measurements. Reproduce the actual journal paths before fixing
these in the next package; the GC change does not bound them.

### H1.1 ACK cleanup

An independent temporary Qt harness reproduced 7,183 ms for ACK of 100,000 raw
rows with 32 large-payload metadata candidates after the global-GC fix. The queued
implementation reduces the same public ACK path to 1 ms, deleting only 128 raw
rows on its first pass. Cursor/queue acceptance is atomic; raw deletion is eventual
and fenced by the current slowest client. Focused ACK 20/20, protocol 43/43 and
transcript window 28/28 have passing evidence. The latest ACK run includes actual
prepared query plans and capability/NUL fixtures; the final full matrix remains
an explicit gate. See ADR 0049 and the evidence JSON.

### Next H1.1 command-cap reproduction

The temporary Qt probe at the ACK branch head confirms that the 256th command
write removes 90,256 rows from a 100,256-row fixture in one pass (341/337 ms).
The fixture deliberately uses future timestamps to isolate the cap from age GC;
it additionally shows that after wall-clock rollback the newly accepted command
can be absent immediately after the timestamp-ordered cap. Preserve that command's
idempotency result in the next package; do not infer ordinary steady-state timing
from this clock-skew fixture. Evidence:
[evidence/h1-command-cap-qt.json](evidence/h1-command-cap-qt.json).

The full ACK matrix exposed a handoff fixture's immediate-raw-deletion assumption.
Its regression now compares the complete retained handoff history before and after
explicitly draining bounded cleanup; all six handoff cases pass. This is an
intentional physical-cleanup contract update, not a skipped handoff assertion.
The retention fixture likewise drains acknowledged raw rows explicitly. Its new
regression verifies that large detail remains untrimmed while raw cleanup is queued,
then becomes eligible after drain. Retention 124/124 passes with the original
reference/fault/budget assertions intact. Final full Pro rerun is still required.

The independent review found a P2 in skipping unsupported raw rows: deleting
later cursors made an interior gap invisible to `session.events_after`'s retained
prefix check. Four new SessionHost head/middle × oversized/binary cases reproduce
it on the old implementation. The fix removes only a contiguous acknowledged
prefix and rotates blocked generations for fairness; it preserves the entire
unsupported tail. Author verification: 4/4 targets PASS (ACK 24/24, usage retention
124/124, handoff 6/6, transcript window 28/28); mutation reintroducing the skipped
row/suffix gap is caught. The full run on the earlier head was cancelled after
this P2 was confirmed. Exact-head full/review gates must be rerun before the P2 is
considered independently resolved.

The final ACK head `658325f1b63dde44668841827191d08b7b183bb8` passed full Pro
93/93 and independent 9/9 affected targets, including ACK 24/24 and retention
124/124. Review resolved the prefix P2; #223 merged at
`1bdaee5b608da4ef0a958b5888e83859dd920dee`. Earlier pending-gate notes above are
historical; source/build cleanup remains separately tracked.

### H1.1 metadata retention

The committed Qt regression on the #223 baseline removes 90,256 rows on the
256th command/audit write (359/159 ms) and loses the newly accepted result after
clock rollback. Bounded lanes remove at most 256 rows and 1 MiB per table, use
accepted-write rowid order for caps, and persist age/cap progress transactionally.
Focused regressions cover reopen, shared age/cap byte budget, unsupported values,
orphan generation isolation, busy/WAL readers, cursor wrap and failure rollback.
Focused suite: 20/20. Mutation checks catch doubled row and 16-MiB byte budgets;
[Qt evidence](evidence/h1-metadata-retention-qt.json) records the before/after fixture.
Full matrix and exact
head review remain gates before merge. Evidence is synthetic; no live journal,
Windows, deployment or installation acceptance is implied.

The first full Pro run passed 93/94 targets. Its existing transcript-window expiry
case exposed an empty-tail pass that only reset the cursor, delaying the reopen
recheck. The fix wraps an empty tail once in the same pass without increasing the
candidate budget; the original expiry assertion remains intact. Focused and full
gates must pass again at the revised head.

The revised metadata head `6b489251957e744c6864b7b0d9335071a2cba993` passed the
full Pro 94/94 matrix and independent exact-head review (metadata 20/20,
transcript 28/28 plus empty/nonempty-wrap probes). #228 merged at
`eca3bad52d61c51f250df4d51594bad03f2b0226`; earlier pending gates are resolved.

**Next executable task:** finish cloud-cache full Pro/review/merge gates;
while review runs, audit explicit cancellation/deletion separately. Do not
mark H1.1/H1 complete on these subsets.


### H1.1 staged-attachment expiry reproduction

A temporary Qt Release probe of `beginAttachment` with 160 staged files and
102,400 chunks (50 MiB) took 105 ms when none had expired, and 554 ms while deleting
the entire expired backlog. The operation starts an upload in another session;
it should not perform backlog-sized global work. The regression package will
bound both candidate scans and deleted bytes, preserve current drafts/generations,
and prevent reuse of an attachment identity until all old chunks are gone.
[Qt evidence](evidence/h1-attachment-expiry-qt.json). Implementation now passes
20/20 focused cases, including actual prepared plans, generation/revision and
unknown-parent guards, reopen/ID reuse, unsupported rows, abort/ignored mutations,
commit rollback, busy deferral and capability absence. The committed baseline
regressions fail both the work bound (102,400 chunks deleted, 680 ms) and the
invalid-input side-effect check. The bounded pass deletes 128 chunks in 20 ms.
Byte-budget and missing-identity-fence mutations are caught; restored tests pass.
Full Pro and independent exact-head review remain pending; ADR 0051 records the
contract and limits. The former LocalJournal attachment implementation moved to
its owning module without changing explicit cancel/delete semantics.


The first staged-expiry head `b35a7caba8f892f161cdc776598e83d06e86337b` passed full
Pro 95/95. A subsequent boundary audit reproduced four missing logical-expiry
checks (begin/read/commit/chunk) for an expired identity beyond the 32-manifest
window, plus an expired-draft quota obstruction. Those regressions fail on the
prior implementation; current drafts still trigger the unchanged quota. Direct
eligibility checks and usable-only quota counting now pass focused 4/4 targets:
staging 26/26, protocol 43/43, cloud privacy 60/60, lifecycle 28 pass/1 live skip.
The final full suite and independent review must run at the revised head; the
older 95/95 is not carried forward as final-head evidence.


Independent review of staged expiry found a P2: cancellation could remove a
retirement marker while other chunk revisions remained, then a new begin reused
that identity and encountered stale chunks. Five regressions reproduced the
retiring/current mixed-revision cases, stale cancellation, ignored manifest
removal and a failed commit that left the transaction open. Cancellation now
preserves retiring identities, checks the current revision, retains a marker
when other chunks remain and rolls back failed finalization/commit. Staging
32/32 passes (including ignored marker update); affected production matrix 4/4
passes. Both preceding staging heads passed full Pro 95/95, but the cancellation
fix requires its own final full/review gates. Explicit current-upload cancellation
work bounds and downgrade of mixed-revision retirement remain separate H1.1/H1.2
acceptance; no release/rollback claim is inferred.


Final staged-expiry head `ceb5476add8844cc53de3e3fa4e4521e31610e47` passed full
Pro 95/95 (263.98 s). Independent exact-head review passed all four affected
targets and the original cancellation probe. #230 merged at
`8f0858bc67b0eba99b9b5529bf5d123f2b179678`. Earlier pending full/review gates above
are resolved; generated build and linked-source cleanup is tracked separately.

### H1.1 completed deletion work

A synthetic Qt completion over 100,000 already completed jobs took 413 ms and
removed 100,001 rows. Completion now writes only its own durable receipt/target
list, and a sixth bounded metadata lane removes completed rows. Pending pages
seek state plus rowid with a shared suffix/prefix limit; an accumulated completed
backlog cannot force a global scan/sort. Baseline regressions also exposed ignored
updates reporting success and malformed provider-target JSON becoming completed.
The focused suite passes 32/32, and all 10 affected Release/headless targets pass
(63.25 s). Replacing the pending-state index with a session-ID index makes the
actual prepared-plan regression fail; restored sources pass. No shared interface,
CMake/core or app composition changed, so the documented affected matrix applies.
Independent exact-head review passed; #235 merged at
`f0d73d30d49d06dab8290edf01971e1a3a06fa18`. Generated build removed; linked-source
cleanup remains separately tracked.
See [ADR 0052](../../docs/decisions/0052-bounded-completed-deletion-work.md) and
[Qt evidence](evidence/h1-deletion-work-qt.json). Per-record JSON size, index startup,
explicit session deletion and tombstone enumeration remain open.

The independent cloud-cache probe removed 100,000 cached 1-KiB rows in 269 ms.
The same API serves both successful upload ACK and policy/key invalidation;
async cleanup must preserve invalidation semantics before changing that path.

### H1.1 explicit attachment cancellation

A current synthetic 640-chunk upload removed all 20 MiB in one synchronous
cancellation. Unknown parent state and an oversized manifest were also discarded.
Cancellation now reads at most 128 candidates across all revisions, reserves
manifest cost in a shared 1-MiB mutation budget and commits a retirement marker
when chunks remain. The identity stays unavailable until bounded maintenance
finishes; unsupported data remains recoverable.

Focused Release/offscreen tests pass 41/41. Mutations increasing the row or byte
budget, or moving revision filtering into the SQL range, each fail regression.
The actual production query uses the identity primary index without a sort; a
100k other-revision backlog does not expand the candidate read. The affected
10-target Release/headless matrix passes (76.98 s), with the live lifecycle case
skipped by design. Independent review and merge are pending. See
[ADR 0054](../../docs/decisions/0054-bounded-attachment-cancellation.md).
Explicit session deletion, tombstone enumeration and large-record recovery remain
open; neither cancellation nor prior expiry evidence completes H1.1/H1.2.


Deletion-work final head `7ffc9709325c9ef740df3f47f39e6592c5c0cae5` passed independent
10/10 affected targets with no source changes. #235 merged at
`f0d73d30d49d06dab8290edf01971e1a3a06fa18`; earlier pending review/merge gates above
are resolved. Linked-source/generated-build cleanup remains separately tracked.

### H1.1 cloud cache

The initial cache cleanup branch passed cache 18/18 and privacy 62/62, but a deeper
audit reproduced an unsafe deferred-GC assumption: an old cloud cursor deleted a
newly regenerated cache entry after workspace/account reset. That regression failed
with actual remaining rows 0 instead of 1. The implementation now uses receipts
for the exact captured ciphertext; pending upload clears its receipt, partial ACK
marks only accepted matching envelopes, and old account callbacks cannot write
progress or receipts. Legacy cache migrates to an unmarked state and is retained
for a later verified receipt or separate recovery.

ACK cleanup keeps 256-row/1-MiB lane bounds. Invalidation removes one exact entry
and must succeed before upload/reconciliation proceeds. Initial backlog comparison
was 100,000 rows/269 ms versus 256 rows/2 ms; exact invalidation took under 1 ms.
The 1-MiB invalidation-budget mutation was caught. Final receipt regressions pass cache 26/26 and privacy 63/63 (2/2 targets, 8.72 s),
including legacy migration. Removing ciphertext matching or the stale account
callback guard makes its regression fail; restored sources pass. Full Pro and
independent review remain pending. The shared header/CMake changes require
full Pro. [Qt evidence](evidence/h1-cloud-cache-qt.json).

Ordinary cache read/JSON size, malformed empty-object and legacy unmarked-cache
recovery remain H1.2/H4.1 checks. Explicit deletion/current-upload cancellation,
tombstone enumeration and mixed multi-GB acceptance remain open. No UI, live
journal, release or installation scope is added.


Cloud-cache review at `b007cb59` found P2: mirror/raw progress preceded receipt
storage, so a failed receipt was skipped by all later uploads and remained forever.
The first full build also exposed one protocol test calling the removed cleanup
API. The revised package uses an atomic SQLite receipt + scoped recovery cursor,
then mirrors credentials/raw ACK; registration recovers after a crash or later
write failure. The protocol smoke now exercises that contract. Added the reviewer
transport regression plus credential-failure/restart and atomic progress probes.
All final matrix/review gates must run again at the revised head; the original
focused result is not final acceptance. No merge or release has occurred.


The cloud-cache recovery revision passes 4/4 affected targets (14.56 s): cache
27/27, privacy 65/65, protocol 43/43, metadata 32/32. It includes the reviewer's
ignored-receipt retry/reopen probe, identical ciphertext on retry, and credential
failure followed by restart/cache reclamation without resending an accepted event.
Scoped progress and receipts roll back together on ignored writes or failed commits.
Upload-progress handling now has its own owning file; the sync file is below 500
lines. Final head `4e654c1afe25774ba48ffa99643f2e8c65f593b7` passed full Pro
96/96 (321.03 s) and independent exact-head review. #237 merged at
`0f6f39e05a007d889064760ffc64b6f1673851ac`. Generated build removed; linked-source
cleanup remains separately tracked.

Independent review of #240 found a read-time bound gap: unsupported revision
TEXT/BLOB values were projected verbatim before candidate filtering. Both new
actual-query regressions reproduced 64 MiB of returned metadata (32 malformed
rows) while deleting no chunks. Projection now emits only integer or NULL;
revision filtering still follows LIMIT. Final affected matrix and exact-head
re-review are required before merge.

### H1.3 session deletion failure recovery (prioritized safety finding)

Four synthetic Qt regressions fail on the prior source: failed COMMIT leaves a
transaction open, ignored tombstone/work inserts report deletion success, and an
ignored raw-history DELETE commits incomplete cleanup. The transaction now has
one rollback guard; mandatory inserts require one affected row and each existing
history-table DELETE must leave no row for that identity/generation.
Focused provider deletion passes 10/10, including retry/reopen and unrelated-session
preservation. The 12-target Release/headless matrix passes (75.49 s), with one
live lifecycle case skipped deliberately. Independent exact-head review passed; #241 merged at
`187566a6bad4626f1d5380cd51ab1e24795291b6`. Final formatted head passed 12/12
(55.64 s). Source/build cleanup remains separately tracked. See [ADR 0055](../../docs/decisions/0055-session-delete-transaction-recovery.md).
This does not finish bulk-delete work bounds or previously committed-state recovery.

Next safety audit: provider cleanup reads `sessions()` to protect shared threads,
but that method also returns an empty list after a read error. Reproduce this with
fake providers before changing the ownership check; it is a candidate, not yet a
confirmed live data-loss event. No user data or provider quota is involved.

### H1.3 provider ownership read failures

Three Qt/fake-provider regressions reproduced unsafe cleanup: query failure and
failure after a partial catalog both sent `thread/delete` for a retained shared
thread; a new local deletion also succeeded when ownership could not be read.
`readSessions()` now distinguishes a complete empty catalog from read failure.
Both new deletion and queued cleanup fail closed on the latter; pending work and
history survive, and repair/restart permits safe progress. Focused provider suite
passes 13/13 including positive retry after repair. Final head `0dba2be9` passed
full Pro 96/96 (298.15 s) and independent exact-head review; #242 merged at
`b74fa25eba040012db76a232c78f75f435d5b67c`. Generated build removed; source
cleanup remains separately tracked.
See [ADR 0056](../../docs/decisions/0056-fail-closed-provider-ownership-reads.md).
Malformed ownership JSON and catalog/per-record bounds remain separate audits.

### H1.3 unsupported provider ownership metadata

Following the database-read guard, fake-provider probes reproduced unsafe cleanup
when ownership JSON had unsupported syntax/structure. The expanded baseline fails
23 new scenarios; an empty-provider local case was already refused. A checked
ownership reader now preserves malformed/unknown metadata and blocks queued/new
local deletion before destructive effects. Both source and retained records are
covered; repair/restart recovers safe shared-target processing. Focused provider
suite passes 37/37. Affected Release/headless matrix passes 7/7 (31.07 s); review/merge remain pending. Parent #242 full Pro
96/96 and independent review have completed. See
[ADR 0057](../../docs/decisions/0057-preserve-unsupported-provider-ownership.md).
Catalog/per-record bounds, unsupported SQL column types and handoff normalization
remain explicitly open.

### H1.3 ownership guards accepted; append failure recovery

- [x] #242 merged `b74fa25eba040012db76a232c78f75f435d5b67c`; full Pro 96/96
  (298.15 s), focused 13/13 and independent review passed. Earlier pending gates resolved.
- [x] #243 merged `857c5590c4c7c6d843123245cbebdc4cfaeea31e`; exact head
  `def33b43` affected 7/7 (27.34 s), provider deletion 37/37 and independent
  5/5 (40.39 s) passed. Earlier pending gates resolved. Generated builds removed;
  linked source cleanup remains in the final cleanup ledger.
- [x] Reproduce ignored mandatory writes in ordinary/chunked append: original
  production fails 30 scenarios; fixed focused fixture 33/33, including setup/cleanup.
- [x] Append affected Release/headless matrix 13/13 (84.57 s), protocol 74/74,
  retention 124/124; live-provider and opt-in 9 GiB cases skipped as intended.
- [x] #244 final head `11df3b89`: exact-head 13/13 (65.59 s), independent
  protocol 74/74, retention 124/124, transcript 28/28, lifecycle 28 pass/1 skip,
  activity 9/9. Merged `2438e3fe97a16ea313145ed86ce851bbef6dfa71`.
  Source/build cleanup is tracked separately (ADR 0058).
- [ ] Audit ignored multirow replay/activity mutations, integer counter bounds,
  unsupported stored types and crash/interruption recovery. Do not close H1.3 yet.

### H1.3 append counter validation

- [x] Reproduce unsupported/overflow counter handling: original production fails
  14 scenarios; focused fix 25/25 including four positive boundary controls.
- [x] #245 head `278432ee`: final exact-head Release/headless 13/13 (67.00 s),
  protocol 97/97; independent 4/4 (51.63 s). Merged
  `67ce6a84906027df64a191017cff9df2f8468e38`. Generated build removed;
  linked source cleanup remains tracked separately (ADR 0059).
- [ ] Continue migration/backfill/fork counter and process-interruption audits;
  append guards alone do not finish H1.2/H1.3.

### H1.3 fork copy integrity and destination ownership

- [x] Reproduce incomplete copy success in 11 ignored-write cases; preserve the
  already-correct commit rollback and allow empty histories.
- [x] Reproduce existing/self destination replacement in two storage cases;
  the deleted-destination tombstone control already passes.
- [x] Exercise an actual delayed fake-provider response racing another start at
  the destination ID; guarded fork preserves the new owner (ADR 0060).
- [x] Removing the destination guard makes the delayed-provider probe fail with
  an incorrect successful fork. Initial noncanonical-path harness was corrected;
  that earlier passing mutation is not acceptance evidence.
- [x] Final focused fork suite 24/24 after the canonical race fixture correction.
- [x] #246 final head `3762051d`: affected Release/headless 13/13 (68.68 s),
  fork 24/24, protocol 97/97; independent 5/5 (36.34 s). Merged
  `4d56c73c26df706e805ecbd83b266bbc80495db4`; cleanup remains separately tracked.
- [ ] Continue fork source counter/read validation, whole-history copy bounds,
  multirow replay/activity mutations and process interruption. H1.3 stays open.

### H1.3 preserve unsupported fork-source counters

- [x] Eight negative/fractional/TEXT/BLOB counter probes fail original production.
  Integer-only source projection refuses them before assigning fork metadata.
- [x] Combined fork suite passes 32/32 after integration of #246.
- [x] #247 final head `3d609825`: affected Release/headless 13/13 (84.11 s),
  fork 32/32, protocol 97/97; independent 4/4 (20.90 s), 163 checks. Merged
  `52b32485dc179d71d2dbe2300579b0da230e4bb2`. Build removed; source cleanup tracked separately.
- [ ] Continue caller-level append/flush failure handling, broader source reads,
  startup/backfill compatibility and process interruption. H1.3 remains open.

### H1.3/H2.3 ordered stream recovery after append failure

- [x] Reproduce prefix loss/order failure at timer, item and lifecycle boundaries
  on original production (three failing cases).
- [x] Preserve order through bounded per-session/host queues, defer only provider
  events and non-acceptance notifications; reject failed command acceptance.
- [x] A new oversized-event regression exposed a permanently blocked retry head;
  admission now matches journal representability before enqueuing.
- [x] Focused Release/headless 17/17 (42.88 s): recovery, no ghost prompt,
  per-session/global count and byte caps, deletion budget release, subscriber
  deletion, shutdown, real busy writer and normal coalescing.
- [x] Full Pro caught a constructor regression: interrupted-session completion
  ran before runtime registration. Register the session before the append;
  rerun the existing restart regression and full Pro before accepting the fix.
- [x] #248 final head `b3d5c7ba`: full Pro 96/96 (322.22 s), restart 3/3;
  independent protocol 97 pass, lifecycle 42 pass/1 skip and startup-failure
  recovery probe passed. Merged `b7f965679ae13e6fb3f2ec85b8c4d0f9081d73a4`.
  Generated builds removed; linked source cleanup remains tracked separately.
- [ ] Continue multirow replay/activity mutation and caller command atomicity
  audits. Volatile recovery does not finish crash/startup/backfill acceptance.

### H1.3 complete multirow replay/activity mutations

- [x] Reproduce 14 ignored-write failures: superseded assistant/command deltas,
  ordinary/chunked items, activity items/summaries and completion/error settlement.
- [x] Verify exact postconditions inside append; rollback preserves all snapshots,
  raw/replay history and counters, then repair/retry/reopen succeeds.
- [x] Add running-turn partial indexes to both activity tables. Qt query plans
  avoid settled history; the 100,000-row fixture preserves running earlier turns
  and recreates missing indexes on reopen. 25 production settlements: 8 ms.
- [x] Focused append 47/47 and activity 10/10 pass (ADR 0063).
- [x] #249 initial head `85e5d619`: affected 13/13 (123.00 s), independent
  4/4 (65.22 s; protocol 111, activity 10, transcript 28, retention 124).
- [x] #249 final head `4525aa8c`: rebased affected 13/13 (90.35 s),
  independent delta review PASS. Merged `1d5f2cbc1ae94a08da606faeed0809bd787df5de`;
  cleanup remains separately tracked.
- [ ] H1.1/H1.2 still cover very large running turns and index construction;
  H1.3 retains higher-level command atomicity and crash recovery work.

### H1.2/H1.3 release a failed startup write transaction

- [x] Deferred commit failure reproduces an activity backfill write lock retained
  while the failed journal object remains alive; turn/replay controls already pass.
- [x] Preserve the commit diagnostic and roll back activity initialization before
  returning failure. Focused temporary-journal startup set passes 5/5 (ADR 0064).
- [x] #250 initial head `874956a4`: affected 13/13 (107.99 s), independent
  3/3 (14.65 s; protocol 100, activity 9, transcript 28).
- [x] #250 final head `fef67e69`: rebased affected 13/13 (89.96 s), independent
  delta review PASS. Merged `731c40bab07674083828dabc9ba4eda2fb5a1190`;
  cleanup remains separately tracked.
- [ ] Continue mid-fetch failures, unknown formats, low-space/process interruption
  and large migration bounds. This package does not finish startup acceptance.

### H1.4 media cleanup ownership preflight

- [x] Original source fails 22/24 initial fault cases and 9/9 catalog-budget/root
  cases; two initial directory controls already preserve data. A new filename
  regression also caught an overly permissive first guard.
- [x] Cleanup now requires a complete validated catalog before any unlink: regular
  nonsymlink canonical digest files, checked reads/enumeration, bounded by 4 MiB
  per file, 16 MiB total and 4,096 entries. Unknown ownership preserves data.
- [x] Focused temporary-store set 41/41, including repaired shared references,
  existing cleanup controls and absent/empty-catalog compatibility (ADR 0065).
- [x] #251 head `8da7ffd1`: affected 14/14 (102.51 s), independent 3/3
  (78.28 s; protocol 151, lifecycle 42 pass/1 skip, fork 32) and permission probes
  8/8. Merged `b77df2c523c4c148b2aba779f8aa90e2c2ccddbc`; cleanup tracked separately.
- [ ] Continue media write/copy durability, storage-path races and total media-file
  enumeration/decode bounds. Over-budget catalogs need a separate recovery path;
  this change does not stop new writes or complete H1.4/H1.5.


### H1.4 validate content-addressed media reuse

- [x] Original source fails 18 of 20 attachment/inline-item cases: unsupported
  stored entries are reported as success, or broad file permissions survive reuse.
  The two valid-file controls pass.
- [x] Require regular nonsymlink blobs, matching bounded bytes and owner-only
  permissions before returning a reference; reject linked/non-directory roots.
  Preserve unsupported entries for repair (ADR 0066).
- [x] Focused 26/26; initial head `51d1a49c` affected 14/14 (101.57 s).
- [x] #252 final head `e16fb511`: integrated #251; affected 14/14 (92.20 s).
- [x] #252 independent exact-head 3/3 (73.75 s; protocol 171, lifecycle
  42 pass/1 skip, fork 32). Merged `886fbe201ad88e2bbd466c8e0689f3fc7a423223`;
  cleanup tracked separately.
- [ ] Continue reference writes/copies, ancestor/path races, decode and enumeration
  bounds. This package does not complete H1.4 or change retention defaults.


### H1.4 require ownership persistence before media publication

- [x] Actual-source standalone probe confirms attachment and inline publication
  return a media reference when the session reference path is a directory.
- [x] Original source fails all 27 cases: 24 ownership failures plus three
  repeated-hash growth cases. The actual partial-write probe also fails.
- [x] Checked atomic reference publication preserves unsupported metadata and
  withholds references on failure; repeated hashes are deduplicated (ADR 0067).
  Focused media set 91/91, including POSIX partial-write rollback/repair.
- [x] #253 head `98ad1946`: full Release Pro build and 96/96 suite (324.78 s).
- [ ] Independent exact-head review, merge and cleanup. Review dispatch is held
  by the conversation-chain limit; the user must manually restart reviewer #6212.
  No merge gate is waived.
- [ ] Fork-copy/journal coordination, crash durability, write-admission/blob cleanup
  after failure and broader path/decode/enumeration bounds remain separate work.


### H1.3/H1.4 coordinate fork ownership and journal commit

- [x] Original source fails nine of twelve fake-provider fork cases; existing
  commit-failure, matching destination and empty-source controls already pass.
- [x] Prepare checked atomic media ownership before SQLite commit; rollback failed
  preparation and preserve conflicting target metadata. Matching sets permit retry.
- [x] Split reference ownership from image/blob handling and share bounded parsing
  and atomic writes (ADR 0068). Initial protocol/fork 2/2 (199/44 checks); extended
  fork set 14/14 verifies reopen/orphan-reference cleanup without source blob loss.
- [x] #254 head `7397165c`: full Release Pro build and 96/96 suite (312.14 s).
- [ ] Integrate #253, independent exact-head review, merge and cleanup. The
  shared-header matrix will be reconciled against the final integrated head.
- [ ] Whole-history fork bounds, lost-reference recovery, process interruption,
  ancestor races and provider-thread compensation remain separate audit work.


### H1.4 bound and verify reference-based previews

- [x] Actual-source probe accepts five invalid metadata/blob cases. The broader
  fixture reproduces seven failures including media-root links and trailing
  newlines; six valid/refusal controls already pass.
- [x] Reuse bounded ownership validation, require canonical references and reject
  linked roots. Read bounded image bytes, verify their hash and decode that same
  snapshot. Preserve unsupported data for repair (ADR 0069).
- [x] Focused preview fixture 15/15, including repair and decoded JPEG bounds.
- [x] #255 head `20156fc7`: Release headless build and affected 15/15 targets
  PASS (107.00 s). Generated build removed after preserving evidence.
- [ ] Parent integration, independent exact-head review, merge and source cleanup.
  Review dispatch is held by the chain limit; continue independent local audits
  without bypassing the merge gate.
- [ ] Ancestor races, decode/concurrency acceptance, total blob enumeration,
  write-admission/orphan recovery and the approved media/archive policy remain.


### H3.1 bounded oversized-provider scan

- [x] Source audit identifies unbounded frame allocation below the nominal scan
  depth: each nested container still appends a frame while discarding a large
  Codex line. A scalar overflow depth can preserve shallow routing without that
  allocation.
- [x] Original source fails the fresh-child memory assertion: 201,637,888 bytes
  of peak growth; its other 24 checks pass. Expanded array/object and boundary
  coverage plus the scalar-depth fix passes 46/46; fixed growth 147,456 bytes.
- [x] Keep at most 64 routing frames and resume shallow routing after skipped
  containers (ADR 0070). No UI or live provider changes.
- [x] #256 head `413514af`: full Release Pro build and 97/97 targets PASS
  (309.16 s). Generated build removed after evidence preservation.
- [ ] Independent exact-head review, merge and source cleanup; review dispatch
  is held by the existing chain limit. Independent of pending media packages.
- [ ] Continue Claude/Cursor newline-boundary checks, aggregate read buffering,
  malformed/UTF-8 input, stderr handling and version-probe acceptance. No whole
  H3.1 completion claim follows from the scanner fix alone.


### H3.1 Claude/Cursor per-line admission

- [x] Original adapters fail eight real-QProcess coalesced-output cases; six
  valid/refusal controls pass. Newline presence bypasses the original byte guard.
- [x] Read bounded chunks, check each complete/partial raw line before parse, and
  close/drain rejected stdout before callbacks. Focused 16/16 includes request
  suppression after failure, repeat notifications and a fresh process generation.
- [x] #257 head `927d6f0d`: Release headless build and affected 15/15 targets
  PASS (87.33 s). Generated build removed after preserving evidence.
- [ ] Independent exact-head review, merge and source cleanup. No shared header
  or UI implementation change; dispatch remains held by the chain limit.
- [ ] Continue version-probe output bounds: it currently truncates only after
  process exit, leaving accumulation during the probe unbounded. Stderr, UTF-8,
  parser memory and event-loop budgets remain separate acceptance work.


### H3.1 version-probe result admission

- [x] Original source reproduces five output/exit/identity failures; a separate
  same-path replacement probe also fails. Existing valid/cache/timeout controls
  continue to pass.
- [x] Bound merged output while reading, require successful exit, retain Unknown
  retry behavior, and only join matching executable generations (ADR 0072).
  Focused fixture 13/13 passes, including early flood termination and repair.
- [x] #258 head `d3b1cee9`: Release headless build and affected 15/15 targets
  PASS (89.23 s). Generated build removed after preserving evidence.
- [ ] Independent exact-head review, merge and source cleanup. No UI or shared
  header changes; dispatch remains held by the chain limit.
- [ ] Continue stderr/encoding, process I/O/event-loop and waiter-admission bounds;
  H3.1 is not complete solely from these three provider packages.


### H3.1/H3.4 oversized routing string semantics

- [x] Original scanner fails 24 encoding/path cases; 14 valid/refusal checks pass.
  The first test-harness run did not register the new slots and is not evidence.
- [x] Preserve bounded raw tokens, validate/decode JSON strings, refuse dotted-key
  aliases and withhold all routing when captured metadata is invalid (ADR 0073).
  Expanded focused suite 91/91 includes depth/memory and next-line recovery.
- [x] Full Pro build and 97/97 tests pass at `6fd79e51` (308.05 seconds).
  Draft #259 is pushed; generated builds are removed. No UI implementation change.
- [x] #256 and #259 integrated and reviewed by #7705; merged at `7ceebc34`
  and `b93394bb`. Task worktrees and builds were removed after safe checks.
- [ ] Duplicate keys/outer structure, RPC request-id typing, timeout/recovery and
  broader stderr/normal-frame encoding remain open; this is selective extraction.


### H3.4 preserve response call identity

- [x] Fake-provider handshake reproduces 15 wrong-id failures across Codex, Claude,
  Cursor and Codex oversized responses; 23 controls already pass.
- [x] Use a shared typed positive-integer lookup guard. Focused 39/39 passes;
  invalid ids preserve pending initialize, followed by exactly one valid completion
  (ADR 0074). Numeric boundary controls also pass.
- [x] Full Pro build and 97/97 tests pass at `d835d334` (316.56 seconds).
  Draft #260 is pushed; generated build is removed. It branches from merged
  integration, independently of the waiting metadata/string packages.
- [ ] Independent exact-head review by #7705, merge and source cleanup; the
  user reassignment supersedes the historical dispatch hold.
- [ ] Continue result/error-envelope validation, incoming ids, duplicate fields,
  counter exhaustion and pending-call admission/timeout/cancellation checks.

### H3.4 validate matched response envelopes

- [x] Reproduce 72 malformed-envelope failures on the original three adapters;
  17 control/setup checks pass. Missing/scalar results can falsely complete
  initialize; ambiguous/error-shape failures also lack a protocol diagnostic.
- [x] Share a bounded envelope guard (ADR 0075): one object/null result or object
  error, static failure, exactly-once callback. Combined focused 126/126 PASS,
  including outstanding-call isolation, duplicate replies and valid recovery.
- [ ] Full Pro shared-header gate, integrate parent #260, independent exact-head
  review, merge and cleanup. No UI implementation changes.
- [ ] Continue pending-call/counter/lifecycle and method-schema audit; broader H3
  remains open. Current closure-first priority is #71; do not start a new domain.


### 2026-11-08 — integrate the existing #260/#261 response package

- #256/#259/#257/#258/#275 are merged; integration is `4cd20d4f`.
  Catalog review PASS at `2bbd4661`: independent 14/14 and daemon 52/52.
  Its worktree/build were safely removed after the PR was pushed.
- #261 already contains #260; combine their integration/review rather than
  rebuilding the same shared headers twice. No new production fix is added here.
  #7705 must review both original changes and the combined final head.
- [ ] Run full Release Pro build and offscreen suite on the integrated response
  head; verify the combined 126-case fixture and preserve failure evidence.
- [ ] Review and merge the combined package; verify #260 is included before
  closing or marking its work complete. Keep #71 open for malformed frame
  settlement, capability coverage and remaining concrete bounds findings.

## 2026-11-08 — #71 closure-first integration and catalog output

- [x] #256 merged at `7ceebc34`; #259 merged at `b93394bb` after the
  doc-only count clarification passed review at `16d8ee70`.
- [x] #257 integration head `6981197f` passed independent delta review and merged
  at `d76d0a89`. Author Release headless matrix: 48/50 first pass; two timing
  failures passed unchanged in a serial 2/2 rerun. This is not a clean first-pass
  50/50 result. Production/regression code matched the previously reviewed head.
- [x] #258 integration head `216b2043`: Release headless build and serial
  **51/51 PASS**, 372.28 s. Production and regression match its previously
  reviewed head. #7705 passed the exact-head delta; merged at `9825970f`.
- [x] Reproduce Cursor catalog accumulation and unsuccessful-exit acceptance;
  implement the owning-path fix in ADR 0088. Final self-hosted CLI fixture:
  original production **7 pass / 7 fail**; fixed **14/14 PASS**, including
  interrupting stdout and stderr 64-MiB floods before their completion marker.
  Evidence: `evidence/h3-cursor-catalog-bounds-qt.json`.
- [x] Catalog code commit `3f7892b1`: Release headless build and the affected
  **17/17 targets PASS**, 261.33 s, serial/offscreen. The following evidence-only
  commit preserves the tested production/test hashes; no full Desktop claim.
- [x] #7705 passed catalog head `2bbd4661` with independent Debug headless
  14/14 focused and 52/52 daemon targets. #275 merged at `4cd20d4f`; the
  task worktree/build had already been safely removed after pushing.
- [x] Integrate #275 into the combined #260/#261 branch before the full Pro
  test run. Production merges cleanly; preserve both docs additions.
- [ ] Next concrete #71 package: malformed normal-frame settlement/recovery
  across all three adapters. This follows the catalog merge; no new domain audit.
- [ ] Finish #71 acceptance: normal malformed/binary/invalid-UTF8 frame behavior,
  capability-to-adapter coverage, and disposition of remaining probe/waiter limits.
  Existing #260/#261 own response id/envelope work overlapping #74; do not duplicate
  them. Do not close #71 solely because the first four PRs passed.

Owner #2739 continues this issue; no new domain audit or UI implementation starts.
The relay rollout gate remains external and does not block this provider work.


## 2026-11-08 — #71 malformed normal frames

- [x] Original production fails 42 final fake-provider cases, with 5 control/setup
  checks passing. All three adapters can ignore a corrupt frame and dispatch its
  valid-looking suffix. Qt also accepts raw control bytes inside JSON strings.
- [x] Implement generation rejection, buffered-suffix quarantine and static
  lifecycle failure (ADR 0089), preserving valid whitespace/escapes and fresh
  process recovery. This is the sole new package following catalog #275.
- [ ] Full Release Pro build and full offscreen suite at the final code head;
  preserve the exact head, focused totals, full-run failures and any justified rerun.
- [ ] Independent #7705 review, merge and safe worktree/build cleanup.
- [x] #260/#261 combined head `d4737f95` passed #7705 review (126/126 response,
  53/53 daemon independently) and merged via #261 at `a6967590`. Author Release Pro: 100/101 first pass (421.97 s), response 126/126;
  unchanged serial Cursor rerun 24/24 (12.23 s) after a 3 s timeout needed 3.1 s.
  Do not call that a clean first-pass 101/101. #260 is included by exact ancestry.
- [ ] Finish capability-to-adapter acceptance and disposition of concrete #71
  findings before closing the issue. Do not begin another domain audit. No UI,
  provider quota, live journal, install or deploy work is authorized here.

- PR #276 integrates that response tree before final testing. Remaining capability
  acceptance found a concrete P2: omitted ACP v1 loadSession/image flags stay
  enabled. Fix and regress this in the same #71 completion package before the
  final full matrix and independent review; no second new unreviewed package.


### #276 final contract/compatibility integration

- [x] Integrate reviewed #260/#261 (`a6967590`) into the frame package. Source
  conflicts were include insertions only; preserve frame rejection and response
  admission together. GitHub also marks #260 merged through this inclusion.
- [x] Reproduce ACP v1 omitted/wrong-type capability defaults: compatibility
  20 pass/12 fail; Cursor 24 pass/3 fail, including actual forbidden session/load.
- [x] Require explicit true for loadSession/image and correct the fake provider's
  advertised feature set (ADR 0090). This stays in #276, the single new package.
- [x] Run full Release Pro/offscreen verification at code commit `ed2ec861`:
  build passed; first run **99/102 targets** in 548.67 s with parallelism 2.
  Agent, agent-shell and chat-Mermaid UI targets failed; a single unchanged serial
  rerun passed **3/3** in 140.33 s. No source, UI or timeout was changed between
  runs. These results are not a clean first-pass 102/102 or proof of root cause.
  Focused cases: malformed frames **47/47**, capabilities **32/32**, Cursor
  **27/27**, response admission **126/126**, catalog **14/14**.
  See `evidence/h3-provider-contract-qt.json` for source/log hashes and limits.
- [x] Obtain independent #7705 review of final head `c33730ff` and merge #276 at
  `00b2f003`. Reviewer daemon suite passed 54/54; the stderr regression gap below
  remains before #71 closure.
- [x] Remove the pushed response/frame worktrees and builds after verifying clean
  state, remote heads and absence of other users/processes.
  Request/waiter admission and absent-response deadlines remain H3.4; no universal
  QProcess memory, live-provider, other-platform, installation or deployment claim.

### #71 final stderr regression gate

- [x] Merge #276 at `00b2f003` after final-head review PASS and author verification.
  Independent reviewer confirmed line bounds, version behavior and capability
  mappings; the remaining closure gap is long-lived adapter stderr regression.
- [x] Add a fake-process fixture for two 32-MiB stderr floods per adapter, with
  mandatory stdout round trips between bursts and successful calls afterward.
  Test-only friendship observes the existing adapter tail without a runtime API.
- [x] Focused fixture passes 5/5. Mutations removing the drain or tail cap each
  fail all three adapter cases (2 setup/cleanup pass, 3 fail); restored code passes
  5/5 again. The failures identify unread QProcess stderr and an over-limit tail.
- [x] Full Release Pro build and 103/103 offscreen targets pass at `a8d45029`
  (368.10 s, parallelism 2, no rerun). Runtime drain implementations are unchanged.
  Evidence and source/log hashes: `evidence/h3-provider-stderr-flood-qt.json`.
- [ ] Obtain #7705 review, merge and close #71 only after all five criteria have
  evidence. Request/waiter admission and absent-response deadlines stay H3.4/#74.
- [ ] Remove the pushed worktree/build after ownership and clean-state checks.

## 2026-11-08 — Private migration copy and closure discipline

- [x] Reproduce the in-progress copy mode gap under umask 0022; umask 0077
  control passes. Protect the copy and sidecars with private staging, check final
  permissions, and preserve unsupported artifacts. See ADR 0087 and
  `evidence/h1-private-migration-copy-qt.json`.
- [x] Release Pro build and final-source matrix executed: journal-space 19 pass /
  1 optional scale skip. Full Pro first run 93/96 (636.55 s); the three timeout
  targets pass on a single isolated sequential retry, 3/3 (26.85 s), with no
  source changes. No clean-first-run, Windows ACL, mixed-GB or crash claim.
- [x] Reconcile original acceptance and current state for 21 scoped issues.
  GitHub confirms 35 merged PRs but zero issue closures on 2026-11-07 local time;
  `evidence/closure-audit-2026-11-08.json` records the snapshot.
- [ ] Independent review/integration of the pending migration-copy package.
- [ ] Safe source-worktree cleanup is verified and reported separately after
  pushing; do not infer cleanup merely from a passing test or opened PR.

The closure ledger overrides the old parallel-audit dispatch order. New audit
branches are frozen; finish the existing #71 packet and its complete acceptance
before moving to another issue. #85 retains its original rollout gate.

### H1.6 stored policy JSON admission

- [x] Reproduce eight unsupported-row failures in the actual Qt readers: both
  policy types accept valid JSON padded beyond 4 KiB in TEXT and BLOB columns.
  Eight valid boundary controls pass; normal writes remain schema-bounded.
- [x] Gate policy projection with metadata byte lengths before Qt/JSON parsing;
  preserve rejected rows instead of returning default revision zero (ADR 0085).
- [x] Focused 18/18; Release headless affected 4/4 targets (37.67 s): policy 31/31,
  size policy 5/5, retention 124/124, journal space 10 pass / 1 scale opt-in skip.
  Reopen, unchanged bytes/revision/history, repair/save and normal retention pass.
- [ ] Independent exact-head review and merge. Reviewer dispatch remains held by
  `chain_limit`; the queued request needs manual activation, not another retry.
- [ ] H1.6 remains open. ADR 0046 specifies append-row order within each age phase,
  not global timestamp order; do not claim the plan's oldest-turn wording has
  been globally verified or silently change that contract. The existing size
  fixture covers expired-before-recent, free pages first, Forever/off/no-limit,
  revision changes and guarded turns; representative integrated scale/crash
  acceptance remains H1.7. No UI, deployment or installed journal was changed.

Evidence: [h1-stored-policy-read-qt.json](evidence/h1-stored-policy-read-qt.json).
