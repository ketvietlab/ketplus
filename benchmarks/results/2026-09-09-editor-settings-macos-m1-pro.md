# KetPlus performance checkpoint — editor settings — 2026-09-09

## Environment

- Base revision reported by the benchmark: `c93128b`
- Source state: working tree with editor typography settings and the concurrent
  embedded-terminal changes applied
- Build: Release, AppleClang
- KetPlus: 0.1.0
- Qt: 6.11.1
- OS: macOS 26.2, Darwin 25.2.0
- Hardware: Apple M1 Pro, 10 CPU cores, 32 GiB RAM
- Application bundle: 6.6 MiB before Qt deployment

One complete suite was run. Each startup case contains seven launches and the
table reports the median. The comparison uses the immediately preceding
embedded-terminal checkpoint, which was visibly bimodal and overlapped other
working-tree activity. Startup movements therefore need confirmation before
being attributed to this feature.

## Typography application

The new repeatable workload alternates between 13/24 px and 14/25 px typography
on a highlighted 1 MiB C++ document. Its initial baseline is **20.816 ms**. This
includes updating Scintilla styles, line metrics, gutter width, and re-lexing the
document.

The Settings dialog is constructed only when requested. Normal startup loads
three small values from native application settings and creates no dialog.

## Editor engine

| Workload | Current median | Previous checkpoint | Change |
| --- | ---: | ---: | ---: |
| Set 1 MiB text | 0.610 ms | 0.609 ms | +0.2% |
| Set 10 MiB text | 5.729 ms | 5.618 ms | +2.0% |
| Set 50 MiB text | 29.032 ms | 28.100 ms | +3.3% |
| Find match at end of 10 MiB | 1.431 ms | 1.586 ms | -9.8% |
| Replace All in 10 MiB | 177.360 ms | 209.457 ms | -15.3% |

No editor-engine workload regressed past the 15% investigation threshold.

## Native application startup

| Workload | Startup median | Median max RSS | Median peak footprint | Previous startup | Change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Empty document | 387.649 ms | 124.2 MiB | 44.5 MiB | 533.773 ms | -27.4% |
| Open 1 MiB text | 425.191 ms | 128.0 MiB | 48.3 MiB | 814.844 ms | -47.8% |
| Open 10 MiB text | 413.024 ms | 155.3 MiB | 75.6 MiB | 932.101 ms | -55.7% |
| Open 50 MiB text | 610.260 ms | 280.1 MiB | 200.4 MiB | 639.621 ms | -4.6% |
| Open 10 MiB generic | 504.102 ms | 156.0 MiB | 76.1 MiB | 452.667 ms | +11.4% |
| Open 10 MiB C++ | 548.043 ms | 141.8 MiB | 63.3 MiB | 418.518 ms | +31.0% |

The C++ case crossed the 20% threshold while the other startup cases moved in
the opposite direction. Both runs were noisy, so this isolated result is not
treated as a confirmed regression. The feature adds no eager Settings dialog or
document restyling during startup.

## Verification

- Custom font size and line height are verified against Scintilla's effective
  style size and rendered row height.
- Font, size, and line-height persistence is verified through a fresh native
  settings store.
- Git diff typography follows the same settings.
- The Settings dialog and `Cmd+,` entry point were exercised in the built app.
- Editor and Git diff tests pass. The complete suite currently has one unrelated
  failure in the concurrently developed terminal focus-restoration test.

Raw measurements: `raw-20260909-settings-working-tree.txt`.
