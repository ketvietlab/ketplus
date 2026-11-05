# R00-D Resolution And Source Review

Recorded 2026-09-12. Reviewer: Integration/Codex. This is source/integrity review,
not designer approval, API freeze, or proof of pixel correctness. Historical
baseline and CI failures remain unchanged.

## Finding Resolution

| Finding | Change / evidence | Result |
| --- | --- | --- |
| Deleted connections.html accepted | Reproduced CLI exit 0 at reviewed #18 `88c490b838de41208282af53fc3ee7266525a7b8` in isolated docs/tests/tasks/design copy with Git objects. Added CLI regression requiring exit 1 and named missing source. | Fixed; negative test |
| HTML/CSS/JS absent from required set | `rendering-source.cjs` derives every blob from immutable design tree; only explicit documentation/progress classes excluded from local rendering checks. | Fail closed for missing, modified, renamed source |
| Submitted receipt can conceal drift | Exact inventory equality with Git-derived entries; local bytes checked against pinned Git blobs, not submitted hashes. | Omitted entry / rewritten hash rejected |
| Reference lacks full source binding | Approved visual evidence and approval record must bind revision, design tree and rendering-source SHA-256. | Structural provenance only; visual BLOCKED |
| Progress updates invalidate reference | atlas.progress.json and documentation classified separately. Historical doc hashes remain source receipts, not current rendering requirements. | Positive regression covers progress and README changes |
| CI baseline publication | `check-characterization.cjs` verifies clean Release checkout, source digest, four slots and nine unique samples before success upload. | Registration in separate #22; hosted result tracked separately |

## Rendering Source Inventory

[rendering-source-inventory.json](rendering-source-inventory.json) is generated
from design revision `7efc058d99b37f269fdebd5b69044fff32b84195`, not the
submitted baseline manifest. It contains Git blob and SHA-256 per entry, the
immutable whole-design tree, and SHA-256 of the ordered rendering-entry JSON.
The checker recomputes this from Git objects on each invocation. All screen
HTML, shared CSS/JS, atlas/schema/design-system descriptors and rendering assets
are required. Unknown non-documentation files in the pinned tree are included.
Only atlas.progress.json, Markdown and licenses are non-rendering classes.
No canonical file or dependency pin was changed. Missing Git objects fail.

Reference receipts use `{ revision, designTree, renderingSourceSha256 }` as
`renderingSource` in both visual evidence and its approval record. The design
revision is independent of the native checkout revision. A later progress-only
commit does not require updating this pinned receipt. A real source change
requires a separately reviewed reference, never local hash repair.

Since D18 the live design bundle may change after its review. The checkers read
the atlas, progress, screens and design-system files from the pinned revision
and report live differences as `drift` warnings instead of failing. Evidence
still binds to the pinned revision; paths that escape the repository still fail.
`check-plan.cjs` is itself hash-pinned evidence and stays unchanged; CI runs the
pinned-revision copy `check-plan-pinned.cjs`.

## Exact Owner Sources Reviewed

| Source | Reviewed / source-verified | Tested | Frozen / implemented |
| --- | --- | --- | --- |
| Backend #479 `e2a0844c5db717ff6ab0c700d123f4facc228d8c` | GitHub HEAD rechecked; prior actual schema/fixture review retained; fixture SHA `d454b2825c7bdbf89c94d7ed7fc6222ecf13454fc10dcab178265d2d869415d3` | Not rerun here; R02-B incomplete | Not frozen; vNext not implemented |
| Core #19 `c739042143af4666f0d820b17ae36ed6b0531c5d` | Merged as `0d789192596d01a387d9dc69efae1ec53de13b25`; historical receipts retained | 18 Node tests independently passed in R00-C | Generator/harness implemented; not wire/API freeze |
| Daemon #21 `79f802a1062936d469068f18bd121a0d9479c215` | Read R03-D resolution, R03-E plan, isolated CMake, metrics publisher, build provenance, negative tests and raw Release report | Owner local complete/clean/9 samples at build `7caa4094aaa78b764a531b2b96e894d6ce552fd9`; source digest `d3951e9a0956991bbc40d9aa0297b000ab8f659de6a8b24555d70ee90b1badf1`. Hosted stack evidence separate. | Test adapter/publication implemented in proposal; vNext runtime blocked |
| Mobile #8 `05e554628eec1121857339f27e2f1e050e4667b9` | Read exact implementation-plan.md via GitHub contents API | Not built/tested by Desktop Integration | Plan only; no mobile runtime approval |

