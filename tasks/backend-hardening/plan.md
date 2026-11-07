# Backend hardening execution plan

Updated: 2026-11-07. Owner: conversation #2739. Independent reviewer: #6212.
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
4. Send one complete exact-head request to #6212. While an item awaits review,
   continue an independent ready audit. Do not poll the peer or stop with an
   invitation to continue. If all remaining items depend on an external gate,
   record its owner and exact missing evidence instead of claiming completion.
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

| Item | State | Evidence / remaining work | Next action / owner |
| --- | --- | --- | --- |
| H0 | Complete | integration inventory; #212 merge/review and inherited limits reconciled | None |
| H1.1 | In progress | Global orphan-GC source path found outside bounded detail pass; synthetic probe recorded below | Reproduce in Qt/production harness, then bound work / #2739 |
| H1.2–H1.7 | Not started | No claim that #128 backend acceptance is complete | Follow H1.1, prioritize a confirmed safety finding / #2739 |
| H2–H3 | Not started | Open daemon/provider review issues mapped | Execute after storage, or independent audit while waiting review / #2739 |
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

**Next executable task:** add a Qt regression for this global path, including
reopen and a command-result-triggered pass, then choose a bounded/transactional
cleanup strategy preserving raw/replay references. Re-run the affected daemon
matrix and request #6212 review; do not reuse #212's PASS for this new change.
