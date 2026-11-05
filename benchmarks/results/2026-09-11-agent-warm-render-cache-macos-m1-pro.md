# Agent warm-render cache checkpoint — 2026-09-11

## Scenario

- Release build, Qt 6.11.1, Metal, macOS 26.2
- Apple M1 Pro with 32 GiB RAM
- One Agent Workspace process with 200 synthetic messages per conversation
- Three-second idle, one cold traversal, two warm traversals, then three-second scroll
- Synthetic transcripts only; no provider calls

The benchmark assigns a stable scope to every conversation so it exercises the
production render cache. Peak RSS comes from `getrusage` at process completion.

## Raw results

| Conversations | Warm views | Populate | Cold-switch CPU | Warm-switch CPU | Peak RSS |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 852.787 ms | 0.685% | 0.643% | 192,912 KiB |
| 6 | 6 | 644.220 ms | 22.566% | 17.089% | 303,504 KiB |
| 12 | 8 | 807.490 ms | 54.507% | 100.822% | 426,032 KiB |

## Interpretation

- Six simultaneously warm render views add about 108 MiB over one view, or
  approximately 22 MiB for each additional rendered conversation in this fixture.
- Switching within the six-entry working set drops CPU from 22.6% during cold
  construction to 17.1% on the warm path.
- Twelve visited conversations retain only eight live render views, confirming
  the application-wide cap. Evicted QML objects are destroyed, but the process
  allocator and scene graph do not immediately return their high-water pages to
  macOS, so peak RSS can remain above the live-object budget.
- Repeatedly cycling all 12 conversations exceeds the cache working set and
  intentionally becomes a cache-thrash workload. Its 100.8% CPU result is the
  bounded-memory trade-off, not the warm fast path.
