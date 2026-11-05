# Baseline Report

## Source State

- Desktop repo: PR worktree for `ketplus-desktop`.
- Branch: `refactor/r03-r05-r06-core-transcript`.
- Base: `origin/develop` at
  `7efc058d99b37f269fdebd5b69044fff32b84195`.
- Runtime refactor: not performed.
- Provider quota: not used.
- User journals: not used.
- Running daemon replacement: not performed.

## Fixture Baseline

Generator:
`tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs`.

Default seed: `ketplus-transcript-refactor-wave1`.

Workloads covered by compact manifest:

- `items-1k`: 1,000 display items in one selected conversation for
  render/windowing stress.
- `items-10k`: 10,000 display items in one selected conversation for
  render/windowing stress.
- `20-conversations-100-switches`: 20 conversations and 100 deterministic
  selection operations.
- `giant-turn`: >500 events, one assistant item with >=8 MiB source when
  materialized.
- `multi-assistant-tool-items`: multiple assistant/tool items in one turn.
- `stream-overlap-duplicates-stale`: 50 synthetic deltas/s with page/live
  overlap, exact 20 ms offsets, duplicate page overlap, and stale selection
  response context.
- `rich-markdown-media-command`: Markdown/code/table/Mermaid/media and command
  lifecycle.
- `command-output-cloud-strip-marker`: command output marker fixture.
- Negative scenarios: cursor gap, conflicting duplicate cursor, and stale
  generation/selection response. These are deliberately invalid and separated
  from replay workloads.

The generator default writes metadata/hash only. Materialized JSONL is opt-in:

```sh
node tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs \
  --materialize /tmp/ketplus-transcript-refactor-events
```

The command-output marker is
`KETPLUS_COMMAND_OUTPUT_SHOULD_NOT_REACH_CLOUD`; self-test fails if the marker
appears in the generator-local expected cloud projection. This is not a
production `CloudEventProjection` C++ test.

Manifest accounting:

- `specificationHash` covers scenario metadata and expectations.
- `logicalCanonical` hashes sorted-key event JSON with one trailing newline per
  record.
- `exactJsonl` hashes materialized event JSONL byte-for-byte:
  `JSON.stringify(record)` plus newline.
- `operationsJsonl` hashes workload operations separately from transcript
  events.
- Large payload size is measured from the actual materialized body, not from a
  placeholder.

## Benchmark Harness

Harness:
`benchmarks/transcript-refactor-baseline.mjs`.

Default behavior:

- Generates a fixture manifest and records environment/build SHA.
- Exits nonzero when fixture generation fails or a requested benchmark fails.
- Records measurement limitations as raw JSON.
- Does not infer p99 from small samples.
- Does not treat offscreen FPS as native evidence.
- Rejects `--release-build-dir`; `benchmarks/run-daemon-restore.sh` currently
  owns its build directory, so this wrapper does not pretend to honor that
  option.

Optional Release daemon restore baseline:

```sh
node benchmarks/transcript-refactor-baseline.mjs --run-daemon-restore
```

This delegates to `benchmarks/run-daemon-restore.sh`, which builds Release
benchmark target `ketplus_daemon_restore_benchmark` and uses synthetic journal
data with a fake/nonexistent provider path. That legacy benchmark uses its own
300-turn synthetic journal; its results must not be attached to the new
1k/10k/giant/page-overlap fixtures until those fixtures are actually replayed
by the measured runtime.

## Measurement Status

| Metric | Status | Notes |
| --- | --- | --- |
| Fixture seed/hash | Ready | Produced by generator manifest; materialized files can verify exact JSONL bytes/hash. |
| first frame/content | Not measured | Needs native run against selected fixture. |
| switch latency | Not measured | Workload sequence exists; runtime instrumentation pending. |
| frame times/GUI stalls | Not measured | No native capture permission in this audit turn. |
| RSS app/daemon/WebView | Not measured | Harness records env; process measurement pending Release run. |
| model/delegate/cache counts | Not measured | Requires runtime counters in R05/R06 or test-only hooks. |
| parse/layout cost | Not measured | Current evidence is source trace only. |
| page bytes | Partially ready | Manifest records encoded bytes; IPC/backend page byte metrics pending vNext. |

Native capture permission, if granted, is recorded only as permission state.
Without a capture implementation and artifacts, native viewport capture remains
`not_measured`. No bypass was attempted.
