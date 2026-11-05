# Cross-Team Interface Register

Latest [R00-D delta and per-capability readiness](r00-d-resolution.md) supersedes
the R00-C I04/I05 evidence gap below: #21 now contains a production adapter and
local metrics. Reviewed/source-verified/tested/frozen/implemented are tracked
separately there, including Mobile M04-A versus M01-A ownership. The following
R00-C review is historical, not a claim that current #21 is generator-only.

R00-C review, 2026-09-12. All rows below are **reviewed proposals**, none frozen.
Integration read actual owner artifacts, not just PR #17 or coordinator feedback.
The byte hashes and immutable source URLs of every reviewed artifact are in
[owner-review-sources.json](owner-review-sources.json). No private artifact was
sent to the public CM repository. Owner test reports are distinguished from
tests independently run here.

| Owner | Actual reviewed HEAD | Handoff / fixture evidence | Acceptance reviewer |
| --- | --- | --- | --- |
| Backend, ketviet PR #479 | `e2a0844c5db717ff6ab0c700d123f4facc228d8c` | `contracts/ketplus-refactor-vnext/` schema, OpenAPI, fixtures, manifest; `vnext-fixtures.test.ts`; `audit-and-contract-handoff.md` | Backend owner, then daemon/mobile consumers |
| Public CM, ketplus PR #3 | `aa50715f60b8a056c395dd106d9959d98ce6abd2` | Proposed interfaces, token/metric mapping, `RefactorWave1ContractTest.cpp` | CM owner; designer/user for visual reference |
| Core/Transcript, Desktop PR #19 | `c739042143af4666f0d820b17ae36ed6b0531c5d` | Handoff, generator, harness, both Node test files | Core/daemon owner; Integration for harness registration |

HEADs were queried from GitHub before review and again before handoff. CM advanced
from `33ff0dda` to `aa50715f`; Core advanced from `c1dc1cbb` to `c7390421` and #19
squash-merged as `0d789192596d01a387d9dc69efae1ec53de13b25`. Integration reviewed
the changed artifacts and reran Core tests. Previous source receipts remain in
`previousReviews`. A merge or coordinator review is not API/visual approval.

## Interface Decisions

| ID / capability | Owner; producer -> consumer | Proposed revision / fixture | Unresolved decisions and acceptance evidence still required | Status; task effect |
| --- | --- | --- | --- | --- |
| I01 Canonical identity, revision, order, coverage | Backend + daemon; canonical page -> Desktop R05 / mobile R13 | Backend HEAD above; fixture bundle hash below | Item IDs separate from event cursor; specify ordering comparator, equal-revision conflicts, within-turn coverage and large-item completeness. Current `payload` is open object; `CoverageRange` has turn ranges only. Need fully schema-valid positive DTOs, schema-rejected negatives and owner freeze. | reviewed; R02 contract tests READY; R03/R05/R06/R13 consumer implementation BLOCKED |
| I02 Within-turn / large-item continuation; encoded budgets | Backend wire owner, daemon authority; page/relay -> Desktop/mobile | Backend `TranscriptPageVNext`, `pagination-oversized-item-reference`, `partial-turn-continuation-required` | `largeItemRef` appears in fixture payload but has no specified fetch/chunk DTO/path. Fixture item omits required `turnId`, `turnSequence`, `order`. Define scoped continuation binding, progress guarantees, authenticated chunk integrity, envelope encoding budget, expiry/retry and lossless completion. Numbers 524288/1048576 remain proposals. | reviewed; independent schema/byte tests READY; consumer paging BLOCKED |
| I03 Cloud TTL vs daemon history availability | Backend relay + Core storage; cloud hot replica / daemon SQLite -> mobile | Backend TTL 86400 and relay DTO proposal; Core handoff | Cloud expiry must not claim daemon history expired. `OlderHistoryRelayStatus` has `responded` but no `retained` availability; complete successful response, poll/delivery/cancel/timeout fencing, offline vs unavailable cases and typed encrypted envelopes need owner decisions. | reviewed; audit READY; R03/R13 relay implementation BLOCKED |
| I04 Command-only projection | Core daemon + Backend; raw local events -> encrypted cloud/mobile projection | Core marker events `ebe04c18...`, expected projection `45b8acde...`; full hashes below | Generator-local `projectForCloud` is not production `CloudEventProjection`. Replay raw markers through a production adapter; retain full command text/status while stripping output before encryption. Backend substring absence assertions do not execute that projection. | reviewed; Core headless characterization READY; production conformance/implementation BLOCKED |
| I05 Workload-to-production adapter | Core; event/operation schedules -> production journal/model/renderer harness | Core generator source receipt and deterministic manifest below | Legacy restore harness uses its built-in 300-turn journal, not these workloads. Map scenario operations and source bytes into production adapters; record raw results, exact binary/revision/build type and negative rejection. No performance budget or frame/RSS result accepted here. | reviewed; isolated characterization READY; model/renderer implementation and performance acceptance BLOCKED |
| I06 Catalog hierarchy and draft binding | Backend; scoped catalog -> Desktop R04/R08 / mobile R13 | Backend `ResourceScope`, `CatalogEntry`, draft-bind fixture | `ResourceScope` requires repository/worktree for all catalog kinds, including workspace/machine. Define hierarchy-specific scope, parents, safe integer/revision rules, single bind, pagination/resync. Fixture catalog entry lacks required scope. Need real DTO conformance, not scenario summaries. | reviewed; contract tests READY; catalog consumers BLOCKED |
| I07 Lifecycle/tombstone/resync | Backend + daemon; resource operation/ack -> Desktop/mobile stores | Backend lifecycle and reconnect fixtures; tombstone proposal 604800 seconds | Define destructive capability enforcement (optional false flag is not an authorization rule), active-turn guards, parent/child cascade and partial failures, generation/revision fences, replayable acknowledgments, tombstone GC after long offline cache, restore only from Archived. Need schema and executable state-transition tests plus owner freeze. | reviewed; lifecycle audit READY; R08/R13 implementation BLOCKED |
| I08 Generic Settings host | Public CM; SettingsHost / SettingsPage -> consumer-owned pages | CM interfaces and contract-test receipts | New proposal specifies parent ownership, opaque IDs, registration results/order, dialog-wide buffered Reset, apply-all compatibility and no cross-store transaction or extension-preview rollback guarantee. New tests cover Cancel persistence, Reset/Save, ThemeManager rollback and clean-page apply. Still need owner acceptance of dirty-only migration (deferred), embeddable content/adapter boundary and tests for proposed registration lifetime/selection. Private pages stay private. | reviewed; R01 audit READY; public API implementation/consumer port BLOCKED |
| I09 Read-only worktree semantics | Public CM; Git discovery/preflight -> workspace selection | CM WorktreeOperation proposal, parser tests | New proposal uses common-dir plus registered git-dir, allows sibling paths, and makes dirty/locked/current/main observations warnings for select. It now specifies non-atomic revalidation, cancellation/timeout and exactly-once completion while alive. Factory ownership of the workspace-selection adapter/thread marshalling still needs approval and disposable runtime characterization. Parser tests remain observations, not preflight proof. | reviewed; disposable characterization READY under owner scope; implementation BLOCKED |
| I10 Native metric units / token mapping | Public CM; semantic token snapshot -> Widgets/QML consumers | CM mapping at KDS `6f923d1e13958ce740e50bccab45efc56ccd2f84` | New proposal uses qreal logical pixels, 16px root-rem conversion, no double-DPR scaling, explicit density and qRound/qCeil boundary rounding; fixes xl=18px vs 2xl=22px. Source alias/light-dark/alpha rules now exist. Need executable source-pinned mapping fixtures, scaling/rounding tests and owner acceptance; drift and visual measurements remain unapproved. | reviewed; token audit READY; R01/R11 port and visual acceptance BLOCKED |

