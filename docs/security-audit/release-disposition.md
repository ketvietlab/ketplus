# Security audit disposition and publication plan

> Historical snapshot: the findings and status below describe the original
> report scope, not the latest remediation state. See the
> [current remediation register](remediation-status.md) for reviewed merge evidence
> and remaining gates. The [accepted report version](https://github.com/ketvietlab/ketplus/blob/90e5ca47dcd5931c0f8f0bcc42c28f960ce3d41d/docs/security-audit/release-disposition.md)
> remains available unchanged; this annotation does not revise its conclusions.

Plan date: 2026-11-16. Task: T-3. Status: proposed work and pending decisions;
publication readiness is **not established** by this document.

This plan uses two accepted reports.
[T-2's public-surface audit](public-surfaces.md), report head
`05ee9a8ad4adb57870b610f659052c6e03a024d1`, was accepted in
[review round 2](https://github.com/ketvietlab/ketplus/pull/705#issuecomment-6909286234).
[PR #705](https://github.com/ketvietlab/ketplus/pull/705) merged into `integration`
at `769e45b75157a82e45d9c5abf2b5cfcb4dc83ef3`.

[T-1's accepted daemon/agent audit](https://github.com/ketvietlab/ketplus/blob/9cf3a535d1eeadcb3c70c78ad61455a179648cbb/docs/security-audit/daemon-agent.md),
English report head `9cf3a535d1eeadcb3c70c78ad61455a179648cbb`, was accepted in
[review round 1](https://github.com/ketvietlab/ketplus/pull/706#issuecomment-6909585411).
[PR #706](https://github.com/ketvietlab/ketplus/pull/706) merged into `integration`
at `eaafe6b32fb3550b02e8aaca7dbab04cb0da58b8`.
Both reports audit implementation baseline
`7c12c773372b1d63eae867b7a6f8dcfefb6cdbbb`; accepting or merging them did not fix
product behavior or clear publication gates. This plan uses those accepted
reports and does not attest intervening implementation changes.

This task changes only this plan. It does not implement the proposals, create
remediation tasks, choose product/history policies, rewrite Git history, repeat
T-1's investigation, build packages, deploy, or authorize publication. Module
owners below are proposed ownership boundaries from
[repository boundaries](../repository-boundaries.md), not new assignments to people.
The leader assigns follow-up tasks after the relevant decisions are recorded.

## Disposition register

All nine findings remain **open for disposition** in this plan. “Accepted report”
means that its evidence and limits passed documentation review, not that a risk
was accepted for release. Future closure needs an implementation or an approved,
verified scope restriction, evidence at the release candidate, and reviewer signoff.

| Finding | Priority carried from accepted audit | Owning module or role | Proposed work package | Decision dependency |
| --- | --- | --- | --- | --- |
| PS-01: implicit Atlas renderer execution | High | `modules/atlas`; `apps/desktop` entry points | A-01: separate viewing from renderer authorization | D-01 |
| PS-02: historical publication boundary | High | Repository maintainers; public-tree guard and publication workflow owners | A-02: exact-ref inventory, private triage and publication-safe history | D-02 |
| PS-03: Git helpers on automatic inspection | Medium | `core` Git services; shared workspace trust, with Lite/Pro composition | A-03: prevent unapproved helper execution in background queries | D-03 |
| PS-04: browser download integrity | Medium | `apps/browser` installer/engine; `modules/browser` host settings | A-04: enforce the selected executable provenance policy | D-04 |
| PS-05: incomplete binary distribution evidence | Medium | Packaging/release maintainers; desktop/core notice surfaces; licensing reviewer | A-05: per-artifact dependency, notice and source-offer gates | Existing distribution policy; release scope still needs explicit approval |
| PS-06: disabled Chromium sandbox | Medium | `apps/browser` launch policy; `modules/browser` host integration | A-06: sandboxed launches and supported-runtime acceptance | D-05 |
| DA-01: key distribution after sign-out | High | `apps/daemon` cloud-control lifecycle, pins and key distribution | A-07: fence stale callbacks and explicit legacy adoption | Existing sign-out/pin trust boundary; no new product policy required |
| DA-02: sender-key cache survives enrollment reset | High | `apps/daemon` sender-key cache and command authorization | A-08: invalidate caches and recheck current authorization | Existing enrollment/pairing boundary; no new product policy required |
| DA-03: credential-storage contract mismatch | Informational | `modules/remote` credential store; architecture/ADR owners | A-09: reconcile the storage policy and implementation | D-06 |

The finding IDs, severities, conditions and original source locations are retained
in [T-2's accepted report](public-surfaces.md#findings) and
[T-1's accepted report](https://github.com/ketvietlab/ketplus/blob/9cf3a535d1eeadcb3c70c78ad61455a179648cbb/docs/security-audit/daemon-agent.md).
This register proposes work; it does not expand conditional findings into claims
of an unauthenticated exploit.

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
| D-06: credential-storage model | Implement the documented OS credential-store model on supported platforms; or explicitly approve file-backed storage and revise the architecture/ADR contract with verified platform controls | OS storage needs signing/access-prompt, headless and migration acceptance. File permissions do not provide encryption or protection from every process under the same OS user; define backup handling, ACLs and the residual at-rest exposure. DA-03 establishes a contract discrepancy, not a demonstrated cross-user bypass. Neither choice is made here. |

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

### A-07: fence account-transition callbacks (DA-01)

Preserve the existing sign-out and device-pin trust boundary in `apps/daemon`.
Capture the account epoch and machine/workspace identity for asynchronous work;
reject stale or unenrolled callbacks before every mutation, pin adoption, wrap,
refresh or upload. Missing pins alone must not authorize legacy adoption; require
explicit migration state. Include the related pairing approval/revocation
callbacks and fallback sender-key adoption identified by T-1.

Acceptance evidence: fake transport holds a device-list reply, completes sign-out,
then returns a synthetic list; assert no new pin, key wrap, device refresh or
key-upload request. Also cover sign-out followed by a different enrollment before
the old reply, and a genuine permitted migration. No real account, provider or
network is needed. Record the native regression target, exact fix commit and
reviewer result. T-1's source analysis and reviewer verification confirm the
reported condition; they do not demonstrate closure at a release candidate.

### A-08: bind sender-key caches to current authorization (DA-02)

Clear the cache on every account transition, fence delayed lookups by account
epoch, and recheck the current pin on cache hits. Retain revocation invalidation;
a cached key must not substitute for current pairing authority. This preserves
the existing policy rather than requiring a new user-selected trust model.

Acceptance evidence: pair device D and warm its cache, sign out, enroll again in
the same workspace within the cache TTL, then require lookup D to fail until new
pairing approval. Cover delayed lookup replies across sign-out and revocation,
and normal authorized cache reuse without unnecessary network requests. Attach
native regression and command-authorization results at the exact fix commit;
distinguish key-lookup assertions from any end-to-end command-execution evidence.
Do not expand the finding into arbitrary signature forgery, cross-workspace key
reuse or a demonstrated provider sandbox escape.

### A-09: reconcile the credential-storage contract (DA-03)

After D-06, make the architecture/ADR, implementation and user-facing protection
claims agree. `modules/remote` owns the store; architecture and platform owners
review the selected model. An OS-store implementation needs an explicit existing
credential migration/recovery plan and supported desktop/headless behavior. If
file-backed storage is approved, document its actual protection and limits;
base64 serialization must not be described as encryption.

Acceptance evidence: an approved storage decision and matching documentation,
plus platform-specific ACL, backup and access-behavior checks. Where storage
changes, use synthetic credentials to verify migration, failure/retry, sign-out
and intended access-prompt behavior without exposing real values. A documentation
correction alone cannot establish the promised OS-store protection. Retain the
Informational classification unless separate evidence justifies a different
finding; no storage-policy choice or implementation is made by this plan.

## Audit and verification gaps

As of the 2026-11-16 disposition update, T-1 is **accepted and merged** at the
English report head and integration merge recorded above. Its accepted scope is
`apps/daemon`, `modules/remote` and `modules/ai`: authentication/IPC, credential
lifecycle, transport, tool authorization and conversation isolation. DA-01/DA-02
remain High findings requiring A-07/A-08 evidence; DA-03 requires the D-06/A-09
storage disposition. T-3 derives these work items from the accepted report and
verdict without repeating T-1's code investigation. Report acceptance closes the
documentation-review dependency, not remediation or release verification.

T-1's six fake-SDK sidecar checks are not DA-01/DA-02 regression tests or proof of
real provider sandbox enforcement. The accepted review's synthetic verification
is not a full native-suite or end-to-end command-execution result. The audit did
not execute `AgentMessagingTest`, `DaemonProtocolTest` or
`cloud-privacy/CloudPrivacyTest`, or a native build/full ctest. Its complete
team/schedule role matrix, every filesystem/attachment operation, WebRTC/native
transport, every adapter extension and the entire legacy direct transport were
not deeply audited. Cryptographic, OS sandbox and hosted-authorization behavior
beyond the reviewed code remains unverified; acceptance of T-1 is not clearance
of those limits.

T-2's [coverage and executed-check limits](public-surfaces.md#executed-checks-and-handoff)
remain applicable to its pinned source. The 99 deterministic tests, harmless
fixtures and redacted scans support the stated offline conclusions; they do not
validate a release artifact or certify later commits. In particular:

| Outstanding evidence | Proposed owner | Required before the applicable readiness conclusion |
| --- | --- | --- |
| DA-01/DA-02 remediation and trust-transition regressions | Daemon owner and reviewer | A-07/A-08 closure at the fix/candidate commit; accepted report and synthetic review evidence do not establish a fixed daemon |
| Credential-storage policy and platform protection | Remote/platform owners and user | D-06/A-09 decision and verified implementation/contract; no assumed OS credential-store protection |
| T-1's unexecuted native checks and coverage limits | Daemon/remote/AI owners and release maintainer | Candidate-specific required native matrix and separately scoped assurance for the limits above; no assumed provider or hosted-service clearance |
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
| Audit report acceptance | T-2 round 2 and T-1 round 1 accepted reports and merges above: verified | Leader reviews the release-candidate delta and scopes remaining assurance; accepted reports do not certify later changes |
| Product remediation/disposition | Six accepted T-2 findings and three accepted T-1 findings: recorded; closure unverified | User/owners decide D-01–D-06; leader assigns work; module owners supply A-01/A-03/A-04/A-06/A-07/A-08/A-09 evidence and reviewer accepts it |
| Source publication | T-2 historical boundary finding: open | Repository owners approve and verify A-02; complete applicable source licensing/secret/transcript review for the exact publication contents |
| Binary distribution | T-2 engineering fixtures: verified only for their stated scope; artifact acceptance unverified | Release/licensing owners complete A-05 and all supported artifact/runtime checks in the release process |
| Publish/deploy authorization | None requested or granted by this plan | User/release owner explicitly authorizes the concrete candidate through the existing release flow after applicable gates hold |

The leader can create the nine work packages independently where their decision
dependencies permit, but should reconcile browser provenance and sandboxing on the
same candidate and verify the interacting DA-01/DA-02 account transitions together.
Repository-owner history decisions must precede any history
mutation. No release conclusion may be inferred from merging these documents.

For every follow-up, retain the finding ID, selected decision, accountable owner,
fix/restriction PR and tested commit, redacted acceptance evidence, reviewer verdict
and remaining limits. Reopen the applicable review if a later engine/dependency,
provider, packaging or policy change invalidates that evidence. This plan is ready
for documentation review only; the underlying publication gates remain unresolved.
