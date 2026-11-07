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
- [x] Foundation: fonts/defaults #141; profile, tokens and zoom #142 (implementation; native acceptance deferred).
- [ ] Foundation: symbol icons #143.
- [ ] Terminal: remaining implementation gaps #144–#147.
- [x] Lite: shell #149, using the merged #148 mock (including kit status bar).
- [x] Lite: Explorer/Search #150 and editor #151 (implementation; acceptance deferred).
- [x] Lite: Git/diff #152 and Markdown/preview #153 (implementation; acceptance deferred).
- [x] Lite: embedded terminal #154, settings #155, common controls #156 (implementation; acceptance deferred).
- [x] Pro: kit/QML foundation #157 (offline module/shared clock; idle-CPU acceptance deferred).
- [x] Pro: sidebar #158 UI composition (metadata/acceptance limits below).
- [x] Pro: header/composer #159 existing-runtime UI; #120 reviewer dependency remains separate.
- [x] Pro: messages #160 (implementation; native acceptance deferred).
- [x] Pro: activity #161 UI and provider step mapping (native acceptance deferred).
- [x] Pro: notices #162 using available journal/runtime state (native acceptance deferred).
- [ ] Pro: questions/details #163.
- [x] Pro: overlays #164, including the existing revisioned repository/worktree picker.
- [x] Pro: worktrees/connections #165 available-runtime UI (Relay pairing; SSH setup limitation below).
- [x] Pro: settings #166 available-runtime controls (messaging behavior dependency remains separate).
- [x] Pro: Atlas #167 chrome and existing WebView composition (native acceptance deferred).

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

Lite controls: PR #211. The model layer `feat/ui-stack-lite-models` adds virtualized recipe rows/Git decorations, inline creation with collision protection, serialized stage/unstage/commit-index commands, recipe Markdown/diff rendering and shared TerminalTab ownership for embedded multi-tab sessions. Tests cover literal paths, unborn index, untracked-file preservation, inline creation/cancel and independent PTY sessions. Native comparisons remain deferred.

Model layer validation: all 16 Lite targets pass; Source Control rendered offscreen at 224 × 640 with real grouped snapshot data. No full native/theme/zoom acceptance or release work claimed.

Lite models: PR #213 on #211. Pro foundation now links the offline Ket QML module, resolves profile typography without the old +2 bias, retains numeric zoom, draws QML symbols/tooltips with kit components and shares the gated animation clock. Runtime selected QML checks pass; font, zoom and Lite regression evidence is recorded in the PR.

Pro foundation: PR #214 on #213. Pro sidebar layer migrates native and QML navigation/conversations, tag filtering, group collapse/create, update pills and authoritative journal-size warnings. It preserves context actions and blocking/async attention. Tags and awaiting-reply consume supplied metadata; automatic classification and reviewer orchestration remain separate behaviour dependencies. Review/native acceptance is still deferred.

Sidebar validation: 16/16 Lite targets, Pro workspace/attention/zoom targets, three selected QML checks and four local-daemon storage cases pass. A 214 px offscreen capture verifies tag rows and warning layout. Integration exposed a Settings navigation callback holding a deleted QListWidgetItem; persistent model indices now fence that lifecycle, with a focused regression test. No native acceptance/review/release claimed.

Sidebar: PR #215 on #214. Header/composer layer extracts the existing input and menus, uses kit recipes/buttons/search/ring/meters, aligns the placeholder with the caret and keeps one send/stop position. It retains draft, IME, attachment, steering and revisioned model configuration behavior; header tags filter the real sidebar. Thirteen selected Agent Workspace cases pass across the focused runs, plus the desktop zoom target; an offscreen light capture checked the header and empty composer. Reviewer assignment/orchestration (#120) is not claimed implemented. Full native acceptance/review remains deferred.

Transcript layer on #216: recipe typography/bubbles/code, syntax highlighting,
fixed gallery/large generated previews, kit close control and journal day dividers.
Focused checks cover streaming, copy/link behavior, geometry, repeated formatting,
numeric zoom, journal dates and Markdown/Mermaid. No provider quota or release work.

Transcript: PR #218 on #216. Activity layer extracts header and changed-file
composition so the main activity delegate is below 500 lines. Kit row/group/file
recipes preserve on-demand detail loading, command copy/output, path actions,
virtual group expansion and reader anchoring. Explicit tool names map to wait,
monitor, wake, skill, tool search, stop, subagent, plan and web steps; private
reasoning remains hidden under the existing contract. Focused interaction,
light/dark detail, grouping and synthetic mapping checks pass.

