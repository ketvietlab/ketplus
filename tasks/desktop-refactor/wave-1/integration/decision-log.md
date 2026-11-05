# Decision Log

## R00-D

- Reviewed/source-verified: immutable 56-file rendering-source inventory and
  source-bound reference receipt enforcement; [resolution](r00-d-resolution.md).
- Tested: exact integration stack Node, isolated Release and full Qt lanes on
  macOS/Linux; [immutable evidence receipt](r00-d-ci-evidence.md).
- Blocked: standalone #22 dependency integration, Backend R02-B/API freeze,
  reference-dependent visual acceptance, lossless vNext replay and release.
- No designer/user approval, API freeze or runtime implementation authorization
  was issued by Integration. Successful tests are not approval decisions.

Reference status terms:

- `proposed`: recorded for review, not binding.
- `reviewed`: Integration reviewed the proposal and scope.
- `approved`: decision accepted by the required owner/reviewer.
- `blocked`: cannot proceed without permission, owner decision, or evidence.

| ID | Decision | Status | Owner / Reviewer | Evidence |
| --- | --- | --- | --- | --- |
| D01 | Use Desktop `origin/develop` `7efc058d99b37f269fdebd5b69044fff32b84195` as wave 1 branch base. | approved | Integration | Worktree `integration-wave-1-baseline` created from `origin/develop`. |
| D02 | Treat PR #17 source revision `37a14562a11b6ec9fb7f137e9e93c95af464bc8d` as exact plan provenance even after squash merge. | approved | Integration | R00-B observed `origin/develop` at `73bddc13aab3425636fbd2f0a61860d95d13402c`; exact source revision is not an ancestor. |
| D03 | Do not cherry-pick design history; use merged PR #16 design artifact from develop. | approved | Integration | Design artifact tree `8f018a580adcb825e7aaab6201a7e6a33087c5c1`. |
| D04 | Lock inventory at 10 screens, 10 flows, 108 states, 115 nodes, 152 edges. | approved | Integration | Plan checker passed and merged atlas heuristic matches. |
| D05 | Reference approval requires actual viewer/native evidence and designer/user confirmation. | blocked | Designer/user | No capture permission/evidence available in this handoff. |
| D06 | Public CM receives only tokens, measurements, generic component states, and public API needs. | reviewed | Integration + CM owner pending | Public-safe brief prepared; CM owner still must approve implementation scope. |
| D07 | Backend C01 to C05 contract areas are the right proposal surface. | reviewed | Integration + Backend owner pending | Backend proposal reviewed; schema/fixtures not approved. |
| D08 | Core transcript/cache performance risks are accepted for planning. | reviewed | Integration + Core owner pending | Performance findings reviewed; release benchmark not run. |
| D09 | Archive/delete/restore lifecycle cannot be implemented from mock semantics alone. | reviewed | Integration + Backend/Core owners pending | No existing IPC/Control command is approved for this lifecycle. |
| D10 | Do not edit `ketjs`, `ketatlas`, or `vendor/ketplus-cm` in this wave 1 integration handoff. | approved | Integration | Repository boundaries and user instruction. |
| D11 | Do not advance CM gitlink during this handoff. | approved | Integration | Desktop remains pinned to `cm-v0.1.5` at `9fb86c8ccf64bb75e1e5cdf31314cedf46d24439`. |
| D12 | Native performance targets and cache budgets remain proposals until measured. | blocked | Integration + task owners | No release benchmark run in this handoff. |
| D13 | Evidence validation must derive inventory from `atlas.json` and reject invalid status claims. | approved | Integration | `evidence-validator.cjs` plus `tests/design/integration-evidence.test.cjs`. |
| D14 | Enforce task prerequisites and generate Markdown from one policy/manifest; independent audit/characterization may begin without visual approval. | reviewed | Integration; coordinator reviews policy | [Task matrix](task-readiness.md), [schema](evidence-format.md). No implementation authorized. |
| D15 | Backend/CM/Core actual handoff revisions have been reviewed. | reviewed | Integration; each owner retains freeze authority | [Interface register](cross-team-interface-register.md), [source receipts](owner-review-sources.json). I01-I10 remain unresolved. |
| D16 | PNG integrity and approval provenance are necessary evidence, not proof of human consent or pixel correctness. | reviewed | Integration; designer/user retains reference authority | Test-only fixtures rejected by normal checker; no capture occurred. |
| D17 | Separate Node workflow registration from baseline changes; add Core tests only with explicit #19 dependency. | proposed | Integration / CI reviewer | [CI registration plan](ci-registration-plan.md). |
| D18 | Validate wave-1 evidence and plan coverage against the pinned design revision; live design drift is a warning, not a CI failure. | approved | Repository owner | The atlas kept evolving after review (for example the theme sync), which held `design-evidence` red on every change. A new reviewed reference still requires a separate re-pin. |
