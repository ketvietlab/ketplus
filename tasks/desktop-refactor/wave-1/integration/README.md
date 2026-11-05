# Wave 1 Integration Handoff

Latest: [R00-D source integrity, owner review and capability gates](r00-d-resolution.md).
The immutable rendering inventory covers 60 source files; visual approval remains BLOCKED.
Hosted [R00-D CI receipts](r00-d-ci-evidence.md) record exact stack revisions,
macOS/Linux artifacts and preserved failures separately from this document's HEAD.

Scope: baseline, design reference, acceptance, ownership, and QA coordination for the KetPlus Desktop redesign refactor. This is not a native UI implementation and does not approve any product screenshot.

Artifacts:

- [baseline-manifest.json](baseline-manifest.json): exact refs, dependency versions, environment, design hashes, and inventory counts.
- [ownership-register.md](ownership-register.md): Backend, Public CM, and Core/Transcript ownership lanes.
- [reference-evidence-inventory.md](reference-evidence-inventory.md): what is verified, what is not, and required evidence packs.
- [harness-specification.md](harness-specification.md): visual, functional, contract, and performance harness requirements.
- [public-safe-cm-brief.md](public-safe-cm-brief.md): safe handoff for the public KetPlus CM team.
- [backend-core-review.md](backend-core-review.md): review notes for Backend contract proposal and Core performance findings.
- [blockers-wave-2-gates.md](blockers-wave-2-gates.md): blockers and gates that must open before implementation wave 2.
- [decision-log.md](decision-log.md): proposed, reviewed, approved, and blocked decisions.
- [review-resolution-matrix.md](review-resolution-matrix.md): how R00-B review findings were resolved.
- [ci-triage-report.md](ci-triage-report.md): macOS CI failure triage for run `34675651929`.
- [evidence-format.md](evidence-format.md): R00-C schema, task prerequisites, PNG pairing and provenance.
- [task-readiness.md](task-readiness.md): generated task slices, checked against policy/manifest.
- [cross-team-interface-register.md](cross-team-interface-register.md): actual owner handoff review and unresolved decisions.
- [owner-review-sources.json](owner-review-sources.json): exact reviewed revisions and artifact digests.
- [ci-registration-plan.md](ci-registration-plan.md): separate workflow PR and dependency sequence.

Baseline sources:

- Desktop base: `origin/develop` at `7efc058d99b37f269fdebd5b69044fff32b84195`.
- Design artifact: [ketplus.ketatlas](../../../../ketplus.ketatlas/atlas.json), merged by PR #16 and repinned at the pixel-perfect + transcript-v2 integration revision `d0591869113ad4420961bff2ef2f2ac0ea840b58`.
- Plan source: PR #17 exact source revision `37a14562a11b6ec9fb7f137e9e93c95af464bc8d`. R00-B observed a squash merge on `origin/develop` at `73bddc13aab3425636fbd2f0a61860d95d13402c`; the source revision is not an ancestor of develop.
- Mandatory repository docs read: [project status](../../../../docs/project-status.md), [repository boundaries](../../../../docs/repository-boundaries.md), [architecture](../../../../docs/architecture.md), [development](../../../../docs/development.md), [testing](../../../../docs/testing.md), [product architecture](../../../../docs/product-architecture.md), and [licensing](../../../../docs/licensing.md).

Validation:

```sh
node tasks/desktop-refactor/wave-1/integration/check-integration.cjs
node --test tests/design/integration-evidence.test.cjs
node tasks/desktop-refactor/check-plan.cjs
git diff --check
```

The checker validates schema, paths including symlinks, links, atlas-derived counts, required assets from the pinned design tree, artifact digests/immutable sources, PNG integrity/pairing, approval provenance and every task prerequisite. It does not authenticate human consent or prove pixel correctness. Fetch exact PR #17 source when Git objects are absent; see [evidence-format.md](evidence-format.md).

Historical `verificationPerformed` pending/not-run fields describe the initial snapshot, not the current checker result. New owner review evidence has its own timestamp/revision; no historical capture is rewritten.

Current readiness summary:

- `R00-Audit`: READY from structure and inventory evidence.
- `R00-Visual-Reference`: BLOCKED pending designer/user approved actual viewer/native evidence.
- CM audit, Backend contract tests and Core headless characterization: READY to start, not completion or production proof.
- All contract-dependent implementation and visual acceptance slices: BLOCKED pending their own prerequisites.
