# Két Blue design system evidence

Captured 2026-11-05 with Chromium 157 at 1280 × 800 (device scale 2, saved at 1×)
from `screens/connections.html`, after materializing `@ketvietlab/design-system`
0.1.44 at ketviet `e8adee2d` (PR #2177).

- `connections-light.png`: the primary button measures `rgb(54, 95, 214)`
  (`--kv-action-primary-bg`, `#365FD6`) with white text. No application switcher
  rail: the shell's first child is the sidebar.
- `connections-dark.png`: the selected navigation item uses
  `--kv-nav-selected-text` (`#DDE6FF`) on `--kv-accent-subtle` (`#202C4B`), and the
  accent icon uses `--kv-accent-text` (`#9EB5FF`). Before this change both used
  `--kv-accent` (`#1746FF`), about 2.2:1 on that surface.

Compared with the 0.1.7 bundle, the only layout change seen on this screen is
taller Linux machine card padding from the new surface styles.
