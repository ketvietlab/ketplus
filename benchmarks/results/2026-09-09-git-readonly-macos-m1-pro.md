# KetPlus performance checkpoint — read-only Git — 2026-09-09

## Environment

- Base revision reported by the benchmark: `d1b5ca0`
- Source state: working tree with the read-only Git integration applied
- Build: Release, AppleClang
- KetPlus: 0.1.0
- Qt: 6.11.1
- OS: macOS 26.2, Darwin 25.2.0
- Hardware: Apple M1 Pro, 10 CPU cores, 32 GiB RAM
- Application bundle: 6.3 MiB before Qt deployment

This checkpoint was taken before the Git integration received its own commit
because other feature work was present in the shared working tree. The comparison
therefore detects overall movement since `54d3c2a`; it must not be attributed to
Git alone. Git repository discovery and status are asynchronous and only start
when a workspace folder is open, so the existing empty/file startup cases verify
that the integration adds no synchronous startup penalty.

One complete suite was run. Each native startup case contains seven launches;
the table reports the median. Confirm a future suspected regression with three
complete suites, as required by the benchmark protocol.

## Editor engine

| Workload | Current median | Previous checkpoint | Change |
| --- | ---: | ---: | ---: |
| Set 1 MiB text | 0.542 ms | 0.553 ms | -2.0% |
| Set 10 MiB text | 5.001 ms | 5.533 ms | -9.6% |
| Set 50 MiB text | 25.576 ms | 28.916 ms | -11.6% |
| Find match at end of 10 MiB | 1.364 ms | 1.425 ms | -4.3% |
| Replace All in 10 MiB | 154.024 ms | 164.976 ms | -6.6% |

No editor-engine workload regressed.

## Native application startup

| Workload | Startup median | Median max RSS | Median peak footprint | Previous startup | Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Empty document | 367.769 ms | 123.5 MiB | 44.2 MiB | 476.874 ms | -22.9% |
| Open 1 MiB text | 372.877 ms | 127.6 MiB | 48.4 MiB | 707.498 ms | -47.3% |
| Open 10 MiB text | 387.416 ms | 155.0 MiB | 75.8 MiB | 708.763 ms | -45.3% |
| Open 50 MiB text | 420.001 ms | 280.2 MiB | 200.7 MiB | 778.427 ms | -46.0% |
| Open 10 MiB generic | 382.170 ms | 141.8 MiB | 62.7 MiB | 1,049.521 ms | -63.6% |
| Open 10 MiB C++ | 397.033 ms | 155.3 MiB | 76.1 MiB | 1,071.952 ms | -63.0% |

No measured startup case regressed. The large improvement in highlighted-file
startup includes changes made after the previous syntax-highlighting checkpoint
and should not be credited to the Git integration.

## Git-specific verification

The automated Git integration test creates a real temporary repository and a
linked worktree, modifies a tracked file, adds an untracked file, then verifies:

- repository and linked-worktree discovery;
- porcelain-v2 status parsing;
- separate staged and unstaged classification and diff generation;
- tracked-file and untracked-file diff generation;
- no staging, commit, checkout, or other repository mutation.

The full test suite passes: 5/5 test executables.

Raw measurements: `raw-20260909-git-readonly-working-tree.txt`.
