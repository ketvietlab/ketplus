# KetPlus performance checkpoint — Qt Quick Agent Workspace — 2026-09-09

## Environment

- Base revision: `be4a4d1`
- Source state: working tree with the Qt Quick Agent Workspace UI mock
- Build: Release, AppleClang
- KetPlus: 0.1.0
- Qt: 6.11.1
- OS: macOS 26.2, Darwin 25.2.0
- Hardware: Apple M1 Pro, 10 CPU cores, 32 GiB RAM
- Qt Quick renderer: Metal

Three independent process runs were recorded. The table reports medians. The
mock contains 1,000 mixed user, assistant, tool, and file messages in a
virtualized `ListView`. Scroll is driven at 8 ms intervals for two seconds on a
120 Hz display. Before idle measurement, the benchmark waits 2.5 seconds for
transient scrollbar animations to settle.

The desktop was locked during this run, so visual inspection was unavailable.
The benchmark still received Metal `frameSwapped` events at the expected display
rate. Repeat once with an unlocked, otherwise idle desktop before using the
first-frame number as a release gate.

## Results

| Workload | Median |
| --- | ---: |
| First presented frame | 257.249 ms |
| Populate and present 1,000 messages | 32.598 ms |
| Steady idle CPU | 0.341% |
| Steady idle frames / 2 s | 0 |
| Full-width scroll CPU | 38.949% |
| Full-width scroll rate | 118.044 FPS |
| Resize to 520 px context split | 23.833 ms |
| Context-split scroll CPU | 27.651% |
| Context-split scroll rate | 119.530 FPS |

## Interpretation

- The UI is genuinely rendered through Metal, not the Qt software backend.
- Steady idle is event-driven: it submits no frames and stays well below the 2%
  CPU guardrail.
- Both widths remain above the 110 FPS guardrail for the 120 Hz workload.
- Roughly 28–39% of one CPU core is still used while forcing 120 content updates
  per second. GPU rendering removes raster paint from the CPU but does not remove
  QML layout, text shaping, model updates, and scene-graph submission. This is a
  useful baseline, not evidence that scroll cost is solved everywhere.
- The first run paid additional cold initialization cost. The median is used so
  later feature checkpoints can be compared consistently.

Raw measurements: `raw-20260909-agent-workspace-working-tree.txt`.