Immutable sources: [Daemon R03-D](https://github.com/ketvietlab/ketplus-desktop/blob/79f802a1062936d469068f18bd121a0d9479c215/tasks/desktop-refactor/wave-1/core-transcript/r03-d-resolution.md),
[Daemon R03-E](https://github.com/ketvietlab/ketplus-desktop/blob/79f802a1062936d469068f18bd121a0d9479c215/tasks/desktop-refactor/wave-1/core-transcript/r03-e-implementation-plan.md),
[Mobile plan](https://github.com/ketvietlab/ketplus-app/blob/05e554628eec1121857339f27e2f1e050e4667b9/tasks/mobile-refactor/implementation-plan.md).

## Capability Readiness Delta

These rows refine the enforced Desktop [task matrix](task-readiness.md), not
replace it or authorize another owner's files.

| Capability / owner | Prerequisite / evidence | Reviewer still required | READY / BLOCKED |
| --- | --- | --- | --- |
| I04 command-only projection, Daemon/Core | #21 now executes production projector and workload adapter; owner tests cover command/output markers | Core for outgoing encryption/outbox boundary | v1 characterization READY; end-to-end cloud conformance BLOCKED |
| I05 journal paging, Daemon/Core | #21 production journal metrics replace generator-only evidence; nine samples include v1 giant rejection/truncation, not lossless replay | Core + Backend + consumers | Isolated characterization READY; lossless paging / performance acceptance BLOCKED |
| I01/I02 identity, revision, order, within-turn continuation, encoded budgets | R03-E T1-T4/T6 propose conformance; Backend R02-B gaps unresolved | Backend wire + Core persistence + consumer owners | Audit/generators READY; consumer/runtime implementation BLOCKED |
| I03 cloud TTL versus daemon availability | R03-E T7; retained success, relay lifetime and cancel/timeout contracts absent | Backend + Daemon | Audit READY; relay implementation BLOCKED |
| I06/I07 catalog, lifecycle/tombstone/resync | Schema-valid fixtures and frozen hierarchical scope/guards/GC needed | Backend + resource owner | Contract tests READY; consumers BLOCKED |
| I08/I09/I10 CM settings host, read-only worktree, logical metrics | Existing actual CM receipt remains reviewed proposal; no new CM acceptance received | Public CM; designer for measurements | Audit READY; API port/visual acceptance BLOCKED |
| Mobile M04-A transcript | Existing-v1 characterization, scope/race/identity and synthetic replay under Mobile transcript owner; no shared component ownership implied | Mobile transcript owner; Backend/Daemon for Stage B | Independent v1 work READY under mobile plan; vNext paging BLOCKED |
| Mobile M01-A components | Token/metric audit and generic fixtures under Native foundations owner; not M04 transcript store | Mobile components owner + designer | Audit/fixtures READY; reference-dependent visual acceptance BLOCKED |

Mobile shared RootView/LiveRemoteView, token/resource/contract synchronization
have one mobile integrator. Desktop Integration does not edit those files.
No private contracts or artifacts were copied into Public CM.

## Verification Commands

Local R00-D result: 104 validator tests passed (zero skipped), integration
checker and plan checker passed; deleted-screen CLI reproducer now exits 1.
Final [hosted and local CI evidence](r00-d-ci-evidence.md) records exact tested
revisions, successful macOS/Linux artifacts and the earlier dirty-source failure.
Inventory: 10 screens, 10 flows, 108 states, 115 nodes, 152 edges. These are
source/metadata tests, not native rendering or performance measurements.

```sh
node tasks/desktop-refactor/wave-1/integration/check-integration.cjs
node --test tests/design/integration-evidence.test.cjs
node tasks/desktop-refactor/check-plan.cjs
git diff --check
```

On the exact #18+#21 integration stack, after the owner's documented isolated
Release sequence, validate the successful metrics artifact with:

```sh
node tasks/desktop-refactor/wave-1/integration/check-characterization.cjs "$R03_METRICS"
```

No screenshot capture performed. Visual/API approval and wave-wide readiness
remain BLOCKED. Historical run 34675651929 remains unresolved; a later green
build is not a flaky diagnosis or proof of its cause.
