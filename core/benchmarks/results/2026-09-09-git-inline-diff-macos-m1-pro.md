# KetPlus performance checkpoint — styled inline Git diff — 2026-09-09

## Environment

- Base revision reported by the benchmark: `053a5de`
- Source state: working tree with the styled inline Git diff applied
- Build: Release, AppleClang
- KetPlus: 0.1.0
- Qt: 6.11.1
- OS: macOS 26.2, Darwin 25.2.0
- Hardware: Apple M1 Pro, 10 CPU cores, 32 GiB RAM
- Application bundle: 6.4 MiB before Qt deployment

One complete suite was run. Each startup case contains seven launches and the
table reports the median. The comparison uses the stable read-only Git checkpoint
instead of the intervening Source Control result, whose startup measurements were
heavily affected by system scheduling and concurrent base-revision changes.

## Editor engine

| Workload | Current median | Read-only Git checkpoint | Change |
| --- | ---: | ---: | ---: |
| Set 1 MiB text | 0.566 ms | 0.542 ms | +4.4% |
| Set 10 MiB text | 5.585 ms | 5.001 ms | +11.7% |
| Set 50 MiB text | 27.382 ms | 25.576 ms | +7.1% |
| Find match at end of 10 MiB | 1.392 ms | 1.364 ms | +2.1% |
| Replace All in 10 MiB | 168.108 ms | 154.024 ms | +9.1% |

No editor-engine workload crossed the 15% investigation threshold.

## Native application startup

| Workload | Startup median | Median max RSS | Median peak footprint | Previous startup | Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Empty document | 390.096 ms | 123.1 MiB | 44.1 MiB | 367.769 ms | +6.1% |
| Open 1 MiB text | 394.517 ms | 113.0 MiB | 35.0 MiB | 372.877 ms | +5.8% |
| Open 10 MiB text | 407.996 ms | 140.7 MiB | 62.8 MiB | 387.416 ms | +5.3% |
| Open 50 MiB text | 463.264 ms | 279.6 MiB | 200.6 MiB | 420.001 ms | +10.3% |
| Open 10 MiB generic | 392.072 ms | 155.2 MiB | 76.0 MiB | 382.170 ms | +2.6% |
| Open 10 MiB C++ | 386.276 ms | 141.9 MiB | 63.2 MiB | 397.033 ms | -2.7% |

No startup or memory case crossed the 20% investigation threshold. The inline
diff view is created only when a diff is opened, so it adds no normal-startup
allocation. A structured-render test verifies gutter line numbers, hunk rows,
added/removed markers, summary counts, and removal of raw patch metadata. The
full suite passes: 6/6 test executables.

Raw measurements: `raw-20260909-git-inline-diff-working-tree.txt`.
