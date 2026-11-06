# 0035: Product UI is built from the Két Qt kit

Status: accepted
Date: 2026-11-06

## Context

The Qt mock (`mockups/qt`, ADR 0033) and the Terminal mock (`mockups/terminal`,
ADR 0034) are built from the Két Qt kit (`ketvietlab/ket-qt`): Két Design
System tokens, Két Symbol icons, and Qt Widgets and Qt Quick components. The
shipped apps still draw their own palette and icons in `core/src/ui/Theme.cpp`
and in each app. Bringing KetPlus Terminal, KétPlus and KétPlus Pro to the
mocks (#136–#139) requires the same kit in the product build, without two
copies of the tokens.

The kit requires Qt 6.8. Core required 6.5; the Pro build already requires 6.11.

## Decision

- **Core builds the kit from source.** `core/cmake/Dependencies.cmake` fetches
  `ket-qt` pinned to a commit and links `KetQt::Tokens`, `KetQt::Icons` and
  `KetQt::Widgets` into `ketplus_ui`, so every app that uses core's UI sees
  them. They are linked statically. The kit's tests, examples and install rules
  are off, so `cmake --install` of an app ships no KetQt headers, archives or
  CMake package. Its build directory lives in each build tree, not in the shared
  FetchContent cache.
- **Qt Quick is opt-in.** `KETQT_BUILD_QUICK` is off unless the including
  project sets it first; Pro turns it on when its QML UI lands (#157).
- **Core requires Qt 6.8.** This is the kit's floor and the oldest Qt a Két
  product ships.
- **Tokens and icons come from the kit only.** Colours, spacing, type and icon
  choices move from the hand-written theme to `ket::Tokens`, `ket::Profile` and
  Két Symbol one area at a time, each in its own issue (#141–#143 and the
  per-app tasks). A missing token is added to Két Design System and the kit
  first, then used here; product code does not define its own.
- **Fonts stay with the app.** The kit bundles no fonts. Apps keep registering
  the faces they are licensed to embed (Inter in core, KÉT Sans in Pro).

## Consequences

- Linking the kit changes nothing visible on its own; later changes adopt it
  screen by screen.
- Apps that link `ketplus_ui` also load QtSvg and QtPrintSupport, which the kit's
  icons and Widgets need. KetPlus Terminal deploys both frameworks from now on.
- Builds need Qt 6.8 or newer and network access the first time the kit is
  fetched, like the other pinned dependencies.
- Binary distributions carry the KetQt (MPL 2.0), Két Design System tokens
  (MIT) and Két Symbol (CC BY 4.0) notices.
- A kit change reaches the product only through a new pin.

## Alternatives considered

- **`find_package(KetQt)` from an installed kit.** Depends on what is installed
  on each machine and does not include the QML module yet.
- **Copy the tokens into core.** Two sources that drift apart from Két Design
  System.
- **A Git submodule.** The other pinned dependencies use FetchContent and the
  shared cache; one more submodule adds checkout steps for no gain.
