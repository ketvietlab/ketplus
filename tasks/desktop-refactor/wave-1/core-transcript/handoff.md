# Handoff

R03-C follow-up: [P2 resolution and Integration CI patch request](r03-c-baseline-resolution.md).
Fixture format v3 defines per-session/generation cursors and explicit operation ordering.
Local Node PASS is not hosted CI coverage; registration remains with Integration.

## Proposed Boundaries

- Journal authority remains daemon SQLite raw journal plus compact projection.
  Raw cursor acknowledgement is not rendered-history coverage.
- Canonical transcript page vNext should carry scope/generation, item
  identity, order key, revision, covered ranges, encoded byte count, and opaque
  continuation. `snapshotCursor` alone is only a snapshot boundary.
- Desktop R05 model should own a bounded transcript window with stable item IDs,
  item revisions, source-byte accounting, anchor pins, and page/live merge
  policy. It should not persist row indexes.
- Desktop R06 renderer should consume model roles, maintain block/tail parse
  caches with byte budgets, and keep command lifecycle as one row. Command
  output detail remains outside the cloud/mobile projection in this wave.
- Backend owns conformance fixtures and schema freeze. Desktop owns workload and
  scenario generators in this slice. After schema freeze, map these scenarios to
  backend-owned canonical fixtures by hash instead of creating a competing
  contract.

## Dependencies To Backend

- Transcript page vNext with continuation for oversized turn/item, covered
  ranges, encoded byte limits, and stale-generation rejection.
- DB-side keyset queries for retained events and bounded maintenance jobs.
- Observe-only older-history relay request/response if mobile must fetch
  Mac-held history after cloud TTL.
- Shared fixture hash/version and positive/negative tests for command-output
  stripping, multi-item turns, continuation, duplicate/stale responses, and
  expired/unavailable ranges.

## Dependencies To UI / Renderer

- Stable item/window roles from R05 before R06 block renderer begins.
- Anchor API that can express item key, text position/sample, pixel offset,
  follow-tail state, and pinned selection.
- Counters for loaded items/source bytes, layout documents, media cache bytes,
  parse/layout durations, delegate count, and warm transcript views.
- Clear Latest semantics when the tail was evicted: fetch newest, merge, wait
  for layout settle, then attach follow-tail.

## Observed vs Suspected

Observed:

- `TranscriptPage` v1 has `truncated` without continuation.
- Daemon-backed Desktop restore currently replays compact projection to tail.
- Desktop history loader still asks for 20 turns.
- Desktop model has no byte-bounded eviction.
- Assistant Markdown rendering reparses full body on every delta/text change.
- Backend integration `readTranscriptPage` filters/sorts retained rows in memory.
- Mobile cache/replay logic is `@MainActor`.

Suspected risk:

- Snapshot/ack cursor may be mistaken by consumers for complete rendered
  history coverage.
- Page/live merge will need revision/order conflict rules before vNext paging.
- Current key+offset anchor needs explicit behavior for eviction/latest/font
  changes across a bounded model.

Not verified:

- Native frame/RSS metrics for the new workloads.
- iOS physical-device performance and cache write cost.
- Native viewport stability after prepend/evict/resize/font changes in vNext.
- Production C++ `CloudEventProjection` command-output conformance against the
  new marker fixture.

## Review Resolution Matrix

| Finding | Resolution |
| --- | --- |
| Mobile audit inferred from reconnect preview instead of app path | Re-traced production `RootView -> LiveRemoteView -> HTTPControlPlaneClient/EncryptedSQLiteTranscriptStore -> LiveTranscriptProjection` at `ketplus-app origin/develop fdd84d4fcdf94fd5e5427ca2ee8a99530a25f70f`; preview path is now explicitly separated. |
| `loadOlder()` / live page merge after await not checked | Documented as observed production gap: `loadInitialTranscript` has a post-await guard, while `loadOlder()` and `refresh()` merge/persist without a post-await session/generation/epoch guard. |
| MainActor treated as race-proof | Documented that MainActor serializes synchronous access but does not fence logical races across async suspension. |
| Assistant index key unclear | Documented observed mobile `assistantIndexes: [Int64: Int]`, keyed by turn sequence, not item ID. |
| Harness returned success after generator failure | Harness now exits nonzero on generator failure, spawn error, missing executable, invalid args, or requested benchmark failure, while still writing a diagnostic report when possible. |
| Benchmark continued after fixture failure | Requested benchmark is marked `not_run` and skipped when fixture generation fails. |
| Status vocabulary unclear | Report uses requested/not_run/failed/completed and only exposes artifacts for completed steps. |
| `--release-build-dir` was recorded but not honored | Isolated harness now rejects the unsupported option; `run-daemon-restore.sh` remains owner of its build directory. |
| Native capture permission implied measurement | Report keeps native viewport capture `not_measured` unless a real capture implementation and artifact exist. |
| Published scenarios were not replayable | Every published workload now materializes event JSONL, operation JSONL, and expected JSON; negative scenarios are explicitly labeled invalid. |
| `summaryOnly` substituted for data | Removed from generated artifacts and covered by tests. |
| 50 deltas/s was descriptive only | Operations now include deterministic live event offsets at 20 ms intervals plus page overlap/stale response context. |
| Duplicate/stale cases lacked consumer context | Valid duplicate page overlap keeps the same event stream; stale response operation carries selection/generation context and expected consumer action. |
| Giant hash/byte accounting used placeholder body | Manifest separates specification hash from actual logical/exact JSONL payload hash and byte count; tests verify materialized bytes/hash and actual >=8 MiB body. |
| Command output self-test overstated production proof | Docs now state the self-test validates generator-local expected projection only. Production C++/Backend conformance remains a dependency. |

## Integrator Notes

- No production modules, QML runtime files, protocol schema, root CMake,
  gitlink, coverage, or `atlas.progress.json` were changed.
- If common benchmark registration is needed, integrator should add it in a
  serial patch. This slice intentionally keeps the harness as a standalone
  script.
- Backend remains owner of canonical wire conformance fixtures after schema
  freeze. The files here are workload/scenario generators and do not define a
  competing protocol contract.
