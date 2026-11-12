# KetPlus benchmarks

Performance is measured after each meaningful feature group so regressions are
visible while they are still easy to diagnose. Results are only comparable when
the build type, machine, operating system, and benchmark protocol match.

Run the complete suite from the repository root:

```sh
./benchmarks/run.sh
```

Simulate five independent app processes, each retaining six conversations:

```sh
./benchmarks/run-agent-multi-app.sh 5 6 200
```

Measure daemon-backed transcript restore without calling a model:

```sh
./benchmarks/run-daemon-restore.sh
```

Measure cold/warm conversation selection and repeated history body measurement
with two synthetic 90-turn conversations (8 KiB assistant bodies):

```sh
cmake --preset release -DKETPLUS_BUILD_BENCHMARKS=ON
bash benchmarks/run-conversation-switch.sh build/release /tmp/conversation-switch.txt
```

This offscreen benchmark reports process CPU and elapsed time, isolates initial
paging with read-ahead disabled, and never connects to the installed daemon or a
provider. It does not measure native frames or sidebar click-to-paint latency.
The script accepts an optional third argument pointing to a baseline executable
built with the same fixture and Release settings. It interleaves baseline and
candidate arms in each of three rounds. When invoking the executable directly,
its optional third argument overrides initial page count for a same-build
batching comparison (`history 8 10`).

Exercise the bounded renderer with 1,000/10,000 rows and an indivisible 8 MiB
assistant response (all data is synthetic):

```sh
build/release/ketplus_agent_benchmark --multi-app-stress --conversations 1 --messages-per-conversation 1000
build/release/ketplus_agent_benchmark --multi-app-stress --conversations 1 --messages-per-conversation 10000
build/release/ketplus_agent_benchmark --pathological-assistant-mib 8
```

The giant-response run reports the number of independently virtualized block
rows, synchronous reset time, first usable viewport, complete materialization,
maximum observed event-loop gap and RSS. No rendered rich-text document may
retain more than 16 Ki characters.

This stress case uses synthetic transcripts and never calls an AI model. It
records aggregate process CPU/RSS plus per-instance idle, conversation-switch,
and scroll costs. The default retains 6,000 messages across 30 conversations.

The script builds KetPlus in Release mode, runs the in-process editor workload,
the Qt Quick Agent Workspace workload three times, then measures native
application startup on macOS with empty, plain-text, generic-highlighted, and
language-highlighted files. Raw output is stored under `benchmarks/results/`.

The WebRTC data channel spike (ADR 0024) is a separate CMake project that the
product build never includes. It opens control and bulk channels directly or
through a TURN server, checks a bulk transfer's SHA-256 and reports control
round trips at idle and during the transfer:

```sh
cmake -S benchmarks/webrtc-datachannel -B build/webrtc-spike -DCMAKE_BUILD_TYPE=Release
cmake --build build/webrtc-spike
KETPLUS_SPIKE_TURN_URL=turn:user:password@127.0.0.1:3478 cmake build/webrtc-spike
ctest --test-dir build/webrtc-spike --output-on-failure
```

Without `KETPLUS_SPIKE_TURN_URL` only the direct path runs. `serve` with
`interop.html` repeats the exchange against Chromium's libwebrtc, and the iOS
spike in the private mobile app (a private mobile interoperability client) against Google's
WebRTC for iOS; `--bind <address>` serves a physical phone on the LAN. Findings
are in `docs/webrtc-transport-spike.md`.

## Metrics

For a synthetic streaming comparison in a Release tree configured with
`KETPLUS_BUILD_TESTS=ON`, run
`bash benchmarks/run-transcript-stream.sh build/release /tmp/stream.log`.
It compares the original direct QML update path with coalesced presentation in
the same binary, three samples each with 1,000/10,000 history rows. Each sample
delivers 300 prefixes at 20 ms intervals, checks lossless final text, and records
CPU time, wall time and actual model publications. Published character counts
are a work proxy, not measured parser bytes. Offscreen results do not establish
native frame rate, pixel equivalence, or a bound on giant-document layout stalls.
No provider or installed daemon is used.

Run `bash benchmarks/run-repository-metadata.sh` for a Release comparison of 100
warm SQLite metadata reads against 100 Git identity subprocess lookups in a
temporary repository. This isolates lookup cost, not application startup or
end-to-end Spotlight rendering; no provider is invoked.

- `set_text`: copies content into the document model and Scintilla.
- `apply_typography`: updates font size and line height, then restyles a 1 MiB
  highlighted document.
- `find_at_end`: scans a 10 MiB document for a match at its end.
- `replace_all`: replaces 145,636 whole-word matches in a 10 MiB document.
- `KETPLUS_STARTUP_MS`: time from the first statement in `main` to the first
  event-loop turn after the window is shown. It includes synchronous file load
  and lexing, but is not a first-painted-frame metric.
- `maximum resident set size`: process RSS reported by macOS `time`.
- `peak memory footprint`: physical footprint reported by macOS `time`.
- `AGENT_FIRST_FRAME_MS`: Agent Workspace construction through its first
  presented Qt Quick frame.
- `AGENT_POPULATE_1000_MS`: creation and first render of a 1,000-message mock
  transcript.
