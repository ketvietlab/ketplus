# Pro UI follow-up — epic #139

The implementation stack reaching `feat/new-ui` is not completion of the epic.
Each issue keeps its implementation and acceptance criteria separate. The
light/dark × 1/1.25 app/mock comparison in #136 is part of the UI work, not a
release-only gate. Plan/harness and release deployment remain outside this work.

## Foundation correction (#157)

- [x] Remove the product `ChromeMetrics.js` and call `KetTheme.px` with kit zoom.
- [x] Replace `TintedIcon` consumers with the public `KetIcon`; keep legacy
  provider icon-name conversion at the data boundary.
- [x] Remove the local `KetToolTip` type and consume the kit tooltip directly.
- [x] Use the shared-clock activity indicator for history loading; the active
  turn indicator follows the kit's reduced inactive clock rate.
- [x] Stop transcript measurement when measurable bodies are exhausted. Skip
  GUI-resource bodies without starving older text; restart on rows/layout changes.
- [x] Stop measurement, task elapsed-time and held-message polling while hidden
  or minimized; stop the bounded row-growth/wheel animations when hidden.
- [x] Focused motion/tooltip checks pass. The complete Agent Workspace, row
  measurement and geometry targets pass together (3/3); Desktop zoom passes
  separately (1/1). The native Cocoa composer fixture also passes.
- [x] Remove the Agent Workspace host palette/type map. QML reads kit tokens,
  font family and component recipe sizes directly; editor mono preferences remain
  separate. HTML/Mermaid rendering follows shared Appearance changes.
- [x] Register bundled font families before loading QML in a fresh process.
- [x] Queue document formatting after theme/zoom bindings settle; keep the code
  frame and inline chips aligned with text. Fix the Qt Markdown table-after-fence
  empty paragraph without deleting intentionally empty cells.
- [x] Fit the product brand image inside a fixed-width sidebar at larger zoom.
- [x] Build the final palette/type and document fixes and run the full Pro suite:
  96/96 targets pass. The fresh-process Cocoa theme fixture also passes.
- [x] Native Cocoa checks cover light/dark × 1/1.25 for this synthetic transcript,
  including font, colour, code-frame geometry, header alignment and logo bounds.
  This is a targeted regression matrix, not every app/mock state.
- [x] Measure #126's foreground/background/no-work CPU cases on the native app:
  6.34% foreground and 1.56% background while running, 0.77%/0.16% with no work,
  0.03% hidden (#283 fixture, 1440×900 at 2×). Targets relaxed to ≤7%/≤2%; the
  kit clock rate is unchanged.
- [ ] Compare native app/mock states at both themes and zooms. A synthetic Cocoa
  render is useful evidence, but does not establish the full matrix.

## Remaining Pro work

- [ ] #159: finish the reviewer picker UI; assignment behavior is owned by #120.
- [ ] #160: match image viewer and Mermaid fullscreen to the mock; compare each provider.
- [ ] #161: use kit activity groups and changed-files components without losing
  virtualization, missing/moved-file handling or context actions. Extend the
  kit's public API first where those behaviors cannot currently be expressed.
- [ ] #161: audit every provider step against the mock's transcript-type table.
- [ ] #163: exercise question/approval/error/running/task/details states against the mock.
- [ ] #164: migrate toasts and compare overlays natively.
- [ ] #165: compare Worktrees/Connections; SSH setup needs its transport implementation.
- [ ] #166: add Agent messaging limits and compare every Settings state.
- [ ] Reconcile #158/#162/#167 acceptance evidence even though their issue states are closed.

Passing tests, publishing a PR or merging into a feature branch does not tick
the remaining implementation or visual-acceptance rows above.

## Verification limits

The first complete Agent Workspace run found a test still asserting the removed
tooltip adapter's properties; it now inspects the kit's actual background and
label. A subsequent run hit the existing steering test's 5-second timeout
(the test reported 5.75 seconds required). That test passed in isolation and the
final complete run passed without a timeout change. Live-provider opt-ins remain
disabled; no provider quota or installed app replacement is involved.

The synthetic Cocoa composer and its menus render successfully. The last
computer-use attempt returned `cgWindowNotFound` for the isolated test app. The theme-authority
follow-up uses native Cocoa fixture captures and geometry assertions; interaction
through computer use and the full app/mock matrix remain unverified. No release,
bundle packaging, device installation or whole-epic completion is claimed.
