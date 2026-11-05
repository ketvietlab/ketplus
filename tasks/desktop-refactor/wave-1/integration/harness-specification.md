# Harness Specification

## Locked Reference Environment

- Platform: macOS arm64.
- Qt: 6.11.1 unless an explicit decision updates the reference.
- Renderer: Qt Quick Metal.
- Content rect: 1440 x 900 logical pixels, excluding titlebar/shadow.
- DPR: 2, producing 2880 x 1800 physical pixels.
- Web reference: 100% zoom.
- Typography: product scale 115%.
- Themes: light and dark. The observed machine theme during baseline was Dark, but both themes require separate evidence.
- Fonts: pinned Inter web assets for UI reference; native source/editor monospace recorded per run.

## Visual Harness

Inputs:

- Merged design artifact and locked hashes from [baseline-manifest.json](baseline-manifest.json).
- Synthetic fixture data only.
- Approved reference capture or manual designer/user review.

Outputs per state:

- Reference, native, diff, and overlay images.
- Computed geometry and color reports.
- Accessibility/focus metadata for interactive states.
- JSON evidence ledger with hashes and reviewer outcome.

Required packs:

- 112 state pack at 1440 x 900 DPR 2.
- Light/dark counterparts where the design has only one theme URL.
- Responsive pack at 1024 x 768 and 1920 x 1080 for each screen and each dialog/pane/layout family.
- Typography pack for default 115%, supported min/max settings, Vietnamese glyphs, and long strings.
- Interaction pack for keyboard traversal, dialog focus, IME, selection/copy, wheel/pinch, reduced motion, resize, font change, and theme change during streaming.

Rules:

- No global similarity percentage can pass a state with unexplained wrong copy, missing controls, incorrect wrap, clipping, wrong icon, lost focus ring, or wrong state.
- No golden regeneration in the same patch that causes a mismatch unless reviewed as a design change.
- Tolerance and masks must be local, reviewed, and justified. Existing chat reading-region tolerance does not apply to the redesign fidelity gate.
- Missing capture permission means the visual gate is blocked.

## Functional Harness

Functional scenarios must use synthetic repositories, fake providers, fake clocks, ephemeral daemon state, and throwaway databases.

Coverage:

- Agent journey: send, ack, running, stop, steer failure, questions, expired answers, reconnect, missing folder, and scoped switch.
- Code review: editor, preview, changes/staged, terminal, Git refresh, and no hidden mutation.
- Worktree journey: validation, pending, success/error, missing/recovered, and exact repository identity.
- Atlas journey: new bundle, generating while valid preview exists, invalid candidate retaining old preview, missing CLI, and flow tagging.
- Device journey: all 20 connection states, sign-in errors, QR close, claim, approve, cancel, revoke, expiry, and epoch fencing.
- History journey: initial 3 turns, continuation, older, live overlap, Latest, giant turn, and unavailable ranges.
- Archive/delete journey: parent-child archive, Settings restore, typed delete, dirty/main/active guards, crash/retry, and no resurrection.

## Contract Harness

Required before consumers implement:

- OpenAPI/schema diff and IPC schema examples.
- Shared positive/negative fixtures with hashes.
- Wrong owner/workspace/machine/session/generation/device cases.
- Duplicate/conflicting retry cases.
- 202 to final lifecycle cases.
- Oversized item, expired continuation, page/live race, stale generation, missing folder, migration crash, TTL replay, and tombstone older than content TTL cases.

## Performance Harness

Release build only. Same hardware, power/thermal mode, fixture, and toolchain across baseline/candidate. Report raw runs, median, max, and p95/p99 only when sample size supports it.

Target scenarios:

- Cold open/restore with 20 conversations.
- 100 warm switches across 20 conversations.
- 1k and 10k item scroll plus 50 synthetic deltas/s.
- Giant turn over 500 events and at least 8 MiB source.
- Prepend/reflow/Latest while reading.
- Memory plateau after repeated load/evict batches.
- Backend 100k encrypted event query and retention cleanup.
- Native iOS parity later; simulator is functional only.

Measurements:

- App, daemon, and WebView process memory.
- GUI stalls and frame timing.
- Delegate/layout/document counts.
- Parse/decode counts and durations.
- Page bytes/latency and DB rows examined.
- Cache budget counters.

Provider quota must not be used for performance measurement.
