# 0027: Background computer use

Status: accepted
Date: 2026-10-19

## Context

The first `KetPlus Computer` release (ADR 0026) posts HID events. It moves the
user's pointer, types into the frontmost app, and can only click a window that
is on top at the point. The user cannot work while the agent acts. A system
banner over the target makes a click fail. Codex computer use works in the
background with its own pointer. Its helper reads the accessibility tree and
uses `AXUIElementPerformAction` and `AXUIElementSetAttributeValue`. It sends
events to one process and draws an overlay pointer. It also loads further
functions with `dlsym`, presumably to fake focus, and those were not
identified.

A spike in the agent app (macOS 26.2) measured, with the target covered by
another process's window and Finder frontmost:

- Keyboard events sent with `CGEvent.postToPid` reached the target's focused
  field. The pointer did not move and the frontmost app did not change.
- Mouse clicks sent with `postToPid` did not press a Cocoa button in the
  inactive window, even after setting `AXMain` on the window.
- `AXPress` pressed it, and `AXValue` could be set.
- A ScreenCaptureKit display filter limited to the target app showed the
  covered window in full. A `desktopIndependentWindow` filter failed with
  `-3811` in three of three runs.
- Command shortcuts sent with `postToPid` and the target's Select All menu item
  had no effect. Responder-chain actions need a key window, which an inactive
  app does not have.

## Decision

- **Background is the default.** Input never moves the user's pointer, takes
  keyboard focus or changes the frontmost app unless the model asks for the
  real pointer with `"foreground": true`.
- **Accessibility first.** `observe` returns a granted app's accessibility
  tree, even behind other windows. Each element has a reference, its actions
  and its frame in screenshot pixels. `act` presses, focuses, selects, opens
  the menu of, sets the value of, increments, decrements, confirms, cancels,
  raises, picks or scrolls to an element. As in `browser.v0`, a newer `observe`
  or any input makes earlier references stale (`STALE_SNAPSHOT`). The agent
  sets `AXManualAccessibility` so that Chromium and Electron apps build their
  tree.
- **Coordinate tools map to accessibility.** `left_click` focuses a text
  input, presses an element with `AXPress` or selects a row or cell, taking the
  deepest element at the point in the target app's own tree. `right_click`
  opens the nearest `AXShowMenu`. `scroll` moves the scroll bar of the scroll
  area there. The target is the frontmost granted app with a window at the
  point. Windows of other apps on top are ignored, because the screenshot does
  not show them either. Without a suitable element the call fails with
  `NEEDS_FOREGROUND`. Double, triple and middle clicks, drags, pointer moves and
  clicks with modifiers always need the real pointer.
- **Keyboard goes to one process.** `type` and `key` use `postToPid` with a
  private event source. They go to the `app` parameter, else the app the
  conversation last acted on, else the frontmost app. A shortcut with Command,
  Control or Option first runs the matching enabled menu bar item. Select-all
  in a focused text input sets the selected range. Typing is refused when the
  target's focused element is a secure text field. Secure input in another app
  no longer blocks background typing. Foreground typing keeps the global check.
- **Visible but not intrusive.** An overlay pointer drawn by the agent glides
  to each background action. The control banner has a Stop button. Esc stops
  the agent only while a granted app is frontmost or within 15 s of
  real-pointer input. Otherwise Esc belongs to the user's own app.
  `open_application` no longer brings the app forward unless `foreground` is
  set.
- **Policy is unchanged.** Grants, tiers, the lease, remote-turn refusal and the
  audit log apply to background and foreground input alike. `set_value` counts
  as typing, so click-only apps refuse it. The audit records the mode and never
  the text.
- **No private window-server API.** The agent does not fake focus. Actions that
  need a key window may still need the foreground.

## Consequences

The user keeps working while the agent drives granted apps on the current
Space, even behind other windows. Minimized windows and windows on other Spaces
are neither captured nor reachable. Apps
with poor accessibility (canvases, games, some custom controls) need
`foreground`. A pressed element may differ from a pixel click, for example in a
custom view that reacts to the click position. The model is told to verify
with a screenshot or a new `observe`.

`scripts/e2e.mjs` covers the background path with the fixture covered by a
bundle-less window (`fixture-cover`) and Finder frontmost, then repeats the
foreground path. With `--manual-esc` it also asks for a real Esc and a click on
Stop.

## Alternatives considered

- Private SkyLight focus calls, as Codex appears to use. They would let more
  shortcuts and clicks work in inactive apps, but they are undocumented and can
  break with any macOS update.
- Mouse events sent with `postToPid`. Cocoa controls in inactive windows
  ignored them in the spike.
- Keeping foreground-only input. It is simpler, but the user cannot work in
  parallel and anything on top of the target blocks the agent.
