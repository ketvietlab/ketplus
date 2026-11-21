# Large workspace: 3,000 conversations over 150 repositories

Date: 2026-11-17. Release build, Apple Silicon laptop, macOS 26.2, Qt 6.11.2.

Fixture: `ketplus_large_workspace_journal` with 3,000 conversations over 150 Git
repositories, skewed so a few hold most conversations. Each conversation has 3
turns, and 10% are archived, which leaves 2,695 listed. The journal is 103 MB.

## Sidebar, offscreen (`ketplus_sidebar_scale_benchmark`, median ms of 5 rounds)

Before, every update rebuilt every row:

| Conversations | Initial | Rename / add | Select | Unchanged |
| ---: | ---: | ---: | ---: | ---: |
| 300 | 63 | 62 | 10 | 7 |
| 1,000 | 170 | 172 | 17 | 9 |
| 3,000 | 461 | 508 | 33 | 13 |
| 10,000 | 1,494 | 1,630 | 74 | 19 |

After, rows are built only for what is previewed, and only a changed repository's
group is rebuilt:

| Conversations | Initial | Rename | Add | Select | Running | Unchanged |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 3,000 | 148 | 35 | 34 | 29 | 21 | 12 |
| 10,000 | 171 | 57 | 57 | 49 | 40 | 19 |

## Real Pro app (`benchmarks/run-large-workspace.sh build/perf 3000 150 25`)

The app runs against `ketplus_fixture_daemon` and opens Agent Workspace. Each
change below builds on the ones above it.

| Change | Conversations listed | App CPU in 25 s | Idle CPU | Sidebar refresh |
| --- | ---: | ---: | ---: | ---: |
| Before | 0 (`session_catalog_too_large`) | 1.1 s | 0% | - |
| Paged live catalog (ADR 0131) | 2,695 | 17.8 s | 92% | - |
| Workspace writes deferred while applying a catalog | 2,695 | 6.0 s | 0.2% | 85 ms |
| One repository lookup per worktree, not per conversation | 2,695 | 3.6 s | 0.2% | 33 ms |
| Catalog list and activity signals coalesced | 2,695 | 1.8 s | ≈1% | 28 ms |

- The "Before" row's CPU is that of an app listing nothing.
- With the paged catalog alone, the app was still applying the catalog when the
  run ended: each workspace was written for every one of its conversations.
- The first refresh after the catalog builds every group and takes about 220 ms.
- App memory is about 300 MB and the daemon's about 48 MB.
