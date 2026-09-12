# Public CM refactor wave 1

Wave 1 audits native public foundations and proposes public extension seams for
future consumers. It does not port private product UI and does not change default
KetPlus CM behavior.

Base revision:

- Public repository: `ketvietlab/ketplus`
- Base branch: `origin/develop`
- Base SHA: `7ac9d758f1b095b4f85362d40fe4b6e55c261680`

Design reference:

- Package: `@ketvietlab/design-system` `0.1.7`
- Revision: `6f923d1e13958ce740e50bccab45efc56ccd2f84`
- Source read: `packages/design-system/src/foundations/tokens.css`
- Source Git blob: `032e5af9eaf27a46a00fab2c8cdac94c4a407c26`
- Source SHA-256: `f01c77596ac50cfba55d271e2b32fbe90bc60c238affebc46fea5cd4eb8149c1`
- Status: consumed as public reference only; canonical tokens/assets were not
  edited, copied, version-bumped, or vendored.

Files in this directory:

- [current-capability-matrix.md](current-capability-matrix.md)
- [proposed-public-interfaces.md](proposed-public-interfaces.md)
- [token-metric-mapping.md](token-metric-mapping.md)
- [compatibility-risks.md](compatibility-risks.md)
- [test-plan.md](test-plan.md)
- [release-slices.md](release-slices.md)

Scope guardrails:

- `ketjs` and `ketatlas` are read-only references.
- No private Desktop source, bundle, screenshot, transcript, workflow, or
  credential is copied or linked here.
- Visual measurements not approved by Integration/QA remain `Pending QA`.
- Proposed public APIs remain generic. Consumer identifiers are opaque and
  consumer-specific policy stays in the consumer.

Evidence state at R01-B:

- source pin and current behavioral/native baselines: verified locally;
- API proposals: pending review approval;
- native visual measurements: Pending QA;
- production API/runtime implementation: not started.
