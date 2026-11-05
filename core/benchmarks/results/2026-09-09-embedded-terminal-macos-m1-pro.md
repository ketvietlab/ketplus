# KetPlus performance checkpoint — embedded terminal — 2026-09-09

## Environment

- Base revision reported by the benchmark: `c93128b`
- Source state: working tree with the embedded terminal and concurrent Git diff
  syntax-rendering changes applied
- Build: Release, AppleClang
- KetPlus: 0.1.0
- Qt: 6.11.1
- OS: macOS 26.2, Darwin 25.2.0
- Hardware: Apple M1 Pro, 10 CPU cores, 32 GiB RAM
- Application bundle: 6.6 MiB before Qt deployment

One complete suite was run. Each startup case contains seven launches and the
table reports the median. The comparison uses the immediately preceding styled
inline Git diff checkpoint. Terminal construction remains lazy: none of the
startup cases opens the terminal panel or starts a PTY.

This run was visibly bimodal and overlapped other working-tree activity. Keep it
as a checkpoint, but do not attribute its startup or Replace All movement to the
terminal until three quiet-system suites confirm it.

## Editor engine

| Workload | Current median | Previous checkpoint | Change |
| --- | ---: | ---: | ---: |
| Set 1 MiB text | 0.609 ms | 0.566 ms | +7.6% |
| Set 10 MiB text | 5.618 ms | 5.585 ms | +0.6% |
| Set 50 MiB text | 28.100 ms | 27.382 ms | +2.6% |
| Find match at end of 10 MiB | 1.586 ms | 1.392 ms | +13.9% |
| Replace All in 10 MiB | 209.457 ms | 168.108 ms | +24.6% |

Replace All crossed the 15% investigation threshold. The terminal is not linked
into the editor benchmark executable, so this cannot be caused by terminal
runtime initialization. Re-run on an idle system before investigating code.

## Native application startup

| Workload | Startup median | Median max RSS | Median peak footprint | Previous startup | Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Empty document | 533.773 ms | 123.1 MiB | 44.0 MiB | 390.096 ms | +36.8% |
| Open 1 MiB text | 814.844 ms | 128.0 MiB | 48.7 MiB | 394.517 ms | +106.5% |
| Open 10 MiB text | 932.101 ms | 154.3 MiB | 76.2 MiB | 407.996 ms | +128.5% |
| Open 50 MiB text | 639.621 ms | 270.7 MiB | 192.3 MiB | 463.264 ms | +38.1% |
| Open 10 MiB generic | 452.667 ms | 147.0 MiB | 67.7 MiB | 392.072 ms | +15.5% |
| Open 10 MiB C++ | 418.518 ms | 146.8 MiB | 67.6 MiB | 386.276 ms | +8.3% |

The first four startup cases crossed the 20% threshold, but individual samples
split into fast and slow clusters. Since the terminal panel is not instantiated
in these cases, a terminal-specific startup cost would be limited to loading its
linked code. Confirmation runs are required before accepting these numbers as a
regression.

## Terminal verification

- A real interactive shell starts through `forkpty` in the requested working
  directory.
- PTY output passes through libvterm, including ANSI color sequences.
- The terminal test executable passes 4/4 Qt test cases.
- The complete application links successfully and all 7/7 test executables pass.

Raw measurements: `raw-20260909-embedded-terminal-working-tree.txt`.
