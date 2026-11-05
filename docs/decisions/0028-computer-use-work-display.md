# 0028: Computer use on a work display

Status: accepted
Date: 2026-10-19

## Context

Background input (ADR 0027) keeps the user's pointer and front app, but the
granted apps still share the user's display. Their windows sit among the user's
own windows, the overlay pointer and the banner appear over the user's work,
and a screenshot of the main display only shows granted apps where they happen
to be. Many users have a second display. The agent always captured the main
display and could not see a granted window on another one.

## Decision

- **A work display.** The agent works on one display. By default this is the
  first display that is not the main one. With a single display it is the main
  display. Settings → Computer use → Work display chooses "Secondary display"
  (default) or "Main display", stored as `display` (`secondary` or `main`) in
  `computer-tools.json`. The agent reads the file on every call, so a change
  applies at once.
- **Granted apps move there.** After the user allows apps in the approval
  panel, the agent moves their unminimized windows that are not already on the
  work display. Frameless windows count; panels, sheets, system dialogs and
  windows under 160x100 points stay put. It sets the accessibility position and size,
  shrinks a window to the visible frame and cascades several windows, after the
  conversation's granted windows already on the display.
  `open_application` does the same for an app it opens or reveals, and gives a
  launched app up to 3 s to show a window. The approval panel names the
  display when more than one is connected. `request_access` and
  `open_application` report `movedToWorkDisplay`.
- **Screenshots default to it.** `screenshot` captures the work display unless
  its `display` parameter is `main` or a display ID. The result names the
  display, its global origin and its size in points. Coordinates in later
  calls use that screenshot's display. `list_apps` lists the displays and marks
  the work display. `observe` marks elements whose centre is on another display
  as `offscreen`. When no granted app has a window on the display, `screenshot`
  answers `NOTHING_ON_DISPLAY` instead of ScreenCaptureKit's `-3811`.
- **Hidden windows are raised or flagged.** Some apps, Qt among them, stop
  repainting a window that other windows cover, so a capture shows its old
  pixels while accessibility already reports the new state. Before a
  screenshot the agent finds granted windows that other apps' windows hide by
  at least half, using the on-screen window order. On a work display that is
  not the main display it raises them with the accessibility raise action,
  which does not activate the app, and waits 300 ms. On the main display it
  leaves the user's windows in front and lists the app under `covered`, and
  the MCP result says the image may be old. `raised` lists apps it raised.
- **The banner follows.** The control banner shows at the top of the work
  display.
- **The user's windows stay put.** Only granted apps' windows move. The agent
  never moves windows back when access ends; the user decides where they go.

## Consequences

With two displays the user keeps the main display to themself while the agent
works on the other. Windows that refuse to move or resize, full-screen windows
and windows on other Spaces stay where they are; the agent can still capture
another display with `display`. When a display disconnects during a turn, the
next screenshot uses a remaining display. The fixture opens on the
main display, so `scripts/e2e.mjs` checks the move and takes element positions
from `observe` instead of from the fixture.

## Verification (2026-10-19, macOS 26.2, built-in display and a second display)

- `swift test`: 31 tests, including display choice, display arguments,
  frameless windows, window placement and background key characters.
- `scripts/e2e.mjs` passed every check. The work display was the second
  display; the grant and `open_application` moved the fixture there; the
  screenshot reported it; background and foreground input reached the fixture
  on it.
- Driving KétChat QA (a Qt app with a frameless window) first left its window
  on the main display, because only `AXStandardWindow` windows moved, and the
  work-display capture then failed with `-3811`. With frameless windows
  accepted, the grant moved it to the second display, a screenshot showed it
  there, and a background click and typing filtered its conversation list.
  Background Backspace was ignored until key events carried their NSEvent
  characters; `scripts/e2e.mjs` now checks it on the fixture.
- With KétChat QA already on the second display, granting the fixture placed it
  28 points down and right of KétChat instead of exactly on top of it.
- A KétChat QA window under Slack on the second display kept showing old pixels
  in screenshots while `observe` already reported new text, because Qt stops
  painting a hidden window. With hidden windows raised, an ordinary window over
  the whole display no longer hid it: the first screenshot raised KétChat and
  showed its current window, KetPlus stayed the front app, and the next
  screenshot needed no raise. `swift test` has 33 tests, including coverage.
- Not yet verified: a turn started from the desktop app, displays with
  different scale factors, and a display that disconnects during a turn.

## Alternatives considered

- A virtual display created by the agent. It would give every user a work
  display, but it needs private CoreGraphics API and confuses window placement.
- Capturing all displays in one image. Images get large and coordinates mix
  scale factors.
- Leaving windows where they are and only choosing the capture display. The
  agent would then miss granted windows on the other display.
