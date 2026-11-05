# KetPlus performance checkpoint — Git changes panel — 2026-09-09

## Environment

- Base revision reported by the benchmark: `b35aac2`
- Source state: working tree with the staged/unstaged Source Control panel applied
- Build: Release, AppleClang
- KetPlus: 0.1.0
- Qt: 6.11.1
- OS: macOS 26.2, Darwin 25.2.0
- Hardware: Apple M1 Pro, 10 CPU cores, 32 GiB RAM
- Application bundle: 6.4 MiB before Qt deployment

Three complete suites were run. Each native startup case contains seven launches;
the representative value is the median of the three per-suite medians. The
immediately preceding Git checkpoint used base revision `d1b5ca0`. Revision
`b35aac2` and other working-tree changes landed between checkpoints, so the
startup comparison measures the combined tree and cannot attribute movement to
the Source Control panel alone.

A later checkpoint on the same machine returned to the normal startup range; see
`2026-09-09-git-inline-diff-macos-m1-pro.md`. This confirms that the elevated
numbers below were not a persistent Source Control panel cost.

## Editor engine

| Workload | Current median | Previous Git checkpoint | Change |
| --- | ---: | ---: | ---: |
| Set 1 MiB text | 0.542 ms | 0.542 ms | 0.0% |
| Set 10 MiB text | 5.281 ms | 5.001 ms | +5.6% |
| Set 50 MiB text | 27.577 ms | 25.576 ms | +7.8% |
| Find match at end of 10 MiB | 1.367 ms | 1.364 ms | +0.2% |
| Replace All in 10 MiB | 152.889 ms | 154.024 ms | -0.7% |

No editor-engine workload crossed the 15% investigation threshold.

## Native application startup

| Workload | Startup median | Median max RSS | Median peak footprint | Previous startup | Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Empty document | 438.984 ms | 109.6 MiB | 31.7 MiB | 367.769 ms | +19.4% |
| Open 1 MiB text | 726.360 ms | 128.0 MiB | 49.2 MiB | 372.877 ms | +94.8% |
| Open 10 MiB text | 755.113 ms | 154.9 MiB | 76.7 MiB | 387.416 ms | +94.9% |
| Open 50 MiB text | 779.912 ms | 280.2 MiB | 201.1 MiB | 420.001 ms | +85.7% |
| Open 10 MiB generic | 753.532 ms | 156.1 MiB | 76.8 MiB | 382.170 ms | +97.2% |
| Open 10 MiB C++ | 761.395 ms | 155.4 MiB | 76.4 MiB | 397.033 ms | +91.8% |

The file-opening startup regression is confirmed for the combined source tree,
but it is not attributable to the Git changes panel: that widget is created
only when Source Control is opened, while every startup benchmark above leaves
it unconstructed. Peak memory remains effectively flat. This result is retained
as a warning for the next startup investigation rather than accepted as a Git UI
cost.

## Git verification

The Git integration test now verifies that a file changed both before and after
staging appears in both logical groups, and that:

- the staged diff compares `HEAD` with the index;
- the unstaged diff compares the index with the working tree;
- untracked files use a read-only no-index diff;
- no Git command exposed by KetPlus mutates the repository.

The full test suite passes: 5/5 test executables.

Raw measurements:

- `raw-20260909-git-changes-panel-working-tree.txt`
- `raw-20260909-git-changes-panel-working-tree-2.txt`
- `raw-20260909-git-changes-panel-working-tree-3.txt`
