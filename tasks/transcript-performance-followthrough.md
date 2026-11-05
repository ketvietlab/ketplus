# Transcript performance follow-through

Scope: private Desktop/daemon only. The turn navigator is explicitly deferred in
[its debt entry](transcript-turn-navigator-debt.md). No CM, ketjs, ketatlas,
mobile/cloud protocol, provider-quota or user-journal changes are required here.
Collapsing working/activity groups is explicitly outside this change; it was
discussed separately and is not enabled here.

## Implemented in this branch

- Coalesce rendered streaming snapshots; canonical input remains lossless.
- Stop repeated same-worktree skill scans during Git refresh.
- Advertised local bounded newest-three-turn window; metadata-only cold discovery.
- Bounded backward cursor pages, including within turns; fail closed on compaction.
- Pure history projection; current lifecycle and approval snapshots remain separate.
- Live/history scoped identities; inactive text invalidates instead of accumulating.
- Soft inactive data/view text budgets; model release follows render-view eviction.
- Offscreen formatting deferral and source pin for the live message being read.
- Explicit partial Markdown context, bounded prefix anchor repair, top-boundary loading.

## Still open — do not claim all performance findings closed

1. Giant assistant bodies are now split into stable 12 KiB target / 16 KiB maximum
   render rows and published incrementally from a worker. Native selection and
   accessibility ranges do not yet span those independent rows; canonical full
   copy/export remains available separately. See
   [renderer gap](transcript-bounded-renderer-gap.md).
2. The active presentation window is now hard bounded to 300 messages / 16 MiB,
   with cursor-backed edge rehydration and anchor-safe eviction. The budget is a
   UTF-8/model-text bound, not a physical process or GPU-memory ceiling. One
   indivisible canonical event may exceed the target so it is never silently
   truncated; its presentation still uses bounded render rows.
3. Native CPU/frame-time/RSS and
   pixel captures for this combined revision are not yet measured.
4. Existing 256 KiB individual persisted-event cap remains. Paging cannot recover
   events a previous producer failed to persist; lossless large-event storage is
   a separate producer/storage change.
5. Unsupported old daemons still use compatibility full replay. No new packaged
   app, installed binary, daemon replacement or relaunch is performed in this turn.

## Source ownership / review note

New daemon/client projection and window logic live in focused files. The existing
large AgentWorkspace.qml remains the integration point for the body delegate and
scroll transaction because its text-document, selection and anchor interfaces are
coupled. This change does not claim to have completed that file's decomposition;
extracting it without a tested render-unit contract would increase parity risk.

## Verification

Combined Release build passed on macOS arm64 / Qt 6.11.1. Full private CTest
passed 21/21 targets (109.87 seconds) after integrating the shell, lifecycle,
files, questions and attachment verticals. Provider/native checks remain opt-in and
were not enabled. The viewport target covers middle-reader streaming/final
reflow, scrollbar holds, LF/CRLF prefix loads, offscreen updates and Latest.
The client window target covers selected-only hydration, the 300-message / 16 MiB
hard window, lossless cursor-backed edge rehydration, separate export,
inactive eviction/reload, scope switching, and a pending
approval restored alongside its command without automatic decline.

Deterministic fixtures only; no real transcript is committed. A final combined
Release run passed all 14 streaming benchmark cases. Coalescing reduced median
CPU from 3,720.273 to 2,015.289 ms at 1k rows (45.83%) and from 4,809.591 to
2,523.956 ms at 10k rows (47.52%), with 300 publications reduced to 59 and 59
respectively. The run overlapped an active installed KetPlus session and is not
an idle-machine replacement for the pinned baseline. The 10k native stress run
reported 97.859 ms populate, 0.478% idle CPU, zero idle frames, 230,784 KiB max
RSS and 96.021 scroll FPS. The 8 MiB pathological item materialized 681 bounded
rows in 65.394 ms with a 28.540 ms maximum UI gap, 211,392 KiB max RSS and
102.061 scroll FPS. These FPS observations remain below the documented 110 FPS
investigation threshold and therefore are evidence, not a release-gate pass.

Synthetic stream
benchmarks compare presentation paths, not native end-user FPS or the full new
architecture. The [final benchmark report](../benchmarks/results/2026-09-12-transcript-stream-macos-m1-pro.md)
records 12 raw samples: median process CPU is 68.07% lower for 1k historical rows
and 67.14% lower for 10k, comparing direct/coalesced paths in the same binary.
These are not native FPS/RSS or installed-old-app improvement claims.
