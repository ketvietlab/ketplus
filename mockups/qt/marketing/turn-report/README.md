# End-of-turn reports

## English marketing copy

**Headline:** Know what’s done. Know what’s next.

**Subhead:** End each turn with a clear view of completed work, supporting evidence, blockers, and the next action—with an owner.

**Feature caption:** A finished turn doesn’t mean a finished project. KétPlus keeps open work visible, so you can pick up with context.

**Short social caption:** Tests passed. Package built. Release blocked. See the difference at a glance—with the evidence and the person responsible for what happens next.

**Alt text:** KétPlus Agent Workspace showing an end-of-turn release report. Code review and regression tests are complete, the package is built, release signing is blocked by missing key access, and device checks are waiting. Each open task names its next action and owner.

## Fixture and scope

The editable scene is `workspace/turn-report` in the native Qt mock (ADR 0033). It reuses the existing `TranscriptBlock` final-answer renderer and Két `KetMarkdown`; this is not a new dashboard or a claim that structured task tracking has been implemented. The report is an agent response shaped by response preferences. No provider, signing service or deployment is called.

The release task, conversation number, branch, test count and timings are fictional sample data. “Turn complete” describes the conversation turn; the table explicitly leaves signing and device verification open. All sample text is English. Captures demonstrate the desktop mock, not iPhone installation or parity with the mobile client.

## Reproduce

Build the standalone mock as described in ../../README.md. From the repository root, using its normal build path:

```sh
build/mock/KetPlusMock.app/Contents/MacOS/KetPlusMock --screen=workspace --state=turn-report --light --screenshot=mockups/qt/marketing/turn-report/desktop-light.png
build/mock/KetPlusMock.app/Contents/MacOS/KetPlusMock --screen=workspace --state=turn-report --dark --screenshot=mockups/qt/marketing/turn-report/desktop-dark.png
```

The captures are unretouched native window output. The explanatory headline and captions above are supplied separately for marketing layouts.

## Verification

- Standalone Qt mock build succeeded with the repository-pinned Két Qt dependency.
- Offscreen smoke: 47 screens, 0 QML problems (includes `workspace/turn-report`).
- Native macOS light/dark captures inspected: all five task rows, evidence, owners and the next-step paragraph are visible; no clipped table cells.
- No production code changed. The fixture’s 48 regression tests are illustrative, not the validation result for this change.
- No mobile-device, packaging or deployment verification is claimed.
