# Két UI implementation stack

## Execution agreement — 2026-11-07

Implement phases 1–5 of the manual UI plan as stacked **draft** pull requests.
Finish the stack before requesting review. Do not merge or begin the review
phase. The current native Qt mock is the approved design reference; no additional
mock-approval checkpoint is required. Check a delivery row only after its code
is committed, pushed and attached to a PR. A checked row means scoped code
implementation, not final acceptance or release.

Excluded by the user: phase 6 (conversation Plan / daemon continuation, #198),
phase 7 (overall acceptance, review, release packaging, deployment and device
installation), and Repo Memory implementation. The Plan mock remains in #197.

Baseline: public `ketplus/integration`, commit `698f0510`, the two-edition source
layout from ADR 0029. `core/` builds independently of Pro. Shared components are
consumed from ket-qt, pinned to `d47638214f80695524cd2724996674da7afdbb6a`.
The native references are `mockups/qt` and `mockups/terminal` (ADR 0033–34).

## Manual delivery checklist

- [x] Phase 1: reconcile repository ownership, baseline and existing merged work.
- [x] Phase 2: #141 fonts/defaults; #142 shared profile/tokens/zoom; #143 symbol
  rendering and approved app-icon formats. Remaining legacy palette/QSS objects
  are projections of kit tokens, not independently defined palettes.
- [x] Phase 3: Terminal #144–146 UI and #147 name/font/icon source assets.
  Windows/Linux standalone Terminal packaging and installation are phase 7.
- [x] Phase 4: Lite #149–156 shell/status bar, Explorer/Search, editor,
  Git/diff, Markdown/preview, embedded terminals, settings and common controls.
- [x] Phase 5: #157 Pro kit/QML foundation and shared clock; #158 sidebar.
- [x] Phase 5: #159 header/composer with existing runtime; #160 messages;
  #161 activity; #162 journal-derived notices.
- [x] Phase 5: #163 question/approval/task/scope/running/error cards using the
  existing runtime and preserving real IDs, drafts and actions.
- [x] Phase 5: #163 / #130 held-message release — local authenticated operation,
  durable batch authorization/claim recovery and the explicit transcript action.
- [x] Phase 5: #163 explicit Atlas-result transcript receipts — #238 reads real
  manifest metadata, reuses Edit/Open actions and fences unavailable sources.
- [x] Phase 5: #164 overlays; #165 Worktrees/Connections available-runtime UI;
  #166 Settings available-runtime controls; #167 Atlas chrome/composition.
- [ ] Phase 6: conversation Plan/harness — explicitly excluded, not started.
- [ ] Phase 7: aggregate acceptance/review/release/deployment — explicitly
  excluded, not started. Focused patch tests below are not this phase.

## Draft stack

Every PR targets the branch immediately before it. No reviewer was requested.
The base/head/draft chain is checked against GitHub at final publication.

| PR | Layer | Head | Base |
| --- | --- | --- | --- |
| [207](https://github.com/ketvietlab/ketplus/pull/207) | Fonts / zoom | `feat/kit-zoom-foundation` | `integration` |
| [208](https://github.com/ketvietlab/ketplus/pull/208) | Symbol renderer | `feat/ui-stack-symbols` | `feat/kit-zoom-foundation` |
| [209](https://github.com/ketvietlab/ketplus/pull/209) | Terminal UI | `feat/ui-stack-terminal` | `feat/ui-stack-symbols` |
| [210](https://github.com/ketvietlab/ketplus/pull/210) | Lite shell | `feat/ui-stack-lite-shell` | `feat/ui-stack-terminal` |
| [211](https://github.com/ketvietlab/ketplus/pull/211) | Lite controls | `feat/ui-stack-lite-panes` | `feat/ui-stack-lite-shell` |
| [213](https://github.com/ketvietlab/ketplus/pull/213) | Lite models / Git / preview | `feat/ui-stack-lite-models` | `feat/ui-stack-lite-panes` |
| [214](https://github.com/ketvietlab/ketplus/pull/214) | Pro QML foundation | `feat/ui-stack-pro-foundation` | `feat/ui-stack-lite-models` |
| [215](https://github.com/ketvietlab/ketplus/pull/215) | Pro sidebar | `feat/ui-stack-pro-sidebar` | `feat/ui-stack-pro-foundation` |
| [216](https://github.com/ketvietlab/ketplus/pull/216) | Header / composer | `feat/ui-stack-pro-composer` | `feat/ui-stack-pro-sidebar` |
| [218](https://github.com/ketvietlab/ketplus/pull/218) | Transcript | `feat/ui-stack-pro-transcript` | `feat/ui-stack-pro-composer` |
| [219](https://github.com/ketvietlab/ketplus/pull/219) | Activity | `feat/ui-stack-pro-activity` | `feat/ui-stack-pro-transcript` |
| [221](https://github.com/ketvietlab/ketplus/pull/221) | Runtime notices | `feat/ui-stack-pro-notices` | `feat/ui-stack-pro-activity` |
| [222](https://github.com/ketvietlab/ketplus/pull/222) | Questions / approvals / tasks | `feat/ui-stack-pro-cards` | `feat/ui-stack-pro-notices` |
| [224](https://github.com/ketvietlab/ketplus/pull/224) | Remove sidebar worktree rail | `feat/ui-stack-sidebar-cleanup` | `feat/ui-stack-pro-cards` |
| [225](https://github.com/ketvietlab/ketplus/pull/225) | Overlays / compact window | `feat/ui-stack-pro-overlays` | `feat/ui-stack-sidebar-cleanup` |
| [226](https://github.com/ketvietlab/ketplus/pull/226) | Worktrees / Connections | `feat/ui-stack-pro-workspaces` | `feat/ui-stack-pro-overlays` |
| [227](https://github.com/ketvietlab/ketplus/pull/227) | Settings | `feat/ui-stack-pro-settings` | `feat/ui-stack-pro-workspaces` |
| [229](https://github.com/ketvietlab/ketplus/pull/229) | Atlas workspace | `feat/ui-stack-pro-atlas` | `feat/ui-stack-pro-settings` |
| [231](https://github.com/ketvietlab/ketplus/pull/231) | Shared font / theme cleanup | `feat/ui-stack-foundation-cleanup` | `feat/ui-stack-pro-atlas` |
| [232](https://github.com/ketvietlab/ketplus/pull/232) | Application icons | `feat/ui-stack-brand-assets` | `feat/ui-stack-foundation-cleanup` |
| [233](https://github.com/ketvietlab/ketplus/pull/233) | Provider operation aliases / checklist | `feat/ui-stack-completion-ledger` | `feat/ui-stack-brand-assets` |
| [236](https://github.com/ketvietlab/ketplus/pull/236) | Explicit held-message release | `feat/ui-stack-held-messages` | `feat/ui-stack-completion-ledger` |
| [238](https://github.com/ketvietlab/ketplus/pull/238) | Atlas result receipts | `feat/ui-stack-atlas-receipts` | `feat/ui-stack-held-messages` |
| This layer | UI / history regression fixtures | `feat/ui-stack-regression-fixtures` | `feat/ui-stack-atlas-receipts` |

The activity-alias layer renders `ScheduleWakeup` as a scheduled wake-up and
`EnterPlanMode` as entering plan mode, with running/completed labels. This is
provider-event presentation; it does not create or enforce conversation plans.

## Technical evidence

| Area | Verified evidence | Limits |
| --- | --- | --- |
| Foundation / Lite | All 16 Lite test targets pass after the final core changes and again after the icon exports. EmptyState regression covers title size/weight under light/dark, same-theme preview/cancel and 14→21→14 zoom. | No cross-platform native acceptance. |
| Shared Pro theme | Five targets pass: Atlas workspace, desktop zoom, font contract, symbols, Settings integration. Actual shown-tooltip font is checked after zoom/theme changes. | Native WebView smoke remains opt-in/skipped. |
| Lite models | Tests cover literal Git paths, unborn index, preservation of untracked files, creation/cancel and independent PTY sessions. Real grouped Source Control rendered offscreen. | No release/platform qualification. |
| Sidebar | Focused checks cover 120 worktrees without sidebar rows, repository→conversation grouping, collapse/create, resizing, context actions and attention. Native and standalone QML use the same hierarchy. | Automatic tags and reviewer assignment need their behavior contracts. |
| Composer / transcript | Focused tests cover IME/drafts, steering, model choice, streaming, links/copy, Markdown/code, images, journal dates and width/zoom. | No live provider quota used. |
| Activity / notices / cards | Synthetic journal/runtime tests cover tool mapping, errors, live/replay receipts, multi-step/secret answers, duplicate-submit guards, task dismissal and a later running task. Final six selected Agent Workspace cases pass (eight including setup/cleanup). | A receipt is not fabricated when runtime evidence is missing. |
| Overlays | Spotlight 17 pass with one helper-only skip; corrected popup and navigation checks pass. Compact-window geometry/draft and closable file details checks pass. | Desktop compact window is distinct from provider token compaction. |
| Workspaces / Settings | Execution picker and worktree cases pass; Connections 10 pass with two opt-in Debug demo skips; four Settings targets and two usage cases pass across focused runs. | Relay pairing is implemented; this does not add SSH transport/setup. |
| Atlas | Atlas suite and available workspace cases pass; library and QWidget chrome captures inspected. Back/hide/show keep the bound conversation/draft. | QWidget capture excludes embedded QQuick pixels; native WebView remains unverified. |
| Application assets | Lite and Pro desktop builds pass. Approved SVGs match byte-for-byte; PNG inspected; six ICO dimensions and ICNS container lengths validated; exporter shell syntax passes. | No installed Dock/Finder/Windows/Linux verification. |

Offscreen capture mode uses a temporary preference store. Captures inspected
include Lite welcome/status bar, Pro sidebar, Settings light/dark, and Atlas
library/chrome. No installed application or daemon was replaced/restarted; no
browser session or paid-provider test was used. `git diff --check` passes.

## Open runtime dependencies — not completed by UI composition

| Remaining item | Evidence / constraint | Next work and owner |
| --- | --- | --- |
| #159 / #120 reviewer assignment | Existing composer/actions are preserved; an assignment/orchestration service is not supplied by this layer. | #120 implementation owns assignment/state; follow-up UI consumes it. |
| #158 metadata / #170 | Tags and attention consume supplied metadata; automatic classification is not claimed. | Behavior issue supplies authoritative classification updates. |
| #165 SSH setup | Existing Linux flow is authenticated outbound Relay. | A separate transport/setup implementation must precede SSH-specific UI behavior. |

These rows keep the remaining scope visible; the entire UI epic is **not** marked
complete and no issue is auto-closed. Existing merged #128 storage behavior was
reused. Idle CPU qualification (#126), full native side-by-side comparisons,
platform font checks, no-Homebrew bundle checks and installed icons belong to
the explicitly deferred acceptance/packaging phase.

## Handoff and worktree cleanup

Use the last draft PR's head to examine the complete stack; individual PRs have
focused descriptions and validation. Review/merge has not started. After the last
branch is pushed, remove the implementation worktree only if it is clean, has no
unpushed commits and no other conversation/process uses it. Never remove the
repository's main checkout. Any new changes after cleanup use a new worktree.


Held-message release verification: the entire fake-provider agent-messaging suite
passes, including all guard reasons, authorization replay, whole-batch rollback,
busy/urgent receivers, expiration/deletion/generation fences and recovery on both
sides of the journaled input. Provider transcript suite passes. Focused Desktop
live-notice/capability/bridge and three QML action/layout cases pass. Pro desktop
and daemon targets build; an offscreen action capture is inspected. Agent-token
IPC and cloud-control cannot grant the release. No installed daemon was updated.

Atlas receipt verification: completed-only Markdown links are extracted outside
code/raw HTML, capped at eight links / 128 K characters, and placed once at the
end of a chunked answer. Manifest reads are capped at 4 MiB. Tests cover metadata,
canonical roots/symlinks, real builder routing, missing CLI feedback, actual QML
actions and deletion/conversation/worktree changes. The dark card render was
inspected. This attributes an explicit link, not creation or preview validation.

The final regression-fixture layer updates toolkit-neutral button lookup, the
actual Markdown segmented control, landscape image reservation, streaming
typography invariance and changed-file hit targets. History paging now counts
user + assistant + turn receipt, checks receipt ordering/unique keys and selects
the latest assistant response independently of the trailing completion notice.
No production behavior is changed by this fixture layer.

Final regression result: the complete Pro build passes. All 91 Pro CTest targets
are verified across the full run (87 passed) and reruns of the four corrected
fixture targets (all passed); the history-window target reports 31 cases passed.
The latest Lite run remains 16/16 after the final core/icon changes. Live-provider
flags were explicitly removed for the Pro run. Native WebView/live-provider and
other opt-in cases remain skipped; this is not phase-7 platform acceptance.