## Independently Checked Evidence

- Backend fixture file SHA-256:
  `d454b2825c7bdbf89c94d7ed7fc6222ecf13454fc10dcab178265d2d869415d3`,
  matching the owner manifest. Its declared canonical payload hash is
  `0447b958f04ce037776a046a616054d5899e08b6a676d471967d612cea9bae67`.
  Source review found `given/request/expected` constrained only to object by
  fixture schema; the focused test checks hashes and selected assertions, not
  schema conformance. Backend tests were not run by Integration.
- CM source review only. The owner reports Qt tests in PR #3; Integration did
  not run public CM Qt tests or approve their API/visual results.
- Core tests independently run at `c739042143af4666f0d820b17ae36ed6b0531c5d`
  with Node `24.14.1`: **18 passed**, covering both generator and harness suites.
  This includes actual materialization and byte/hash checking of all scenarios,
  the >=8 MiB final item, per-session/generation cursor ordering, and failure propagation for stale/missing/malformed generator artifacts. No production benchmark ran.
- Core deterministic manifest (default seed `ketplus-transcript-refactor-wave1`)
  generated SHA-256:
  `ebee26481857ee4145b9147fd391341e3502c4611e9ab5524627250ad267d367` (format v3).
  Exact giant-turn JSONL: `b0604d69ab18ecb78540dda081a52fefa295faa800f0c81ed0dc1c2fb56f6bf9`,
  8,654,101 bytes. Raw command marker JSONL:
  `ebe04c189292e8aeb2024c58cfcb0b7bc63246c41dfe0d237e07c644722f7ee7`;
  generator-local expected projection:
  `45b8acdee5aeb782f8562398ace5f649fbc00b7dc3655a749d0fa6426e2dd96c`.
  These identify workloads, not a frozen wire contract or a production result.

Reproduce in a separate checkout of the exact Core revision:

```sh
node --test tests/fixtures/transcript-refactor/generate-transcript-workloads.test.mjs tests/fixtures/transcript-refactor/baseline-harness.test.mjs
node tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs --manifest /tmp/core-workloads.json
```

## Handoff To Owners

Backend: return schema-valid DTO fixture pairs and executable invariants for
I01/I02/I03/I06/I07, along with approved scope and revision. CM: return resolved
I08/I09/I10 semantics and generic tests; release/tag/pin is a later gate. Core:
return the production characterization adapter and results for I04/I05, keeping
wire fixtures Backend-owned. Designer/user: capture permission or a real approved
reference package. Coordinator: separate implementation scope authorization.

The enforced rows are [task-readiness.md](task-readiness.md). READY means work
may start on that audit/characterization slice; it does not mean the work is
complete, a contract is frozen, or runtime implementation is authorized.
