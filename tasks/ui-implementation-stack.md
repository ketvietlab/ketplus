# Két UI implementation stack

## Execution agreement — 2026-11-07

Implement phases 1–5 of the conversation's manual UI plan as stacked pull
requests. Finish the complete stack before requesting review. Do not merge or
start the review phase during implementation. The user authorized proceeding
with the current Qt mock; no additional mock-approval checkpoint is required.

Excluded from this stack: phase 6 (conversation Plan / daemon continuation,
issue #198), phase 7 (overall acceptance, release packaging, deployment and
device installation), and Repo Memory implementation. Keep technical checks
for each patch explicit; they do not imply final acceptance or deployment.

## Source and ownership

The baseline is `ketplus/integration`, the public two-edition source tree
(ADR 0029). `core/` remains independent of Pro. Shared components belong to
`ket-qt`; product composition consumes its public APIs. Do not copy source
from the old private Desktop checkout or from mobile/hosted-service repos.

The native design reference is `mockups/qt` and `mockups/terminal` (ADR 0033–34).
The conversation Plan mock in PR #197 remains separate from this UI stack.

## Checklist

Check a row only when its implementation is committed and pushed with a PR.
Record technical verification separately from the later review/acceptance phase.

- [x] Reconcile the current repository layout and merged baseline.
- [ ] Foundation: fonts/defaults #141; profile, tokens and zoom #142.
- [ ] Foundation: symbol icons #143.
- [ ] Terminal: remaining implementation gaps #144–#147.
- [ ] Lite: shell #149, using the merged #148 mock.
- [ ] Lite: Explorer/Search #150 and editor #151.
- [ ] Lite: Git/diff #152 and Markdown/preview #153.
- [ ] Lite: embedded terminal #154, settings #155, common controls #156.
- [ ] Pro: kit/QML foundation #157.
- [ ] Pro: sidebar #158, header/composer #159, messages #160.
- [ ] Pro: activity #161, notices #162, questions/details #163.
- [ ] Pro: overlays #164, worktrees/connections #165, settings #166, Atlas #167.

Behaviour dependencies (#120, #125, #126, #128, #130, #170) must be represented
honestly: preserve real runtime state and actions, and record missing contracts
instead of shipping mock success states. Do not redo already merged #128 work.

## Stack and evidence

| Layer | Branch / base | PR | Implementation | Checks / limits |
| --- | --- | --- | --- | --- |
| Foundation | `feat/kit-zoom-foundation` → `integration` | #207 | Font defaults, kit zoom bridge, terminal/diagram tokens implemented; legacy per-surface adapters continue in later layers | Lite targets pass (14 in suite + 2 corrected expectations rechecked); Pro typography target builds; native/full Pro acceptance deferred |

Every subsequent PR targets the preceding stack branch and names its dependency.
Do not request reviewers until all implementation layers are present. Preserve
unfinished rows across turns. Clean linked worktrees only after their changes
are pushed and after confirming no other conversation/process uses them.

Current layer: `feat/ui-stack-symbols` on `feat/kit-zoom-foundation`; replaces glyph rendering and migration-name adapters with Két Symbols. Symbol renderer checks: 4/4 pass; desktop-support and Atlas library targets compile. Old QML source URLs/resources are retained until the Pro kit cutover.

Symbol layer: PR #208, based on #207. Terminal layer: `feat/ui-stack-terminal`, based on `feat/ui-stack-symbols`; find controls and persistent interface zoom. Packaging/platform expansion stays deferred.

Terminal implementation: kit find field/actions and 75–200% persisted interface zoom; text size stays independent. Lite build and both terminal test targets pass. Native comparison and #147 packaging/platform work are not claimed complete.

Terminal layer: PR #209. Lite shell layer: `feat/ui-stack-lite-shell` on `feat/ui-stack-terminal`; activity rail, first-use welcome, optional editor breadcrumb above the viewport. Lite app and all 16 Lite test targets pass; offscreen startup succeeds. Native acceptance remains deferred.

Lite shell: PR #210 on #209. The next layer replaces pane/find controls, Quick Open with CommandPalette, Markdown mode switching with SegmentedControl, Settings navigation/theme cards/zoom/field rows/footer and core message dialogs with kit composition. It keeps real search, selection, draft/reset and document operations. Further Lite work: model-backed row recipes/Git decorations and mutations, Markdown recipe typography, remaining zoom geometry.
