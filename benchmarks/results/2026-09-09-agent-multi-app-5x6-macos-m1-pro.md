# KetPlus performance checkpoint — five apps × six conversations — 2026-09-09

## Scenario

- 5 independent KetPlus benchmark processes, tiled in a 3 × 2 grid
- 1 synthetic repository per process
- 6 resident conversations per repository
- 200 retained messages per conversation
- 30 conversations and 6,000 retained messages in total
- Release build, Qt 6.11.1, Metal, macOS 26.2
- Apple M1 Pro with 10 CPU cores and 32 GiB RAM
- Synthetic transcripts only; no model calls were made by this stress workload

Every process measures a three-second idle interval, switches across its six
conversations over three seconds, then drives transcript scrolling for three
seconds. All five processes run concurrently.

## Results

| Metric | Result |
| --- | ---: |
| Peak aggregate sampled RSS | 683,312 KiB (667.3 MiB) |
| Sum of per-process maximum RSS | 711,264 KiB (694.6 MiB) |
| Average maximum RSS per app | 142,253 KiB (138.9 MiB) |
| Median steady idle CPU per app | 0.246% |
| Median idle frames / 3 s | 0 |
| Median conversation-switch CPU per app | 3.783% |
| Median active-scroll CPU per app | 10.485% |
| Median active-scroll frame rate | 106.232 FPS |
| Median first frame under concurrent launch | 1,871.636 ms |
| Median retain 1,200 / render selected 200 messages | 30.138 ms |

## Interpretation

- Five idle Agent Workspace processes remain event-driven: every instance
  submitted zero idle frames. Aggregate CPU samples settled around 0.4–3.4%.
- The main cost is memory: approximately 139 MiB maximum RSS per process, or
  roughly 695 MiB for five apps. Most of this is the independent Qt/Metal runtime,
  not the small transcript payload.
- Starting five Metal scene graphs at once raises median first frame to 1.87 s.
  This is a concurrent cold-start stress number, not the normal one-app startup.
- Forcing all five windows to scroll simultaneously yields a median 106 FPS.
  Two processes fell below 100 FPS due to compositor scheduling contention. A
  normal workflow, where only the foreground app scrolls, remains represented by
  the separate single-app checkpoint.
- Switching among six resident conversations is inexpensive and does not cause
  background conversations to repaint continuously.

Raw measurements: `raw-20260909-agent-multi-app-5x6.txt`.
