# Security audit disposition and publication plan

Plan date: 2026-11-16. Task: T-3. Status: proposed work and pending decisions;
publication readiness is **not established** by this document.

The input is [T-2's accepted public-surface audit](public-surfaces.md), report
head `05ee9a8ad4adb57870b610f659052c6e03a024d1`, accepted in
[review round 2](https://github.com/ketvietlab/ketplus/pull/705#issuecomment-6909286234).
[PR #705](https://github.com/ketvietlab/ketplus/pull/705) merged into `integration`
at `769e45b75157a82e45d9c5abf2b5cfcb4dc83ef3`. Its audited implementation baseline
is `7c12c773372b1d63eae867b7a6f8dcfefb6cdbbb`; accepting or merging the report did
not fix product behavior or clear publication gates. The plan starts from the
merged integration snapshot and does not attest intervening implementation changes.

This task changes only this plan. It does not implement the proposals, create
remediation tasks, choose product/history policies, rewrite Git history, repeat
T-1's investigation, build packages, deploy, or authorize publication. Module
owners below are proposed ownership boundaries from
[repository boundaries](../repository-boundaries.md), not new assignments to people.
The leader assigns follow-up tasks after the relevant decisions are recorded.

## Disposition register

All six findings remain **open for disposition** in this plan. “Accepted report”
means that its evidence and limits passed documentation review, not that a risk
was accepted for release. Future closure needs an implementation or an approved,
verified scope restriction, evidence at the release candidate, and reviewer signoff.

| Finding | Priority carried from T-2 | Owning module or role | Proposed work package | Decision dependency |
| --- | --- | --- | --- | --- |
| PS-01: implicit Atlas renderer execution | High | `modules/atlas`; `apps/desktop` entry points | A-01: separate viewing from renderer authorization | D-01 |
| PS-02: historical publication boundary | High | Repository maintainers; public-tree guard and publication workflow owners | A-02: exact-ref inventory, private triage and publication-safe history | D-02 |
| PS-03: Git helpers on automatic inspection | Medium | `core` Git services; shared workspace trust, with Lite/Pro composition | A-03: prevent unapproved helper execution in background queries | D-03 |
| PS-04: browser download integrity | Medium | `apps/browser` installer/engine; `modules/browser` host settings | A-04: enforce the selected executable provenance policy | D-04 |
| PS-05: incomplete binary distribution evidence | Medium | Packaging/release maintainers; desktop/core notice surfaces; licensing reviewer | A-05: per-artifact dependency, notice and source-offer gates | Existing distribution policy; release scope still needs explicit approval |
| PS-06: disabled Chromium sandbox | Medium | `apps/browser` launch policy; `modules/browser` host integration | A-06: sandboxed launches and supported-runtime acceptance | D-05 |

The finding IDs, severities, conditions and original source locations are retained
in [the accepted report](public-surfaces.md#findings). This register proposes work;
it does not expand conditional findings into claims of an unauthenticated exploit.

## Decisions reserved for the user and repository owners

No option is selected here. The leader should present these concrete alternatives
to the user, record the chosen scope and compatibility expectations, then create
the implementation tasks. Repository/history decisions additionally require the
repository owners. Approval of this planning document does not select an option.

| Decision | Alternatives to present | Consequences that must be explicit |
| --- | --- | --- |
| D-01: Atlas renderer trust | Request authorization for each renderer launch; or allow reusable trust bound to canonical bundle identity and exact contract contents | Repeated prompts versus retained executable-content authority. Both must show resolved command/cwd, allow refusal without executing, invalidate trust on changed content, and define revocation. Static viewing must remain distinguishable from starting a renderer. |
| D-02: publication history | Prepare a clean publication snapshot/history with a restricted archival copy; or coordinate sanitization of the selected existing history and refs | A new lineage loses public historical continuity; rewriting selected history changes commit IDs and affects clones, forks, tags and references. Neither alternative proves content is safe without scanning and owner review. Preserve applicable attribution/licenses; do not execute either strategy in this task. |
| D-03: automatic Git behavior | Keep executable helpers disabled for automatic inspection in every workspace; or introduce explicit workspace trust before enabling those helpers | Possible performance/feature changes versus a new grant/revocation lifecycle. Define the default for extracted repositories, canonical worktree identity, shared Lite/Pro behavior and intentional mutation hooks. Ordinary fresh clones do not inherit another repository's local Git config. |
| D-04: browser provisioning | Retain managed downloads with independently trusted artifact identities and cache validation; or require supported installed browsers and disable fallback downloads | Managed provisioning needs maintained version/platform pins and an update process; installed-only mode can leave browser tools unavailable until the user installs a supported browser. Preserve clear readiness errors and specify who owns browser updates. |
| D-05: sandbox compatibility | Require sandboxed startup for every supported browser/platform combination; or narrow the supported combinations/capability availability to those with verified sandboxing | Stricter startup may expose host configuration requirements; a narrower support matrix removes capability on excluded hosts. Define failure messaging and support scope. Silent fallback to `--no-sandbox` does not satisfy A-06 under either alternative. |

Choosing a delay or narrower release scope does not close an unresolved finding
for excluded capabilities or waive the applicable
[licensing gates](../licensing.md#release-gates). Any proposed residual-risk
acceptance needs its own explicit rationale, owner and approval; none is inferred
from these alternatives or from passing offline tests.

## Required acceptance evidence for follow-up work

These are future checks, **not executed or satisfied by T-3**. Each work package
should attach its fix PR, exact tested commit, platform/runtime versions, result
and limits. Keep sensitive triage details private; public evidence should contain
only reviewed, redacted metadata, following [security reporting](../../SECURITY.md).

### A-01: authorize Atlas renderer execution (PS-01)

Implement the selected D-01 policy in the Atlas owner, covering both the
conversation-panel and transcript-result entry points. Check authorization again
immediately before process launch; a sidecar's presence must not itself authorize
its command. Handle content changes, canonical-path changes and revocation.

Acceptance evidence: a fake CLI records launch arguments in temporary fixtures;
untrusted/refused/changed contracts cannot receive renderer opt-in, an authorized
unchanged contract follows the selected policy, and static viewing remains usable.
Cover both desktop entry points, restart/reopen behavior and cancellation. Record
the actual UI authorization behavior with the repository's required native/theme
checks when UI implementation lands; a fake CLI alone cannot establish that UX.

### A-02: establish the publication boundary (PS-02)

Maintainers inventory the exact branches, tags and other refs intended for public
distribution, including any source archives or auxiliary Git content that will
ship. Privately classify historical matches and distinguish false positives from
material that must not be published. Carry out D-02 only after explicit approval,
in a separately owned publication task, with a reviewed recovery/migration plan.

Acceptance evidence: a reproducible scan of the exact intended ref set, reachable
content and commit/tag messages, plus deliberate treatment of binary/archive,
LFS/submodule and credential-format gaps noted by T-2. Include redacted rule/path
or object metadata, private disposition records, reviewed license/attribution
preservation and the final exported source identity. A clean checkout guard or a
Gitleaks false-positive disposition alone cannot satisfy this history gate. Never
paste private matches or secrets into the plan, public PRs or issues.

### A-03: constrain automatic Git queries (PS-03)

Implement D-03 in shared `core` ownership; review automatic status/diff/index paths
for helper behavior rather than treating argument arrays as sufficient isolation.
Define intentional mutation hooks separately from background inspection, and keep
Lite independent of Pro modules.

Acceptance evidence: an isolated temporary repository with synthetic helper
markers cannot execute those helpers before the selected authorization boundary;
authorized behavior and revocation match the chosen policy. Include relevant
fsmonitor/textconv/external-diff/filter paths, normal Git functionality, and both
Lite/Pro integration where shared behavior is exposed. Run focused regressions and
the required Lite suite for any `core` change, as specified by
[testing](../testing.md). An ordinary-clone test alone misses T-2's local-config
precondition.

### A-04: verify browser executable provenance (PS-04)

Implement D-04 across discovery, download, extraction, cache reuse and startup.
Independent trusted identity must cover the selected managed artifact before it
is executed; a completion marker or HTTPS transport alone is not that identity.
For installed-only scope, prove that fallback download paths are disabled and
that readiness errors accurately describe the missing dependency.

Acceptance evidence: offline substituted-archive/cache fixtures are refused,
approved identities are accepted, interrupted installs recover safely, and
host/environment overrides cannot silently weaken the selected policy. Record
platform/version coverage and the provenance/update owner. Actual supported
runtime acceptance remains separate from fixture results; do not download or
tamper with an external service to establish the unit-test cases.

### A-05: complete per-artifact distribution evidence (PS-05)

Generate the inventory from the actual staged output for every edition,
OS/architecture and release artifact in scope. Reconcile native libraries, Qt
plugins/transitives, Node/npm runtimes, fonts, icons, optional executables and
their notices/source obligations. Add the specified source/notice access to the
running application and correct stale licensing-status documentation in its own
change. The existing Linux SPDX generator's three-package inventory is evidence
of a starting point, not completion of this package-level gate.

Acceptance evidence: each artifact digest maps to a complete SBOM, installed
notices, source/build identity and applicable corresponding-source offer; missing
components/notices fail the release gate. Record controlled Qt linkage/toolchain
evidence and the required licensing review. Use
[licensing](../licensing.md#release-gates) and
[release process](../release-process.md#pro-release) as the acceptance authorities;
this plan makes no independent legal-compliance determination.

### A-06: restore Chromium sandbox containment (PS-06)

Implement sandbox-enabling production launch policy and reject overrides that
silently disable it, for both supported system-browser and managed-browser paths.
Resolve D-05's support/startup behavior explicitly; record a clear failure when the
required containment cannot be established. Preserve the origin proxy as its own
network control rather than treating it as a substitute for process containment.

Acceptance evidence: offline launch-option/argument-builder tests against the
pinned dependency require sandboxing and reject disabling arguments. Separately
inspect effective process-sandbox behavior on every supported OS/browser/engine
combination, including unavailable/failed sandbox startup. Capture platform and
engine versions and results without provider calls or hostile exploits. Merely
removing an argument, checking an option, or passing the proxy tests is insufficient
to attest effective runtime containment.

## Audit and verification gaps

At the 2026-11-16 06:41 UTC task-status snapshot, T-1 has been handed to Dev 3 and
is **in review**, not blocked by the earlier provider interruption.
[PR #706](https://github.com/ketvietlab/ketplus/pull/706), head
`ec04a87ba1ff2cb44ba8a718beafdf4a9c9c64b4`, is awaiting reviewer disposition.
Its scope is
`apps/daemon`, `modules/remote` and `modules/ai`: authentication/IPC, credential
lifecycle, transport, tool authorization and conversation isolation. T-3 does not
repeat that investigation or treat preliminary task notes as accepted findings.
No final accepted T-1 report is used as evidence here. Before a readiness decision,
the leader must reconcile the latest T-1 verdict/report and resulting work with
this register; completion of T-2 alone leaves this audit stream unresolved.

T-2's [coverage and executed-check limits](public-surfaces.md#executed-checks-and-handoff)
remain applicable to its pinned source. The 99 deterministic tests, harmless
fixtures and redacted scans support the stated offline conclusions; they do not
validate a release artifact or certify later commits. In particular:

| Outstanding evidence | Proposed owner | Required before the applicable readiness conclusion |
| --- | --- | --- |
| T-1 accepted report and disposition | Dev 3, reviewer, then leader | Final reviewed scope/results and separate follow-ups; no assumed daemon/provider clearance |
| Chromium-dependent security suites and actual sandbox behavior | Browser owner | Runtime-backed tests under the authorized browser workflow; A-04/A-06 evidence |
| Computer permission, capture, input and stop behavior | Computer owner | Native TCC/AX/HID/grant/remote-turn acceptance on supported macOS versions; protocol/no-op tests alone do not cover it |
| Atlas WebView/profile/navigation and external serving boundaries | Atlas owner | Explicit isolation/ownership evidence; managed selected-port readiness follow-up; do not extend the browser proxy's guarantee to WebView |
| Complete native matrix and implementation delta after the audited baseline | Module owners and release maintainer | Review all intervening changes; run the required supported-platform/edition checks at the exact candidate |
| Dependency advisories and unscanned source-content classes | Dependency and repository maintainers | Current version-specific advisory disposition and deliberate binary/archive/history scan coverage; T-2 did not query online advisories |
| Signed/installed artifacts and end-to-end update behavior | Release maintainer | Actual staged-artifact inventory, linkage, production build identity, signatures/notarization/Gatekeeper where applicable, archive verification and update acceptance |
| Private or external implementations | Their owners through separate scoped work | Evidence for any cross-boundary assurance being claimed; no private source or infrastructure details enter this public repository |

The Atlas readiness, Computer token-lifetime and other limitations in T-2's
[remaining trust boundaries](public-surfaces.md#controls-and-remaining-trust-boundaries)
are coverage follow-ups, not additional confirmed findings or implicitly approved
behavior changes. The leader should scope them separately if the intended release
claims depend on those properties.

## Readiness gates and handoff

Source-publication and binary-release evidence must be evaluated separately.
The status below is the evidence status of this plan, not a claim about unseen
work completed elsewhere.

| Gate | Evidence currently used | Remaining proof and owner |
| --- | --- | --- |
| Audit report acceptance | T-2 review round 2 and merge above: verified | Leader obtains/reconciles T-1's final accepted report and reviews the release-candidate delta |
| Product remediation/disposition | Six accepted T-2 findings: recorded; closure unverified | User/owners decide D-01–D-05; leader assigns work; module owners supply A-01/A-03/A-04/A-06 evidence and reviewer accepts it |
| Source publication | T-2 historical boundary finding: open | Repository owners approve and verify A-02; complete applicable source licensing/secret/transcript review for the exact publication contents |
| Binary distribution | T-2 engineering fixtures: verified only for their stated scope; artifact acceptance unverified | Release/licensing owners complete A-05 and all supported artifact/runtime checks in the release process |
| Publish/deploy authorization | None requested or granted by this plan | User/release owner explicitly authorizes the concrete candidate through the existing release flow after applicable gates hold |

The leader can create the six work packages independently where their decision
dependencies permit, but should reconcile browser provenance and sandboxing on the
same candidate. Repository-owner history decisions must precede any history
mutation. No release conclusion may be inferred from merging these documents.

For every follow-up, retain the finding ID, selected decision, accountable owner,
fix/restriction PR and tested commit, redacted acceptance evidence, reviewer verdict
and remaining limits. Reopen the applicable review if a later engine/dependency,
provider, packaging or policy change invalidates that evidence. This plan is ready
for documentation review only; the underlying publication gates remain unresolved.
