# KetPlus performance checkpoint — agent runtime and +2 px UI type — 2026-09-09

## Environment

- Base revision: `be4a4d1`
- Source state: working tree with Codex app-server runtime and Agent Workspace
  typography increased by 2 px
- Build: Release, AppleClang
- KetPlus: 0.1.0
- Qt: 6.11.1
- Codex CLI: 0.153.4
- OS: macOS 26.2, Darwin 25.2.0
- Hardware: Apple M1 Pro, 10 CPU cores, 32 GiB RAM
- Qt Quick renderer: Metal

Three independent processes rendered 1,000 mixed messages. The window was
explicitly raised and activated so macOS would not suppress scene-graph frames
while it was occluded. The table reports medians.

## Results

| Workload | Median |
| --- | ---: |
| First presented frame | 914.075 ms |
| Populate and present 1,000 messages | 51.760 ms |
| Steady idle CPU | 2.817% |
| Steady idle frames / 2 s | 34 |
| Full-width scroll CPU | 47.732% |
| Full-width scroll rate | 119.006 FPS |
| Resize to 520 px context split | 13.535 ms |
| Context-split scroll CPU | 31.928% |
| Context-split scroll rate | 119.881 FPS |

## Comparison and interpretation

- Both scroll widths still sustain the 120 Hz workload and remain above the
  110 FPS guardrail after the larger type scale.
- Full-width scroll CPU rose from 38.949% to 47.732% (+22.6%); split-view CPU
  rose from 27.651% to 31.928% (+15.5%). Larger text increases shaping and
  layout cost, and the full-width change crosses the 20% investigation line.
- Populate time rose from 32.598 ms to 51.760 ms. The benchmark dataset is
  intentionally synthetic and creates all 1,000 model rows at once; live
  streaming appends or updates one row at a time.
- The first-frame and idle-frame numbers are not directly comparable to the
  previous locked-desktop baseline because this checkpoint foregrounds the
  window. Foreground activation also allows focus/cursor-related frames to be
  submitted. Future checkpoints should use this new foreground protocol.
- The agent manager and app-server are lazy and are not started by this UI-only
  workload, so these results isolate the final QML presentation cost.

Raw measurements: `raw-20260909-agent-runtime-font-plus-2.txt`.
