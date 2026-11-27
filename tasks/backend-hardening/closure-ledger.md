# Backend hardening closure ledger

Updated: 2026-11-08 after #71 closure. Scope remains non-UI; no release/install/deploy permission. The original morning inventory below is historical.

## Original closure audit and current progress

GitHub records 35 merged PRs and zero closed issues on 2026-11-07 in
Asia/Ho_Chi_Minh. Merged PRs: #191–196, #199–206, #212, #217, #220, #223, #228,
#230, #235, #237, #240–252. These are real fixes and evidence, but no complete
issue acceptance dossier was produced. The implementation stream kept starting
independent audits instead of finishing a domain. At this snapshot, #253–273
are 21 open drafts, and the migration-copy package was already being built.

The former #6212 independent-review dispatch was held by the conversation system's chain limit.
That delayed review/merge of pending code; it does not explain the unperformed
audits, integration work or missing issue updates, which remain owned by #2739.
The user reassigned independent review to #7705 on 2026-11-08. The former
reviewer hold is historical, not a reason to stop the new dispatch. The queue
must not grow with more independent investigations. Preserve work on pushed branches and remove unused linked worktrees.
Drain it by issue: finish #71's review/integration/acceptance before starting the
next domain, rather than waiting for every unrelated draft to merge first.

## Closure checklist and owners

| Issue / plan area | Established evidence | Required before closure | Owner / next action |
| --- | --- | --- | --- |
| #71 / H3.1 | CLOSED after #277 merged `b3af8ed5`; author Pro 103/103, independent daemon 55/55; all five criteria checked | [Closure evidence](https://github.com/ketvietlab/ketplus/issues/71#issuecomment-6717547995); H3.4 remains separate | None; current issue is #68 |
| #65 / H2.1–2 | IPC/token fixes prepared in #263/#264/#266 | Review/merge; finish permissions, single instance, slow clients, reassembly, reconnect and compatibility criteria | #2739 after the current #68 queue; no claim these checks are complete |
| #66–67 / H2.3–4 | Append/fork/recovery fixes through #250 merged; turn acceptance #262 and media fork #254 pending | Two-client/late-event/handoff, approval/question/task and destructive-lifecycle acceptance; integrated tests | #2739, queued after the current closure target |
| #68 / H1.1–4 | Storage/GC/transaction fixes through #252 merged; #253–255 merged via #255 `691d18c4`; combined #272–274 merged `c3490da5` after independent daemon56/56 | Remaining delete/fork bounds, schema/failure/crash and media race/ownership acceptance; exact-head review and integration | #2739; completing the migration-copy package does not close this issue |
| #69 / H2.5 | Existing agent messaging suite and earlier runtime work | Complete identity, reply, loop/flood, authentication, secret-isolation and delivery audit | #2739; not completed, not blocked solely by relay |
| #72–75 / H3.2–5 | #257/#260/#261 cover limited parsing cases | Full per-provider lifetime, background-task, credentials, worktree and cancellation criteria | #2739; pending local work remains visible |
| #85 / H4 | #116 and client #191 merged; companion relay work is tracked privately; independent client/vector and relay reviews | Existing ADR 0036 rollout gate: database migration, every replica's deployed revision/legacy flag, v2 round-trip and cutoff/owner | Relay release operator still unassigned; #2739 tracks evidence. Keep open; do not move the gate merely to count a closure |
| #128 / H1.5–7 | Detail/pressure/reclaim fixes and synthetic vacuum evidence merged | Media/archive backend contract, representative mixed multi-GB steady state/crash tests; original UI/export/warning criteria remain outside this task | #2739 owns backend gaps; UI owner owns excluded UI acceptance. Keep mixed issue open |
| #77/#78/#97/#103 / H4–5 | Selected backend behavior and Atlas readiness #267/#270 prepared | Remaining nonvisual sync/process boundaries; excluded UI criteria need their owner | #2739 owns backend; no UI implementation in this plan |
| #105 / H5.3 | Tunnel EOF fix #271 prepared | Review/merge plus path confinement, codec, authorization and teardown acceptance | #2739 |
| #107–110 / H6 | #265/#268/#269 prepared; local/hosted evidence recorded per PR | Remaining release/dependency/signature/promotion acceptance; repository enforcement; existing UI test failures require UI owner | #2739 owns backend checks; repository admin owns enforcement. No real publication/install/deploy |

## Dispatch and merge checklist

- [x] Verify original issue criteria, current states and prior review limits.
- [x] Confirm #85 client/relay merge separately from missing rollout evidence.
- [x] Finish the already-started private migration-copy build/test/evidence.
  Journal-space 19 pass/1 optional scale skip; full Pro first run 93/96, then
  the three timeout targets passed on one isolated retry without source changes.
  Its PR still requires independent review; this is not issue acceptance.
- [x] User reassigned independent review to #7705; preserve historical #6212 PASS
  evidence. This is an explicit owner change, not an automatic bypass.
- [x] Review and merge the #71 packet through #7705; finish all five criteria.
  Final #277 review passed `5880b17d`; merge `b3af8ed5`.
- [ ] Respect stacks: #253 → #254 → #255; #256 → #259; #260 → #261;
  #265 → #268. Reconcile current base and retest/review changed heads after merge.
- [ ] Merge only with the required author matrix and independent review.
- [ ] Verify and remove every unused clean/pushed linked worktree; keep the main
  checkout and any worktree owned by another process/conversation.
- [x] Complete #71's original checklist and close it with evidence.
- [x] Merge #68 media #253–255 after Pro104/104 and independent daemon56/56.
- [x] Finish journal #272–274 review/merge after combined Pro104/104 and independent
  daemon56/56 at `c30fc59e`; merged `c3490da5`. Continue to
  reconcile the original seven criteria; remaining bounds/crash/filesystem work
  stays visible and no #68 closure is inferred from package PASS.

Only #71 is declared complete by the linked closure evidence. Packaging, deployment and device
installation are not inferred from build/test results. The existing mixed UI and
operator gates remain open under their original acceptance criteria.

Current #68 criteria and next gate: [acceptance dossier](issue-68-acceptance.md).
