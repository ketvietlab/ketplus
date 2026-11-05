# Daemon transcript restore — macOS M1 Pro

Date: 2026-09-11

Base revision: `9942e83` plus the restore-performance working tree

Machine: Apple M1 Pro, 32 GiB RAM

Build: Release, Qt 6.11+, arm64

The benchmark creates 300 synthetic completed turns containing 7,200 journal
events, including 6,000 assistant streaming deltas. It starts the local daemon
and measures Desktop discovery, compact paged IPC replay, and reconstruction of
the expected 600 user/assistant transcript rows. No provider process or quota is
used.

| Run | Raw events | Replayed events | Pages | Messages | Restore |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 7,200 | 1,200 | 75 | 600 | 330 ms |
| 2 | 7,200 | 1,200 | 75 | 600 | 329 ms |
| 3 | 7,200 | 1,200 | 75 | 600 | 324 ms |

Compaction removes 83.3% of replay dispatches for this completed-turn workload.
All runs reconstructed the complete transcript. This baseline exercises the
restore pipeline but does not replace native launch validation against the
installed application and its existing journal.

Raw output: `raw-20260911-daemon-restore-working-tree.txt`.
