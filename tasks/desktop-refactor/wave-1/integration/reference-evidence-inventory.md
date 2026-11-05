# Reference And Evidence Inventory

R00-D adds [rendering-source-inventory.json](rendering-source-inventory.json):
60 rendering sources, 8 documentation entries, 1 progress metadata entry.
See [source receipt semantics](r00-d-resolution.md). Historical capture status
below remains unchanged; source integrity is not visual approval.

## Inventory Status

Verified by plan checker on PR #17 worktree:

| Metric | Count |
| --- | ---: |
| Screens | 10 |
| Flows | 10 |
| States | 112 |
| Nodes | 119 |
| Edges | 158 |

Verified independently from the merged `ketplus-desktop-redesign.ketatlas/atlas.json`:

| Metric | Count |
| --- | ---: |
| Screens | 10 |
| Unique URLs | 112 |
| Nodes | 119 |
| Edges | 158 |

State count by screen:

| Screen | States |
| --- | ---: |
| workspace | 41 |
| editor | 3 |
| changes | 3 |
| worktrees | 11 |
| atlas-library | 2 |
| atlas-builder | 13 |
| atlas-preview | 2 |
| connections | 20 |
| settings | 10 |
| search | 3 |

## Evidence Classes

| Evidence | Status | Notes |
| --- | --- | --- |
| Plan inventory checker | Verified | `node tasks/desktop-refactor/check-plan.cjs` passed in the plan worktree at `37a14562a11b6ec9fb7f137e9e93c95af464bc8d`. |
| Merged design artifact hash | Verified | `atlas.json` SHA-256 is `87f757c49043908dc57fcfe512a0634434a48c4d9e462071fd243ab4a0d9e780`. |
| Design system lock | Verified | `@ketvietlab/design-system` 0.1.7, revision `6f923d1e13958ce740e50bccab45efc56ccd2f84`. |
| Actual KetAtlas viewer visual reference | Blocked | No viewer/native capture permission was available in this handoff. Previous denial remains respected. |
| Native product screenshots | Blocked | No product UI implementation or capture performed. |
| Native pixel diff gate | Blocked | Requires approved reference images and matching native images. |
| Native performance baseline | Proposed only | Targets and fixtures are specified, but no release benchmark run happened in this handoff. |
| Backend contract behavior | Reviewed proposal only | No backend tests were run here. |
| Core transcript performance findings | Reviewed proposal only | No runtime module changed or benchmarked here. |
| Evidence validator negative tests | Verified | `node --test tests/design/integration-evidence.test.cjs` validates synthetic metadata failures without image artifacts. |

## Required Reference Packs

Each accepted screen/state needs:

- `reference.png` captured from the approved KetAtlas viewer or designer/user-approved manual equivalent.
- `native.png` from KetPlus Desktop at the same state.
- `diff.png` and alpha overlay.
- Geometry report: rects, padding/gap, border/radius, font metrics, colors, clipping, and focus bounds.
- Evidence JSON: task, screen, flow, node, URL, fixture hash, Desktop SHA, CM pin, design hash, environment, dimensions, DPR, capture time, reviewer, and result.

Do not commit personal screenshots, credentials, QR secrets, user transcript content, or absolute machine paths. Use synthetic fixtures only.

## Reference Approval Rule

Reference is not approved until designer/user confirms the actual viewer/native reference. Plan checks, graph counts, DOM/model tests, or CLI audit are evidence for structure only. They are not visual approval.

The valid no-capture baseline is allowed to pass structure validation only while visual readiness remains BLOCKED.