Activity: PR #219 on #218. Notices layer shares live/replay projection for provider,
model, execution-context and fork/rename events, and adds one turn-end receipt.
SDK cost is explicitly a cumulative session estimate, omitted when unavailable;
unknown turn states never become success. Steers retain their label, reconnecting
notices do not fail a turn, and existing trimmed-detail receipts remain visible.
Queued state uses the actual session status; no unsupported queue mutation controls
are fabricated. UI checks (14 cases), provider projection and daemon replay pass;
two old last-row assertions now check both preserved final text and one receipt.

Cards layer on #221: kit question/approval cards preserve multi-step answers,
free text, secret input, one-click confirmation and drafts. Scope errors use the
kit banner; background task rows retain real stop/output actions and locally
dismiss completed receipts without deleting daemon history. Focused runtime
checks pass, including dismissal followed by a new running task. Atlas results
and the right details pane remain part of the Atlas layer; #163 is not complete.

Cards: draft PR #222 on #221. Sidebar correction follows: standalone QML now
uses repository → conversation, removing the inherited worktree rail and its
filter/reveal logic. The native shell already groups conversations by repository.
Six focused cases pass, including a 120-worktree catalog without sidebar rows,
group collapse/create, resizing and context actions; the dark capture is inspected.

Sidebar correction: draft PR #224 on #222. Overlays layer composes kit Quick
search and QML context menus, adds the real Change worktree route and desktop
compact-window presentation. The latter reuses the mounted conversation/draft;
provider context-token compaction is a separate existing feature. Spotlight suite:
17 pass, 1 helper-only skip; final popup-specific and editor navigation reruns pass.
Four QML interaction cases and compact geometry/draft checks pass. Picker skinning
continues with #165. No review, release or native acceptance started.

Overlays additional checks: native sidebar composition and closable file details
pass. Narrow compact header hides scope/actions per mock and keeps its expand
button in bounds. The final popup-specific search/editor checks pass.

Overlays: draft PR #225 on #224. Workspaces layer migrates worktree actions,
status badges and create/select inputs to kit components; the paginated execution
picker paints kit tree recipes without replacing its model or acknowledgment
checks. Connections actions/inputs use the kit; palette and mono font resolve
shared tokens/profile. Mock page/hero typography replaces Markdown-heading roles.

Validation: execution picker 2/2 substantive cases pass; worktree listing/creation
2/2 pass; Connections 10 pass including setup/cleanup, with two opt-in Debug demo
cases skipped. Final typography/width cases pass. The implemented Linux connection
flow is authenticated outbound Relay; a new SSH setup/transport is not supplied by
this UI layer. Native acceptance, review, packaging and deployment remain deferred.

Settings layer on #226 uses kit navigation/theme/accent/segmented/form controls,
restores the shared staged Save/Reset footer and Follow system checkbox, preserves
custom numeric zoom, and routes the sidebar storage warning to Storage without
recreating an open draft. Browser/Computer/Providers/Archived/Updates and Claude
credentials retain their behavior; usage popover buttons use the kit. The four
Settings targets pass across focused runs; two Claude usage cases pass. Storage
light/dark offscreen captures expose and then verify removal of duplicate list
text beneath the kit navigation rows. No quota, native acceptance or review run.

Settings: draft PR #227 on #226. Atlas layer replaces legacy library/panel actions,
search and tab controls with kit controls, uses semantic colors/zoom for cards,
shows actual manifest counts and docks AI Chat beside the unchanged WebView.
Back routes to the bound conversation; hide/show preserves its state. Atlas suite
passes; workspace cases pass across the initial run and corrected token-color
assertions. Native WebView smoke remains explicitly skipped. Inspected library
and QWidget chrome captures (the latter does not capture embedded QQuick pixels).

Remaining #163 contract gaps: a transcript Atlas-result receipt tied to its
producing conversation, and held-message release UI (#130), have no supplied
runtime projection/action in this baseline. Existing Atlas catalog/detail actions
are real; the stack does not fabricate validated-result or release-success rows.

Foundation cleanup layer on #229 removes Inter fetch/resources and unused product
Lucide SVGs/provider/motion wrapper, centralizes remaining semantic colors, and
finishes Lite's kit status bar. Legacy icon names remain lookup adapters only.
The global QWidget font rule is gone; application/QSS updates precede kit
observers so theme/zoom cannot reset recipe title fonts. A regression exercises
light/dark, preview/cancel and 14→21→14 zoom. Capture/benchmark mode uses temporary
preferences. Focused builds/tests and the corrected Lite capture are recorded in
the PR; native acceptance and installed-bundle checks remain deferred.

Foundation cleanup verification: Lite 16/16 test targets and five focused Pro
targets pass (Atlas workspace, desktop zoom, font contract, symbols, Settings
integration). The native WebView test stays opt-in/skipped.
