# Wave 1 Core/Transcript Audit

Scope: R03/R05/R06 preparation only. This package contains source audit,
synthetic workload generators, baseline harness notes, and handoff boundaries.
It does not refactor runtime code.

Base Desktop SHA after fetching `origin/develop`:
`7efc058d99b37f269fdebd5b69044fff32b84195`.

PR head reviewed before this follow-up:
`43bbcf6cb64b998af3e017ce3f6293229a43d229`.

`origin/develop` had advanced to
`73bddc13aab3425636fbd2f0a61860d95d13402c` during the follow-up. This slice
continues from the PR branch without rebasing or force-pushing.

Read-only refs checked:

- Backend current checkout: `8758ac983a1a521c7765458e7a7b4ed0f500e48d`
  on `codex/crm-dashboard-metrics`, dirty; not used as compatibility baseline.
- Backend `origin/integration`: `65f1e35fffffd299cfb4f93f4b24982e2b1bf807`.
- Backend `origin/develop`: `fef34104548917b7da0126faaf358382fa36fd8b`.
- Mobile current checkout: `e7deb9795f7e626cb1268d17e91a8d9d86f4d66e`
  on `main`.
- Mobile `origin/develop`: `fdd84d4fcdf94fd5e5427ca2ee8a99530a25f70f`.
- Mobile `origin/main`: `e7deb9795f7e626cb1268d17e91a8d9d86f4d66e`.

Files in this handoff:

- `audit.md`: architecture trace and gap list with file/function evidence.
- `baseline-report.md`: fixture/baseline record and measurement limits.
- `handoff.md`: proposed boundaries, dependencies, and next-owner asks.
- `tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs`:
  deterministic workload generator. It emits compact manifests by default and
  materializes replayable JSONL events/operations only when explicitly
  requested.
- `benchmarks/transcript-refactor-baseline.mjs`: raw-evidence wrapper for
  fixture generation and optional legacy daemon restore benchmarking.

Focused local checks used for this slice:

```sh
node tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs --self-test
node --test tests/fixtures/transcript-refactor/generate-transcript-workloads.test.mjs
node --test tests/fixtures/transcript-refactor/baseline-harness.test.mjs
node tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs \
  --manifest /tmp/ketplus-transcript-refactor-manifest.json
node tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs \
  --materialize /tmp/ketplus-transcript-refactor-materialized
node benchmarks/transcript-refactor-baseline.mjs \
  --out /tmp/ketplus-transcript-refactor-baseline.json
git diff --check
```

The command-output self-test validates only the generator-local expected
projection. It is not evidence that production `CloudEventProjection` C++ or
Backend projection is leak-free; wiring that conformance test remains a
Backend/runtime dependency after schema freeze.
