# KetPlus performance follow-up — syntax highlighting — 2026-09-09

## Environment

- Revision: `54d3c2a`
- Build: Release, AppleClang 21.0.0
- KetPlus: 0.1.0
- Qt: 6.11.1
- OS: macOS 26.2, Darwin 25.2.0
- Hardware: Apple M1 Pro, 10 CPU cores, 32 GiB RAM
- Application bundle: 6.1 MiB before Qt deployment

The current revision was compared with baseline revision `9159294`. Several
feature groups landed between these revisions, so the comparison detects overall
regressions but does not attribute every difference to syntax highlighting.

Three complete suites were run for the existing workloads. Each startup case
contains seven launches; the representative result below is the median of the
three per-suite medians. A fourth suite added the new generic-highlight case for
a same-run comparison with plain text and C++.

## Editor engine

| Workload | Current median | Baseline | Change |
| --- | ---: | ---: | ---: |
| Set 1 MiB text | 0.553 ms | 0.546 ms | +1.3% |
| Set 10 MiB text | 5.533 ms | 6.228 ms | -11.2% |
| Set 50 MiB text | 28.916 ms | 33.331 ms | -13.2% |
| Find match at end of 10 MiB | 1.425 ms | 1.481 ms | -3.8% |
| Replace All in 10 MiB | 164.976 ms | 181.433 ms | -9.1% |

No editor-engine workload crossed the 15% regression threshold.

## Native application startup

| Workload | Current median | Baseline | Change |
| --- | ---: | ---: | ---: |
| Empty document | 476.874 ms | 406 ms | +17.5% |
| Open 1 MiB text | 707.498 ms | 685 ms | +3.3% |
| Open 10 MiB text | 708.763 ms | 707 ms | +0.2% |
| Open 50 MiB text | 778.427 ms | 744 ms | +4.6% |
| Open 10 MiB C++ with lexer | 1,071.952 ms | 1,029 ms | +4.2% |

Empty startup varied substantially between suites (388–515 ms median), but its
confirmed representative result remained below the 20% investigation threshold.
All file-opening cases were within 5% of the baseline.

## Generic highlighting cost

These values come from the same fourth suite, with seven launches per case.

| 10 MiB workload | Startup median | Median max RSS | Median peak footprint |
| --- | ---: | ---: | ---: |
| Plain text | 700.290 ms | 165.7 MiB | 86.7 MiB |
| Generic highlighting | 1,049.521 ms | 167.9 MiB | 88.6 MiB |
| C++ highlighting | 1,066.212 ms | 167.8 MiB | 88.6 MiB |

Generic highlighting adds 349.231 ms and about 1.9 MiB of peak physical
footprint when synchronously opening this synthetic 10 MiB source file. It is
16.691 ms faster than the C++ profile and has effectively the same memory cost.
This is expected because the generic profile deliberately uses Lexilla's mature
C-family lexer with a smaller cross-language keyword set.

## Interpretation

- The highlighting change introduces no threshold-level regression in the
  existing benchmark suite.
- Generic highlighting remains responsive for normal files, but synchronous
  full-document lexing is visible at 10 MiB. Large files should eventually use
  a size threshold, deferred styling, or both.
- The 50 MiB plain-text peak footprint remains about 251 MiB, so large-file
  memory is still the main performance concern.
- Qt reported a 338–479 ms one-time font-alias population warning before the
  in-process editor workload. That initialization is outside the measured
  editor operations but is worth removing from future startup work.

Raw measurements:

- `raw-20260909T055034Z-54d3c2a.txt`
- `raw-20260909T055244Z-54d3c2a.txt`
- `raw-20260909T055334Z-54d3c2a.txt`
- `raw-20260909-generic-highlight-54d3c2a.txt`
