# 0034: The KetPlus Terminal mock runs the shipped terminal

Status: accepted
Date: 2026-11-06

## Context

KetPlus Terminal (`core/src/terminal_app`) is a standalone native terminal. Its
grid, PTY, search and selection already ship in `core/src/terminal`; only its
window and colours predate Két Design System. The KétPlus Pro mock
(`mockups/qt`, ADR 0033) is Qt Quick with fixtures and cannot host the
QWidget terminal.

## Decision

- **`mockups/terminal` is a separate Qt Widgets mock.** It is its own CMake
  project, independent of `mockups/qt` and of the Lite and Pro builds.
- **It compiles the shipped terminal sources** (`PtyProcess`,
  `TerminalSession`, `TerminalView`, `TerminalTab`) from `core/src` and runs a
  real shell. The mock owns only the window: menus and shortcuts as in
  `TerminalWindow`, tabs and actions from the Két Qt kit, terminal colours from
  Két tokens and KÉT Sans Mono Medium at 14 px.
- **Gaps stay visible.** Where the shipped view cannot be styled from outside
  (font family, ANSI palette), the mock works around or documents it instead of
  patching `core`.

## Consequences

- Terminal behaviour in the mock is the product's; a `core` change shows up in
  the next mock build without porting.
- The mock needs a real shell, so captures include the reviewer's prompt and
  are not committed.

## Alternatives considered

- **Redraw the terminal in QML with fake output.** Repeatable captures, but a
  second terminal implementation that drifts from the shipped one.
- **A Terminal screen inside `mockups/qt`.** Ties the standalone app's design
  to the KétPlus Pro shell.
