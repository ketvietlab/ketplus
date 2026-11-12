# Public-tree boundary hygiene

The mobile clients and hosted relay/account services are proprietary and maintained
in private repositories. Public documentation describes the product boundary and
public wire protocol without naming private repositories, revisions, implementation
paths, schemas or deployment infrastructure. No history was rewritten.

## Removed archival material

The cross-repository refactor work package mixed private implementation audits and
owner-review provenance into a public tree. It is retired here together with its
archival-only evidence validator and synthetic validator tests. Those tests checked
that retired internal workflow, not the product runtime. Public Atlas interaction,
repository-picker and sidebar tests remain. No acceptance is inferred from removed
private evidence. Public native acceptance instructions remain in docs/testing.md.

Removed files:

- `tasks/2026-09-12-transcript-architecture-review.md`
- `tasks/desktop-refactor/01-screen-feature-matrix.md`
- `tasks/desktop-refactor/02-api-alignment.md`
- `tasks/desktop-refactor/03-work-packages-and-agents.md`
- `tasks/desktop-refactor/04-pixel-performance-gates.md`
- `tasks/desktop-refactor/README.md`
- `tasks/desktop-refactor/check-plan-pinned.cjs`
- `tasks/desktop-refactor/check-plan.cjs`
- `tasks/desktop-refactor/coverage.json`
- `tasks/desktop-refactor/investigations/2026-09-12-live-chat-stream-performance.md`
- `tasks/desktop-refactor/r10-atlas-runtime.md`
- `tasks/desktop-refactor/wave-1/core-transcript/README.md`
- `tasks/desktop-refactor/wave-1/core-transcript/audit.md`
- `tasks/desktop-refactor/wave-1/core-transcript/baseline-report.md`
- `tasks/desktop-refactor/wave-1/core-transcript/handoff.md`
- `tasks/desktop-refactor/wave-1/core-transcript/production-characterization.md`
- `tasks/desktop-refactor/wave-1/core-transcript/r03-c-baseline-resolution.md`
- `tasks/desktop-refactor/wave-1/core-transcript/r03-c-raw-release.json`
- `tasks/desktop-refactor/wave-1/core-transcript/r03-d-raw-release.json`
- `tasks/desktop-refactor/wave-1/core-transcript/r03-d-resolution.md`
- `tasks/desktop-refactor/wave-1/core-transcript/r03-e-implementation-plan.md`
- `tasks/desktop-refactor/wave-1/core-transcript/r03-e0-command-privacy.md`
- `tasks/desktop-refactor/wave-1/core-transcript/r03-e0-ctest.log`
- `tasks/desktop-refactor/wave-1/integration/README.md`
- `tasks/desktop-refactor/wave-1/integration/backend-core-review.md`
- `tasks/desktop-refactor/wave-1/integration/baseline-manifest.json`
- `tasks/desktop-refactor/wave-1/integration/blockers-wave-2-gates.md`
- `tasks/desktop-refactor/wave-1/integration/check-characterization.cjs`
- `tasks/desktop-refactor/wave-1/integration/check-integration.cjs`
- `tasks/desktop-refactor/wave-1/integration/ci-registration-plan.md`
- `tasks/desktop-refactor/wave-1/integration/ci-triage-report.md`
- `tasks/desktop-refactor/wave-1/integration/cross-team-interface-register.md`
- `tasks/desktop-refactor/wave-1/integration/decision-log.md`
- `tasks/desktop-refactor/wave-1/integration/evidence-format.md`
- `tasks/desktop-refactor/wave-1/integration/evidence-validator.cjs`
- `tasks/desktop-refactor/wave-1/integration/gate-policy.cjs`
- `tasks/desktop-refactor/wave-1/integration/harness-specification.md`
- `tasks/desktop-refactor/wave-1/integration/manifest-shape.cjs`
- `tasks/desktop-refactor/wave-1/integration/owner-review-sources.json`
- `tasks/desktop-refactor/wave-1/integration/ownership-register.md`
- `tasks/desktop-refactor/wave-1/integration/png-integrity.cjs`
- `tasks/desktop-refactor/wave-1/integration/public-safe-cm-brief.md`
- `tasks/desktop-refactor/wave-1/integration/r00-d-ci-evidence.md`
- `tasks/desktop-refactor/wave-1/integration/r00-d-resolution.md`
- `tasks/desktop-refactor/wave-1/integration/reference-evidence-inventory.md`
- `tasks/desktop-refactor/wave-1/integration/render-readiness.cjs`
- `tasks/desktop-refactor/wave-1/integration/rendering-source-inventory.json`
- `tasks/desktop-refactor/wave-1/integration/rendering-source.cjs`
- `tasks/desktop-refactor/wave-1/integration/review-resolution-matrix.md`
- `tasks/desktop-refactor/wave-1/integration/task-readiness.md`
- `tests/design/integration-evidence.test.cjs`
- `tests/design/integration-synthetic.cjs`

## Rewritten files

Private-source links, revisions and module references were removed from ownership,
licensing, architecture and transport documents. Sample repository names and paths
were replaced in native/mock/Atlas fixtures and their assertions. Adapter provenance
now identifies the published package version and artifact hashes rather than private
source revisions. The generated adapter changes only its provenance comment; its
recorded digest was refreshed. Existing license/copyright notices are unchanged.

