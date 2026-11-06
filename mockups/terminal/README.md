# KétPlus Terminal · Qt mock

KetPlus Terminal restyled with the
[Két Qt kit](https://github.com/ketvietlab/ket-qt) Widgets
([ADR 0034](../../docs/decisions/0034-terminal-mock.md)). Unlike `mockups/qt`
it runs a real shell: it compiles the shipped terminal from `core/src`
(`PtyProcess`, `TerminalSession`, `TerminalView` and `TerminalTab`) and only
the window around it belongs to the mock. It is independent of the KétPlus
desktop mock and of the Lite and Pro builds.

What the mock owns:

- the window: the menus and shortcuts of `core/src/terminal_app/TerminalWindow`,
  document tabs drawn by `ket::Style`, and the embedded panel's New tab, Find,
  Clear scrollback and Restart actions on the tab strip;
- colours: the terminal roles come from Két Design System tokens (`text-main`
  on `page-bg`, `accent` cursor, `accent-muted` selection, `warning` matches);
- type: KÉT Sans Mono Medium at 14 px on a 22 px line (the app's actual size),
  KÉT Sans for the chrome; status messages are Két toasts.
- the app icon: `assets/ketplus-terminal.svg` from Két Symbol (CC BY 4.0,
  licence alongside; credit "Két Symbol © 2026 Két Việt, CC BY 4.0"), the
  window and About icon. The same icon is meant for Lampion OS.

## Build and run

Qt 6.8 or later with Widgets and Svg.

```sh
cmake -S mockups/terminal -B build/terminal-mock -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build/terminal-mock
build/terminal-mock/KetPlusTerminalMock.app/Contents/MacOS/KetPlusTerminalMock
```

The kit and libvterm are pinned by commit in `CMakeLists.txt` (libvterm at the
commit `core/cmake/Dependencies.cmake` uses). `-DFETCHCONTENT_SOURCE_DIR_KETQT`
points at a local kit checkout; `-DKETPLUS_MOCK_KET_SANS_ARCHIVE` at an offline
KÉT Sans archive. The KÉT Sans Mono TTF is shared with `mockups/qt/fonts`.

## Evidence

```sh
KetPlusTerminalMock --demo --dark --screenshot=dark.png         # sample output, two tabs
KetPlusTerminalMock --demo --light --find=Passed --screenshot=find.png
KetPlusTerminalMock --cwd=<folder> --zoom=1.25
```

`--demo` runs `demo/demo.sh` (ANSI colours, a Git summary and a test run) in
the first tab. The prompt and tab titles come from your own shell, so captures
show your user and host name: do not commit them.

## Known gaps in the shipped terminal

- The 16 ANSI colours are libvterm's defaults (`TerminalSession` only takes the
  default foreground and background), so dark blue is hard to read on the dark
  theme. A token-based ANSI palette needs a `core` change.