- `AGENT_IDLE_CPU_PERCENT` and `AGENT_IDLE_FRAMES`: steady-state cost after
  transient scrollbar animations settle.
- `AGENT_SCROLL_CPU_PERCENT` and `AGENT_SCROLL_FPS`: a reproducible two-second,
  120 Hz transcript scroll workload. `AGENT_SPLIT_*` repeats it at 520 px width
  to model an editor or terminal context panel occupying the other half.
- `DAEMON_RESTORE_RAW_EVENTS` and `DAEMON_RESTORE_COMPACTED_EVENTS` show the
  replay reduction for a 300-turn synthetic journal with streamed deltas.
- `DAEMON_RESTORE_REPLAY_PAGES` uses the byte-bounded 128-event page requested
  from a current daemon; unusually large events can lower the events per page.
- `DAEMON_RESTORE_MS` measures discovery, paged IPC replay, and transcript-model
  reconstruction. The workload is local and never starts a provider process.

The Agent Workspace benchmark raises and activates its window before measuring.
This avoids macOS suppressing scene-graph frames when the benchmark is fully
occluded by another application.

Keep summarized baselines as dated Markdown files. Keep raw files when a run is
used for a release decision or when investigating a regression.

## Feature workflow

1. Commit the feature so the measured source tree has a stable revision.
2. Run `./benchmarks/run.sh` on an otherwise idle machine.
3. Add a dated summary under `benchmarks/results/` and compare it with the most
   recent result from the same machine.
4. Investigate an editor-engine median regression above 15%, or a startup or
   memory regression above 20%, before starting the next feature group.

For Agent Workspace, investigate any non-zero steady idle frame count, idle CPU
above 2%, missed frames below 110 FPS on the 120 Hz reference machine, or a CPU
increase above 20% relative to its latest matching baseline.

Startup measurements on desktop systems are noisy. Confirm a suspected startup
regression with at least three complete benchmark runs.

## Transcript scroll metrics (M1 / M2)

`run-transcript-scroll.sh` measures what an upward scroll through a long
transcript actually does to the reading position. Reduced from the layout trace
by `transcript-scroll-metrics.py`:

- **M3 — path-to-net ratio. The success criterion.** Per uninterrupted scroll,
  the distance the content travelled divided by the distance it actually got. A
  hand capture of the reported symptom moved 381,302 px of content to advance
  9,521 px: a ratio of 40, so 97.5% of the motion the reader saw was wasted. No
  threshold, physical meaning, target 1.
- **M1 — counter-gesture frames.** Frames whose content offset moves opposite to
  the active gesture by more than 2 px, reported as episodes, total backward
  pixels and the largest single backward movement. A complement to M3: it
  localises the reversals instead of aggregating them. The `gesture`, `momentum`
  and `idle` phases are reported separately, because a fling that reverses and a
  page that moves while untouched are as visible as a gesture that reverses, but
  have different causes.
- **M2 — row re-entry. Secondary; not the success criterion.** How many times
  each `messageKey` enters the viewport. It was written as the literal encoding
  of "the same passage passes the viewport several times" and it does not work:
  it reads the same on a capture of the symptom as on a clean run, because
  content oscillating around a position never takes the row off screen, so the
  row never re-enters. Retained as a weak signal only.
- **Reverse drift by collapsed-row density.** Frames whose motion opposes their
  own scroll's heading, bucketed by how many zero-height collapsed rows surround
  the viewport, with the waste ratio per bucket. Collapsed-dense regions fail by
  drifting back a few pixels every frame rather than by jumping, so **rank frames
  by sign, never by magnitude** — magnitude ranking reported the exact inverse of
  the truth here, and it is what made M2 useless. Target: zero reverse frames in
  every bucket.

Extent stability (`contentHeight` reversals, largest step) and frame time remain
useful, but neither moved when the reader's experience did — that is the reason
these exist.

Per-frame records fill the default 8 MB trace budget in seconds. Raise it with
`KETPLUS_CHAT_LAYOUT_TRACE_MAX_MB` for a capture session; the app warns once when
a trace stops at the cap, since a truncated trace otherwise reads as a scroll
that settled.

Build (off by default: the benchmark needs a daemon journal to replay):

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
      -DKETPLUS_BUILD_BENCHMARKS=ON -DKETPLUS_BUILD_TRANSCRIPT_SCROLL_BENCH=ON
cmake --build build/release
```

Run — arms are always interleaved round by round, because a non-interleaved
batch is invalid the moment anything about the machine changes mid-session (a
display refresh-rate change has already invalidated one):

```sh
./benchmarks/run-transcript-scroll.sh \
    --journal /path/to/journal.sqlite3 --title-substr "Some Conversation" \
    --arm baseline=build/release/ketplus_transcript_scroll_benchmark \
    --arm candidate=../other-build/ketplus_transcript_scroll_benchmark \
    --rounds 3
```

The journal is the operator's own conversation data. It is copied into a
temporary state directory, never written back, and never committed; no path to
one is checked in.

The scroll runner selects the conversation's origin scope even when its execution worktree changed, and forwards live reading-key changes just like Desktop. A run with zero hydrated transcript rows exits with an error; it is not a successful zero-drift measurement.
