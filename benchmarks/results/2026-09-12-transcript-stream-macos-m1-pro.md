# Transcript streaming — final combined source

Measured on 2026-09-12. Runtime source:
`1e5c9ac5d8a8c3df3e6fd3a6ba06c9bc1a9c6dd3`, based on develop
`3c28339`; immutable CM pin `cm-v0.1.5` /
`9fb86c8ccf64bb75e1e5cdf31314cedf46d24439`.

Environment: Apple M1 Pro, 32 GiB RAM, macOS Tahoe 26.2, arm64,
Qt 6.11.1, Release, Qt offscreen platform.

## Result

This compares direct and coalesced presentation paths **in the same binary**.
Both include the common renderer optimizations in this revision; the direct path
is not a measurement of the previously installed app.

| Historical rows | Direct CPU, median | Coalesced CPU, median | CPU reduction | Publications, median |
| --- | ---: | ---: | ---: | ---: |
| 1,000 | 3,174.634 ms | 1,013.504 ms | 68.07% | 300 → 56 |
| 10,000 | 3,202.783 ms | 1,052.529 ms | 67.14% | 300 → 56 |

Published-character work proxy fell from 1,252,020 to 235,958 (1k) and
237,731 (10k), about 81%. This counts published QString lengths, not parser bytes.
Median wall times were 8,135 → 6,831 ms (1k) and 8,138 → 6,864 ms (10k);
these include the synthetic waits and are not user-perceived latency measurements.

Benchmark: **14 passed, 0 failed, 0 skipped** (12 data samples plus initialization
and cleanup), 94.232 seconds. Each sample checks the exact final body; the
coalesced path also checks fewer than 100 publications.

[Raw samples, summaries, environment and binary hashes](2026-09-12-transcript-stream-macos-m1-pro.json)
are committed without user content or personal paths. Binary hashes identify the
tested artifacts; runtime source revision is recorded separately from any
build-time version metadata.

## Reproduce

From a configured Release build with private tests enabled:

```sh
cmake --build build/release --parallel 2
bash benchmarks/run-transcript-stream.sh build/release /tmp/ketplus-transcript-stream-benchmark.log
```

The script sets `QT_QPA_PLATFORM=offscreen`,
`KETPLUS_DISABLE_DAEMON_AUTOSTART=1` and
`KETPLUS_RUN_STREAM_BENCHMARK=1`. No provider or running daemon is contacted.

The fixture seeds 1k or 10k short user rows and one active assistant. After a
250 ms settle, it submits 300 growing prefixes of an 8,320-character Markdown
body, waiting 20 ms after each update. Completion flushes and settles for 50 ms.
Three repetitions interleave 1k direct/coalesced and 10k direct/coalesced.
CPU uses `std::clock()`; wall time uses `QElapsedTimer`. Because each wait
follows processing, this is not an independently scheduled network arrival test.

## Correctness and remaining limits

The combined Release build and **19/19 private CTest targets passed**
(39.88 seconds). Opt-in provider/native cases were not enabled. Focused tests
cover the local newest-three-turn window, backward paging, live/history
identity, approval restoration, inactive model eviction, and reader-position
retention through streaming and completion.

This benchmark does **not** measure native frame time/FPS/RSS/GPU, approve
pixels, compare an installed old binary, exercise giant active messages, or
measure the complete restore/cache architecture. A visible giant message remains
one QTextDocument; selected history/full export can still grow. See the
[implementation handoff and open findings](../../tasks/transcript-performance-followthrough.md).

No collapse/turn navigator, package distribution, installed-app replacement,
daemon replacement or relaunch is part of this evidence.