- `.github/workflows/build.yml`
- `AGENTS.md`
- `apps/browser/README.md`
- `apps/desktop/src/app/ArchivedSettingsPage.cpp`
- `apps/desktop/src/remote/RemoteControlView.cpp`
- `apps/desktop/src/workspace/SidebarRepositoryList.h`
- `benchmarks/README.md`
- `benchmarks/results/2026-10-12-webrtc-datachannel-macos-m1-pro.md`
- `core/docs/design-system.md`
- `core/docs/licensing.md`
- `core/docs/mermaid-theme-review.md`
- `core/resources/mermaid/SOURCE.json`
- `docs/decisions/0021-default-agent-skills.md`
- `docs/decisions/0024-webrtc-transport-and-forward-only-relay.md`
- `docs/decisions/0029-one-public-repository-two-editions.md`
- `docs/decisions/0030-integration-branch.md`
- `docs/decisions/0116-teams-on-paired-devices.md`
- `docs/licensing.md`
- `docs/linux-headless-verification.md`
- `docs/product-architecture.md`
- `docs/remote-transport.md`
- `docs/repository-boundaries.md`
- `docs/testing.md`
- `docs/webrtc-transport-spike.md`
- `ketplus.ketatlas/DESIGN.md`
- `ketplus.ketatlas/FLOW-CONTRACT.md`
- `ketplus.ketatlas/README.md`
- `ketplus.ketatlas/REVIEW-2026-09.md`
- `ketplus.ketatlas/assets/ket-design-system-contracts.js`
- `ketplus.ketatlas/atlas.json`
- `ketplus.ketatlas/atlas.progress.json`
- `ketplus.ketatlas/design-system.lock.json`
- `ketplus.ketatlas/screens/connections-intro.css`
- `ketplus.ketatlas/screens/conversation-tags.js`
- `ketplus.ketatlas/screens/conversations.js`
- `ketplus.ketatlas/screens/desktop-alignment.css`
- `ketplus.ketatlas/screens/provider-controls.js`
- `ketplus.ketatlas/screens/repository-picker-model.js`
- `ketplus.ketatlas/screens/repository-picker.js`
- `mockups/qt/README.md`
- `mockups/qt/qml/Fixtures.qml`
- `mockups/qt/qml/Main.qml`
- `tasks/2026-09-12-pr16-design-review.md`
- `tasks/backend-hardening/closure-ledger.md`
- `tests/ChatMarkdownTest.cpp`
- `tests/AgentWorkspaceRenderingTest.cpp`
- `tests/AgentWorkspaceShellTest.cpp`
- `tests/ExplorerPanelTest.cpp`
- `tests/SidebarTeamsTest.cpp`
- `tests/TeamPageTest.cpp`
- `tests/TranscriptInteractionTest.cpp`
- `tests/daemon/DaemonLifecycleTest.cpp`
- `tests/daemon/DaemonProtocolTest.cpp`
- `tests/daemon/fixtures/team-remote-v1.json`
- `tests/design/ketatlas-flows.test.cjs`
- `tests/design/repository-picker.test.cjs`

## Deliberate public names and network fixtures retained

- `ketplus_control_crypto` and `ketplus_remote_control`: exported public CMake
  targets in modules/remote and their consumers; these are public API names.
- `ketplus-app-icon.png`: the public app-icon filename, not a repository link.
- `@ketvietlab/*`, public publisher domains and KETVIET JSC attribution: package
  publisher identity and license notices, not private repository ownership.
- Network-classification ranges in apps/browser/src/network-address.mjs and
  origin-policy.mjs: protocol/policy constants, not deployment addresses.
- Synthetic addresses under tests/, core/tests/ and apps/browser/test/: fixtures
  exercising routing/security policy. In particular the address-kind test retains
  its carrier-grade-NAT example; no deployment host is documented by that fixture.
- Loopback and RFC 5737 documentation ranges: local test endpoints and explicit
  examples. A browser README example now uses the documentation range.

The Python guard scans Git-tracked UTF-8 files, rejects private reference patterns,
and prints only file paths/rule names. Its only full-file exception is its own
intentional rejection corpus. Address-classification exceptions apply only to IP
checks, so all other patterns still apply to those source and test files. CI runs
it in the existing Linux daemon job, before compilation. It is a text boundary guard,
not a secret scanner or a replacement for reviewing publication provenance.

A lightweight pull-request workflow also runs the same guard for documentation-only
changes (including integration), which the existing build workflow skips. This does
not enable native builds on integration.

## Review hardening

The guard checks both tracked paths and contents, including clone URL suffixes,
sentence-final repository names, underscore infrastructure prefixes and production
configuration paths. Its regression corpus exercises those forms. Remaining sample
chat/mobile repository and branch names are neutral examples. Mermaid provenance
keeps package version and content hashes without asserting unverified package paths.
The archival removal retires a mixed cross-repository workflow as one unit; keeping
its validators or partial evidence would imply an acceptance trail that is no longer
available publicly. Runtime tests and public validation instructions remain.
