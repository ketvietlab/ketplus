# UI stack self-review

## Scope and review basis

The user authorized finishing defects and self-reviewing the existing stack, with
**no merges**. All 24 PRs remain draft, with their original base/head branches.
Review corrections are appended to #239; the earlier branch heads are not
rewritten. An earlier PR containing a finding is therefore not independently
cleared merely because the complete stack has its correction.

This record combines inspection of the individual layers with regression tests
on the cumulative head. It is **not 24 separately built or visually accepted
revisions**. #207 was also tested at its original head during the initial review.
Live provider calls, packaging, installation and release are excluded. Plan /
harness #198, Repo Memory, reviewer orchestration #120, automatic tags #170 and
SSH setup remain outside this implementation scope.

## Findings and corrections

| Finding | Affected layer | Disposition in #239 |
| --- | --- | --- |
| F1: the accented default family silently resolves to Menlo rather than the bundled code font | #207 | Use the registered `KET Sans Mono` family, normalize only the old default alias, preserve custom families, and check the actual QRawFont family and Medium style. |
| F2: application font/QSS propagation overwrites kit label recipes after zoom | #207 | Already corrected by #231. Rechecked with the EmptyState title size/weight regression and the desktop zoom suite. |
| F3: typography uses raw Qt inputs; theme previews are embedded in an independently styled list | #211, #227 | Compose actual Két Select/ThemeCard controls; separate Appearance and Typography; remove the empty General page and unavailable importer placeholder. Preserve the supported numeric range, staged changes, reset and cancel. |
| F4: Pro typography preview is static sample text | #227 | Reuse the shared live preview. A regression changes font size, checks the preview's layout response and resets it. |
| F5: deferred Settings navigation can override a later selection | #215, #227 | Remove both deferred row resets. The regression visits Storage, moves to Typography, drains events and checks that Typography remains selected. |
| F6: two visual/regression fixtures do not test the intended output | #239 review evidence | Synchronize the composer fixture's kit theme with its dark host palette. For Mermaid failure recovery, check rendered plain source and copied original Markdown rather than Qt's reserialized styled Markdown. |

The intermediate font-list recursion introduced while replacing controls was
caught by the Lite suite and fixed before publication. No intermediate failing
implementation was pushed.

## Layer audit

“Checked” below describes scoped code and deterministic regression review. It
does not mark the native visual acceptance gate complete.

| PR | Review focus and corresponding evidence |
| --- | --- |
| #207 | Font resolution, persisted custom values, zoom ordering, tooltip and kit-label fonts. F1/F2 above. Lite settings, editor/diff, interface-font and desktop-zoom tests. |
| #208 | Symbol name resolution, unknown names, DPR and optical sizes; later QML migration uses kit icons. Symbol icon tests. |
| #209 | Terminal text versus chrome zoom, search geometry and persisted zoom actions. Lite Terminal app and terminal tests. |
| #210 | Welcome/open-folder transition, action wiring and breadcrumb viewport reservation. Lite shell/editor and Pro editor-feature tests. |
| #211 | Settings control ownership, picker bindings and shared input composition. F3 above; Lite settings/extension tests. |
| #213 | Literal Git paths and mutation guards, NewOnly file creation, virtualized file rows, independent terminal sessions and preview state. Full Lite suite. |
| #214 | Kit bootstrap, profile font roles, QML icon adapter and clock acquire/release lifecycle. Font/zoom and Agent Workspace rendering tests. |
| #215 | Repository/conversation identity, storage callback generations/timeouts and sidebar selection. Workspace, attention, shell and daemon-client tests; F5 regression. |
| #216 | Composer drafts, attachment interactions, submission/steering, model and permission selection. Agent Workspace tests and a matched dark-theme Qt render. |
| #218 | Transcript source/copy behavior, syntax block state, image composition and journal dates. Agent, Markdown, Mermaid and provider-transcript tests. |
| #219 | Activity subtype mapping, plain command titles, changed-file actions and recovery state. Transcript interaction/group tests. |
| #221 | Journal-derived event/turn notices, unknown-status handling and cumulative cost wording. Notice and provider-transcript tests. |
| #222 | Stable answer option IDs, secret answer masking, pending-request removal, closed questions and background task state. Agent interaction tests. |
| #224 | No worktree rail in the repository → conversation sidebar. Native composition and standalone QML shell assertions. |
| #225 | Spotlight activation revalidation, unavailable entries, popup lifecycle and compact-window geometry/draft reuse. Spotlight, shell and compact-window tests. |
| #226 | Worktree picker identity/availability and existing Relay connections. Worktree-picker, execution-worktree and remote-workspace tests; no SSH claim. |
| #227 | Shared staged settings, non-preset numeric values, theme cancellation, typography and preview behavior. F3/F4/F5; Settings integration tests. |
| #229 | Atlas chat dock, existing open/edit actions and unavailable viewer state. Atlas and Atlas workspace tests; native WebView remains opt-in. |
| #231 | Font resources, one-time stylesheet scaling and notification order. F2; font, zoom and Lite regressions. |
| #232 | Resource paths and approved icon exports. Both edition builds; installed bundle icons are not verified. |
| #233 | Provider operation aliases and implementation/acceptance distinction in the checklist. Activity tests; no conversation-plan enforcement claim. |
| #236 | Authenticated local operation, session/generation ownership, atomic batch authorization, expiry, durable claim/recovery and duplicate UI guard. Full fake-provider agent-messaging and daemon-client tests. |
| #238 | Bounded local manifest parsing, canonical worktree containment, unavailable state and click-time revalidation. Atlas result, manifest and workspace tests. |
| #239 | Regression fixture expectations and all corrections above. No merge or approval is implied by this self-review. |

## Verification and limits

- Complete Lite build passed; **16/16 CTest targets passed** (7.37 seconds).
- Complete Pro build passed; **91/91 CTest targets passed in one final run**
  (129.84 seconds), after rebuilding the review corrections.
- Focused Settings light/dark capture and composer capture cases passed.
- Changed C++ files pass `clang-format --dry-run --Werror`; `git diff --check` passes.
- Provider-quota flags were unset. Native WebView, real mmdc and other opt-in
  cases retain their skips; passing target totals do not mean those cases ran.

Tests used `QT_QPA_PLATFORM=offscreen` and disabled daemon autostart. The final
Settings checks exercise keyboard selection, custom sizes, font resolution,
preview/reset, navigation ordering and cross-window appearance propagation.

Qt-rendered evidence includes the Lite Appearance/Typography dialog at 920×650,
the Pro Typography workspace at 1100×720 in light/dark, and the standalone
conversation/composer fixture at 1200×900. These are production Qt widgets/QML
with isolated test preferences and synthetic content, not HTML replacements or
live customer sessions.

**Native acceptance remains open:** computer use identifies the isolated review
application but fails to retrieve its window with `cgWindowNotFound`. Offscreen
renders and automated keyboard tests do not establish native popup, macOS focus,
Retina composition or native WebView parity. Re-run those checks when native
window access works. No installed application or running user daemon was replaced.
