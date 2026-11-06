# 0033: The Qt mock replaces the KetAtlas design mock

Status: accepted
Date: 2026-11-06

## Context

KétPlus Pro's design reference was `ketplus.ketatlas`, a static HTML and
JavaScript bundle with its own CSS copy of Két Design System. Native work then
re-created each screen in Qt, and the two drifted: metrics, fonts and states had
to be matched by hand and compared with screenshots. The Két Qt kit
(`ketvietlab/ket-qt`) now has a Qt Widgets and a Qt Quick twin for every
component in that mock, with KetPlus metrics in `ket::Profile`.

## Decision

- **`mockups/qt` is the design reference.** It is an interactive Qt Quick app
  built from the kit's `Ket` QML module, with deterministic fixtures and no
  daemon, Git or provider access. New design work changes this mock first.
- **`ketplus.ketatlas` is frozen.** It stays in the repository as the record of
  earlier decisions and its evidence, but it is not updated. The Atlas product
  feature (opening `*.ketatlas` bundles) is unaffected.
- **Standalone build.** The mock is its own CMake project, not part of the Lite
  or Pro build. It pins the kit by commit through FetchContent, fetches KÉT
  Sans 1.1 from its official release with a SHA-256 hash, and commits the
  KÉT Sans Mono 0.10.4 Medium TTF (SIL OFL 1.1, licence alongside), which has
  no public release. Only the Medium face is bundled, so every monospace text
  in the mock is Medium (Jay, 2026-11-06).
  Monospace text defaults to 14 px (code, diffs, inline code, conversation
  numbers) while all other text keeps the KetPlus profile sizes; the mock sets
  this through `ket::Profile::setCurrent` without kit changes (Jay, 2026-11-06).
- **Evidence is repeatable.** `--screen`, `--state`, `--dark`, `--zoom` and
  `--screenshot` render any screen; `--smoke` opens every screen and fails on
  any QML warning.

## Consequences

- A design change and its components land in the same toolkit the app uses, so
  the native implementation can reuse the kit components directly.
- Reviewing the mock needs a Qt 6.8+ build instead of a browser. There is no
  hosted link to share; screenshots come from `--screenshot`.
- Product colours still come only from tokens; the mock's literals are fixture
  data (tag colours) only.

## Alternatives considered

- **Keep KetAtlas and add Qt screenshots.** Keeps two sources of truth.
- **A Qt Widgets mock.** Closer to parts of the Pro shell, but much slower to
  iterate; the kit keeps both twins in step, so Quick loses nothing.
- **Build the mock inside the Pro target.** Couples a design prototype to the
  daemon and provider build and slows both.
