# Review Resolution Matrix

## R00-D

See the [finding-resolution table and reproducer](r00-d-resolution.md).
Deleted HTML, CSS/JS drift, omitted source receipt entries and repaired local
hashes now fail against the immutable Git tree. No reference approval granted.

## R00-C

| Finding | Resolution / tests | Status |
| --- | --- | --- |
| Inventory evidence opens consumers without frozen contract | `gate-policy.cjs` enforces every required prerequisite ID/category/status; tests cover legacy reproducer, changed prerequisite and forged READY with correct IDs. | resolved |
| Task rows too broad; JSON and Markdown differ | Separate audit, headless characterization, per-task implementation and visual acceptance; generated `task-readiness.md` parity is mandatory. | resolved |
| Pixel passed with JSON artifacts and blocked reference | Approved reference ID/revision required, decoded PNGs, capture metadata, hashed comparison pairing/report and approval provenance; synthetic negative tests cover all bypasses. | resolved |
| Submitted list defines its own required assets | Required set derived from immutable design Git tree; every asset must match pinned blob and submitted digest. Truncated asset list fails. | resolved |
| Missing rows/IDs/types/timestamps and unverified sourceRevision | Required sets and uniqueness, typed fields, valid timestamps/revisions, verified Git blobs and structured fail-closed errors; malformed and symlink tests added. | resolved |
| Approval boolean without artifact/reviewer | Digest-bound approval record with matching evidence/revision/reviewer/role and provenance; machine validation explicitly does not authenticate consent. | resolved |
| Historical no-capture versus future evidence | Historical fields retained; positive test-only scoped capture accepted structurally while global coverage stays BLOCKED; normal validation rejects synthetic evidence. | resolved |
| No actual owner handoff review | Exact Backend/CM/Core revisions and artifact hashes in `owner-review-sources.json`; I01-I10 decisions in cross-team register. Core tests independently run; Backend/CM tests source-reviewed only. | reviewed; owner decisions unresolved |
| Node checks not registered in CI | Separate draft workflow PR/dependency sequence in `ci-registration-plan.md`; no workflow edit in #18. | pending dependency merge and CI review |

Tests: `node --test tests/design/integration-evidence.test.cjs`. All three
reported reproducers are required rejection tests. See [evidence-format.md](evidence-format.md)
for commands and the boundaries of automated validation.

## R00-B Historical Resolution

The earlier resolutions below were incomplete where superseded by R00-C above.

| Finding | Resolution | Evidence | Status |
| --- | --- | --- | --- |
| Ownership was too broad and could be read as authorizing implementation. | Split each lane into `Allowed now -- wave 1` and `Future implementation ownership`; listed concrete path patterns and explicit non-authorized runtime areas. | [ownership-register.md](ownership-register.md) | Resolved |
| Gates were described as a whole-wave checklist. | Replaced with task-by-task prerequisite/evidence/reviewer/status matrix. | [blockers-wave-2-gates.md](blockers-wave-2-gates.md) | Resolved |
| Blockers could be mistaken for passed prerequisites. | Matrix states that recording a blocker is not a pass; READY is only used for audit/harness items with passing structure evidence. | [blockers-wave-2-gates.md](blockers-wave-2-gates.md), [baseline-manifest.json](baseline-manifest.json) | Resolved |
| Manifest included machine-specific path and checkout noise. | Removed absolute worktree path and personal checkout status; kept branch, PR, and immutable revisions. | [baseline-manifest.json](baseline-manifest.json) | Resolved |
| Manifest mixed historical baseline and new evidence. | Preserved historical baseline refs and added R00-B observed develop merge/revision plus evidence entries with recordedAt/revision. | [baseline-manifest.json](baseline-manifest.json) | Resolved |
| Evidence status was not schema-validated. | Added validator module and negative tests for missing revisions, invalid path, hash mismatch, invalid pass/approval claims, and pixel pass without artifacts. | [evidence-validator.cjs](evidence-validator.cjs), [integration-evidence.test.cjs](../../../../tests/design/integration-evidence.test.cjs) | Resolved |
| Inventory checker trusted manifest counts. | Validator now derives screens/flows/states/nodes/edges, statesByScreen, nodesByFlow, and edgesByFlow from `atlas.json` and compares to manifest. | [evidence-validator.cjs](evidence-validator.cjs) | Resolved |
| Asset hash validation was incomplete. | Manifest declares each asset path/hash and validator checks every declared asset exists and hashes correctly. | [baseline-manifest.json](baseline-manifest.json), [evidence-validator.cjs](evidence-validator.cjs) | Resolved |
| Path safety used prefix-like logic. | Validator rejects absolute paths and path escapes using `path.relative` and realpath containment for existing files. | [evidence-validator.cjs](evidence-validator.cjs) | Resolved |
| Backend/Core review provenance was ambiguous. | Review now states it covers PR #17 plan artifacts only, not owner handoff artifacts or runtime results. | [backend-core-review.md](backend-core-review.md) | Resolved |
| Visual gate could be misread as approved by structure checks. | Visual and native-pixel evidence remain `blocked`; tests verify valid no-capture baseline passes structure while readiness stays BLOCKED. | [baseline-manifest.json](baseline-manifest.json), [integration-evidence.test.cjs](../../../../tests/design/integration-evidence.test.cjs) | Resolved |
| CI failure needed triage without masking runtime test. | Added observed/suspected/unresolved report and did not alter runtime tests, timeout, or UI code. | [ci-triage-report.md](ci-triage-report.md) | Resolved |

No resolution in this matrix claims visual approval, Backend approval, CM approval, Core approval, Wave 2 readiness, or native UI implementation acceptance.
