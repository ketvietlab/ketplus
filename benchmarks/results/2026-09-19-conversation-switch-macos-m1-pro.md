# Conversation switching and history measurement — 2026-09-19

## Setup

- Apple M1 Pro, 32 GiB RAM, macOS 26.2 / Darwin 25.2.0, Qt 6.11.1.
- Release, Apple Clang 21, `-O3 -DNDEBUG`, offscreen Qt.
- Base: `073cbc2fdb1110e4a3e9af8b6cc0c35e95aaceb0` (`origin/develop`
  verified before the final build). Candidate: the uncommitted conversation
  switching and reentrant row-window changes on top of that revision.
- Same `ConversationSwitchBenchmark.cpp` fixture in both executables. The
  baseline AI archive was rebuilt with the five affected AI translation units
  and the paging/window headers from the base revision; unchanged objects,
  dependencies and compiler settings were reused. The desktop shell is not
  exercised by this benchmark.
- Three interleaved baseline/candidate rounds after builds and tests completed.
  Two synthetic conversations, 90 turns each, approximately 8 KiB per assistant
  body. The fixture repeats the same body and item IDs in both conversations;
  results therefore include any reusable body measurements across selection.
- History mode disables read-ahead to isolate initial publication. Baseline
  publishes ten pages / 60 messages; candidate publishes one page / 6 messages.
  Older history remains available through paging. Warm selection reads the
  already loaded session without constructing a QML viewport.
- Measurement mode premeasures the same 60 rows four times per process. The
  first sample includes font/layout initialization; the remaining three samples
  test repeated work after the cache is populated.
- All journal/settings data are temporary and synthetic. No installed daemon,
  user transcript or live provider is used.

Raw measurements: [raw-20260919-conversation-switch.txt](raw-20260919-conversation-switch.txt).
The raw file records the tracked diff hash at measurement time; it does not hash
new untracked fixture/result files.

## Results

Medians in milliseconds. History and first-measurement rows have three samples
per arm; repeated measurement has nine samples per arm. CPU is total process CPU,
including workers and the in-process daemon, so it can exceed elapsed time.

| Operation | Baseline elapsed | Candidate elapsed | Baseline CPU | Candidate CPU |
| --- | ---: | ---: | ---: | ---: |
| Initial history, fresh process | 135.322 | 87.641 | 287.151 | 70.519 |
| Select cold second conversation | 54.897 | 9.787 | 220.021 | 4.644 |
| Select warm first conversation | 1.460 | 1.206 | — | — |
| First measurement of 60 rows | 114.519 | 103.918 | 279.707 | 271.995 |
| Repeat measurement of same 60 rows | 34.511 | 1.798 | 218.038 | 3.051 |

Cold selection elapsed time fell about 82%; repeated measurement elapsed time
fell about 95% and CPU about 99% for this fixture. This measures earlier bounded
publication and avoiding repeated text layout, not loading equal history volumes
faster. Fresh font/layout initialization remains visible. One baseline repeated
measurement took 104.376 ms; it is retained in the raw data.

These are backend/offscreen measurements, not native click-to-paint, frame rate,
GPU time or RSS measurements. The shared QML viewport still rebuilds delegates
when its model changes. Native interaction was not verified because computer-use
permission had been denied in the preceding session.

## Correctness and build verification

- Full Release build and `ketplus_bundle_daemon` succeeded. The embedded helper
  is byte-identical to the newly built daemon. Build type and `-DNDEBUG` were
  checked in the CMake cache. The public CM gitlink remains at `cm-v0.2.3`.
- Conversation navigation, desktop transcript history, row measurement,
  transcript window model, streaming, viewport, desktop shell and macOS bundle
  layout targets passed. Sidebar tests cover one reset on conversation change,
  no reset when selecting the current conversation, and restoring the view path
  when returning from a missing worktree to the current workspace.
- The new history-reset regression reproduces the reported `QList::at`
  SIGABRT against the original `TranscriptWindowModel` with Qt assertions
  enabled. Both reentrant range-change tests pass after the fix, with
  `QAbstractItemModelTester` checking notifications.
- Final full CTest run: 32 of 35 targets passed. The Atlas workspace target
  failed its first-created-file ordering assertion in the parallel run and
  passed on a standalone rerun (it also passed in the preceding full run).
- Two targets still have baseline failures: `ketplus_agent_tests` has 14
  failing data cases across usage popovers, activity output, compaction,
  semantic command subtype and typography expectations;
  `ketplus_desktop_daemon_client_tests` has one failing question-status wait.
  The same cases fail at the same assertions with the affected AI code restored
  to `073cbc2`. They were not changed to make this suite appear green.
- Existing stale assertions in the directly affected history/shell tests were
  aligned with current command payload, approval subtype and 32 px search height.
- Provider-quota tests remained disabled. No app was installed or launched in
  place of the existing application.

The production change in the existing 1,400+ line `AgentSessionManager.cpp` is
limited to four lines at its session-selection cancellation boundary. Keeping
that lifecycle fence with its existing owner avoids a separate lifecycle
refactor in this crash/performance fix. No other behavior was moved into that
large file.

Known separate history issues from the earlier review (live-response overwrite
during initial publication, snapshot-retry cursor, copying an evicted newer edge,
and runtime errors without item IDs) remain outside this change.

## Follow-up: blank streaming body after opening a running conversation

The user subsequently reported a streaming label with no body until switching
away and back. Two further defects were reproduced and fixed after the benchmark
above was recorded:

- A projected public assistant row lacked the original live item-start phase
  entry. Its subsequent deltas were deferred as if it were an unclassified new
  item. Existing daemon assistant rows now continue streaming; unknown new items
  still require a public phase.
- QML compared canonical update indexes with the displayed row count. Collapsed
  activity groups made that count smaller, dropping both coalesced deltas and
  final replacements. Both update paths now use `sourceCount()`.

All six new data cases failed before the fixes and pass afterward. They cover
empty and nonempty restored answers, restored commentary, streaming after a
collapsed group, completion, and completion with a queued older delta. The
viewport cases verify the actual text document as well as model contents.
The four focused navigation/history/streaming/viewport targets passed. The final
full Release suite passed 33/35 targets; the remaining 15 assertion failures in
the Agent and desktop-daemon targets match the previously confirmed baseline
exactly. Build, bundle checks, diff whitespace and the unchanged CM gitlink were
verified again. Native interaction remains unverified.

The updated Release bundle was installed at the user's existing application
location with a backup. Its daemon executable is byte-identical to the daemon
already running, so that process was retained. The open desktop process was
also retained and must be reopened to load the new executable.

## Final provider transcript and permission verification

Subsequent work in the same change adds Claude/Cursor transcript normalization,
Codex asynchronous questions, local error/output/answer history, and daemon-owned
permissions that survive selection changes and can be cancelled by Stop. Runtime
errors without item IDs are now restored; the other separate history issues noted
above are not claimed as fixed. The measurements above were not repeated for these
provider changes and remain evidence for the original switching optimization only.

The final full Release suite passed 36/37 targets, with the same 14 baseline UI
assertions in `ketplus_agent_tests`. The desktop-daemon question-status assertion
was corrected to reflect the explicitly nonblocking request. After the final
permission lifecycle edits, all ten directly affected targets passed. A packaging
regression caught broken npm bin links; preserving relative links made the bundle
layout check pass again. No live provider calls or native UI checks were run.

The installed Release now includes the Claude SDK sidecar. Bundle and native
sidecar signatures were verified, the previous app was backed up, and the idle
daemon was restarted from the new bundle. The desktop process was left open for
the user to reopen without losing unsaved work.
