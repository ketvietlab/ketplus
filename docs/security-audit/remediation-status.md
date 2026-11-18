# Security audit remediation status

Reconciliation date: 2026-11-17. Task: T-16. Integration evidence cutoff:
[`4f6493c4bdbb4f56822b65233264299ff8e24923`](https://github.com/ketvietlab/ketplus/commit/4f6493c4bdbb4f56822b65233264299ff8e24923).
This is a documentation reconciliation, not a new runtime audit, risk acceptance,
release approval or authorization to publish source/history/binaries.

## Historical reports and status semantics

The original [public-surfaces report](public-surfaces.md) and
[daemon/agent report](daemon-agent.md) describe implementation baseline
`7c12c773372b1d63eae867b7a6f8dcfefb6cdbbb`. Their finding text, priorities,
reproductions, test results and source-line references remain historical evidence.
The [disposition plan](release-disposition.md) records proposals and unresolved
choices at its original report head. Statements such as "all findings remain
open" in those historical documents must not be read as a current merge register.
This addendum supplies that register without rewriting what was observed then.

An accepted audit means its evidence/limits passed report review. A reviewed,
merged remediation means the named task's implementation and scoped checks were
accepted at its exact head. Neither proves every related path, supported platform,
installed artifact or later regression is secure. Findings can have merged fixes
and still require additional assurance; new follow-up work does not erase earlier
scoped remediation or retroactively change an audit's baseline.

## Seven merged audit/remediation PRs

GitHub PR metadata was checked for merged state, integration base, exact head and
merge commit. Each merge and reviewed head is reachable from the evidence cutoff.
These are seven Security Audit PRs, not a count of all PRs in that integration
snapshot. The first three are documentation; the last four contain remediation.
The head links identify the reviewed task head; merge links identify integration
ancestry. Evidence from a prior tested code-equivalent commit is scoped separately.

| PR / task | Reviewed head | Integration merge | Recorded scope |
| --- | --- | --- | --- |
| [#705](https://github.com/ketvietlab/ketplus/pull/705) / T-2 | [`05ee9a8ad4ad`](https://github.com/ketvietlab/ketplus/commit/05ee9a8ad4adb57870b610f659052c6e03a024d1) | [`769e45b75157`](https://github.com/ketvietlab/ketplus/commit/769e45b75157a82e45d9c5abf2b5cfcb4dc83ef3) | Accepted PS-01–PS-06 historical report; no product fix. |
| [#706](https://github.com/ketvietlab/ketplus/pull/706) / T-1 | [`9cf3a535d1ee`](https://github.com/ketvietlab/ketplus/commit/9cf3a535d1eeadcb3c70c78ad61455a179648cbb) | [`eaafe6b32fb3`](https://github.com/ketvietlab/ketplus/commit/eaafe6b32fb3550b02e8aaca7dbab04cb0da58b8) | Accepted DA-01–DA-03 historical report; no product fix. |
| [#707](https://github.com/ketvietlab/ketplus/pull/707) / T-3 | [`90e5ca47dcd5`](https://github.com/ketvietlab/ketplus/commit/90e5ca47dcd5931c0f8f0bcc42c28f960ce3d41d) | [`85ec7b0e8cbd`](https://github.com/ketvietlab/ketplus/commit/85ec7b0e8cbd1106eb91afb7409cb0abb4407fa8) | Accepted disposition plan; no release approval or product fix. |
| [#709](https://github.com/ketvietlab/ketplus/pull/709) / T-4 | [`efb3f5e29e92`](https://github.com/ketvietlab/ketplus/commit/efb3f5e29e926519373e9ed365bd4e27cd1c1082) | [`7e6b267c73f0`](https://github.com/ketvietlab/ketplus/commit/7e6b267c73f04cccc2a237d0b8126bd1ac70636a) | DA-01 callback/enrollment fencing remediation merged; scoped synthetic regressions reviewed. |
| [#713](https://github.com/ketvietlab/ketplus/pull/713) / T-5 | [`d884425e7629`](https://github.com/ketvietlab/ketplus/commit/d884425e76292f3de2e233f93a46b545823bae10) | [`728498452a71`](https://github.com/ketvietlab/ketplus/commit/728498452a717ceb10bd52877ca7678557481a59) | DA-02 cache invalidation/pin rechecks remediation merged; scoped synthetic regressions reviewed. |
| [#711](https://github.com/ketvietlab/ketplus/pull/711) / T-7 | [`bbba178fee53`](https://github.com/ketvietlab/ketplus/commit/bbba178fee530900bb57b3f36b798bcaae54b064) | [`52cd89fd2bbe`](https://github.com/ketvietlab/ketplus/commit/52cd89fd2bbe459d7bc006b7529a2d9299d6adcd) | PS-01 desktop per-launch consent remediation merged; scope remains the reviewed entry points. |
| [#717](https://github.com/ketvietlab/ketplus/pull/717) / T-6 | [`49663b9c1c50`](https://github.com/ketvietlab/ketplus/commit/49663b9c1c506e0b87a7ac74c03353a860c9d692) | [`1c87abda5369`](https://github.com/ketvietlab/ketplus/commit/1c87abda53694256780642bc4d93f114f798e2ca) | DA-03 native OS-store implementation/contract remediation merged; platform acceptance remains bounded below. |

For #711, the final merge-reconciliation head is bbba178fee53; earlier renderer
checks do not themselves prove the final delta. The task records supplemental
review of that exact head. For #717, the final head changes ADR evidence only
after fully tested code-equivalent `fab40c16ddd8850aee9c2f97fc2afa66e0950e9d`.
[ADR 0130 at the reviewed head](https://github.com/ketvietlab/ketplus/blob/49663b9c1c506e0b87a7ac74c03353a860c9d692/docs/decisions/0130-os-credential-storage.md)
records the unfiltered macOS run: 213 passed, zero failed, one Linux-only suite
skipped out of 214. This is attributed implementation evidence, not a suite rerun
by this documentation task or Linux/Windows acceptance.

## Remaining gates from the original reports

| Finding / work | Status at reconciliation | Evidence still required |
| --- | --- | --- |
| PS-01 / T-7 | Desktop remediation reviewed and merged in #711. | Preserve its entry-point/fixture scope; additional follow-up review stays separate. No real renderer, installed-package or OS-containment acceptance follows from fake CLI tests. |
| DA-01 / T-4 and DA-02 / T-5 | Remediation reviewed and merged in #709/#713. | Keep the tested callback/cache boundaries distinct from complete trust lifecycle assurance and the separately tracked follow-up work. |
| DA-03 / T-6 | Contract and native storage implementation reviewed and merged in #717. | Native Linux Secret Service/headless policy, signed upgrade/access-control prompts, backups and recovery tooling are not verified by that merge. |
| PS-04 / T-8 | Blocked. | Independently publisher-authenticated executable identity, enrollment and release trust-chain evidence for the exact supported artifacts. HTTPS/cache hashing alone is insufficient. |
| PS-06 / T-9 | Pending T-8. | Sandboxed launches and compatibility on supported runtime/platform combinations; no silent sandbox-disabled fallback. |
| PS-05 / T-10 | Blocked; #710 is unmerged tooling/dossier work, not artifact acceptance. | Fresh native artifact/source identity, complete embedded/dependency inventory, applicable source/notices/relink evidence, publisher source access and responsible licensing review. A reviewed recreation plan is not a build or licensing acceptance. |
| PS-03 / T-11 | Blocked by T-10. | Automatic-query helper suppression and focused/native Lite checks under the selected policy; no completion inferred from an ordinary clone. |
| PS-02 / T-12 | Blocked by T-11. | Exact advertised/local ref and archive coverage, reachable-content/message scan, restricted triage and a reviewed coordinated-cleanup/recovery proposal. No shared-history rewrite, ref deletion or force-push is authorized here. |

## Assurance and disclosure limits

Source-confirmed behavior, measured fixture results, inference about broader
runtime effects and untested hypotheses are different evidence classes. A source
trace is not an end-to-end reproduction; a timing result for one API does not
establish whole-application responsiveness. A hypothesis must not be labelled a
demonstrated exploit, and a local test must not be labelled installed/released
acceptance.

The later audit's detailed finding record and reproduction evidence remain in
restricted task material under the existing [security reporting policy](../../SECURITY.md).
This public addendum does not introduce new suspected-vulnerability details,
reproduction steps, private logs, infrastructure identities or a disclosure
policy. Repository owners control any subsequent disclosure decision.

Windows reader compile/runtime coverage for T-7, native Linux Secret Service and
headless operation, signed Desktop upgrade/keychain ACL behavior and recovery
tooling remain explicitly unverified.
Synthetic stores, a disposable test keychain and private D-Bus fixtures do not
establish those production properties. Documented manual recovery is not proof
that legacy-file/OS-store conflict or downgrade recovery tooling exists or has
passed acceptance.

No new provider call, service probe, credential read, native build, packaging,
installation, signing, deployment, history mutation or runtime regression test
was performed by T-16. Its checks cover documentation consistency, exact public
PR/commit identity and public-tree hygiene. Source publication and binary release
retain independent gates in [licensing](../licensing.md#release-gates) and the
[release process](../release-process.md). There is no release/publication-ready
conclusion and no authorization to publish or delete security findings.
