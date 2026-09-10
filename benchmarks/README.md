# KetPlus benchmarks

Performance is measured after each meaningful feature group so regressions are
visible while they are still easy to diagnose. Results are only comparable when
the build type, machine, operating system, and benchmark protocol match.

Run the complete suite from the repository root:

```sh
./benchmarks/run.sh
```

The script builds KetPlus in Release mode, runs the in-process editor workload,
then measures native application startup on macOS with empty, plain-text,
generic-highlighted, and language-highlighted files. Raw output is stored under
`benchmarks/results/`.

## Metrics

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

Keep summarized baselines as dated Markdown files. Keep raw files when a run is
used for a release decision or when investigating a regression.

## Feature workflow

1. Commit the feature so the measured source tree has a stable revision.
2. Run `./benchmarks/run.sh` on an otherwise idle machine.
3. Add a dated summary under `benchmarks/results/` and compare it with the most
   recent result from the same machine.
4. Investigate an editor-engine median regression above 15%, or a startup or
   memory regression above 20%, before starting the next feature group.

Startup measurements on desktop systems are noisy. Confirm a suspected startup
regression with at least three complete benchmark runs.
