# R03-C Production Characterization

Current R03-D base develop: `0d789192596d01a387d9dc69efae1ec53de13b25`, the PR #19
squash merge. The branch merged this base without rewriting history; baseline #19
is no longer a new change in the PR diff. Historical R03-C base was
`73bddc13aab3425636fbd2f0a61860d95d13402c`, stacked on pre-squash #19 head
`c739042143af4666f0d820b17ae36ed6b0531c5d`. Characterization-only changes are
under `tests/daemon/characterization/`, two lines in `tests/daemon/DaemonProtocolTest.cpp`,
and this task handoff/evidence directory. The local merge does not merge either PR.

## Production Trace

Production symbols below are read at the develop revision above (unchanged by
either PR); the measured build/test revision is embedded in the raw report.

1. `apps/daemon/src/runtime/SessionHost.cpp::handleProviderNotification` maps
   `item/agentMessage/delta` to `assistant.delta` with `item_id`/`delta`, command
   output to `command.output_delta`, and item lifecycle to `item_id`, `item_type`,
   nested provider `item`, plus top-level text/command/status copies. Aggregated
   output remains inside the provider command item's `aggregatedOutput` field.
   `startTurn` appends `user.message`; this establishes journal turn numbering.
2. `SessionHost::appendEvent` calls `LocalJournal::appendEvent` before eventReady.
   `apps/daemon/src/storage/LocalJournal.cpp::appendEvent` checks encoded payload
   size, allocates cursor from the selected session/generation, increments turn
   on user.message, and updates raw plus compact projection transactionally.
3. `LocalJournal::updateReplayProjection` omits command output deltas and removes
   assistant deltas superseded by completion. `compactedEventsAfter` and
   `transcriptPage` read that projection. Cursor gaps are valid here.
4. Cloud upload is a separate branch from **raw** `eventsAfter`, not from compact
   replay. `apps/daemon/src/cloud/CloudControlClient.cpp::uploadSessionEvents`
   checks the durable encrypted retry cache, then invokes
   `apps/daemon/src/cloud/CloudEventProjection.cpp::projectEventPayloadForCloud`,
   serializes compact JSON, calls `crypto_.encrypt`, stores the envelope and
   requests `/v2/events:append`. A cached envelope bypasses fresh projection.

These tests call the production journal and projector directly. They do not invoke
the provider normalizer, crypto, network upload or cached-envelope migration.
The last boundary is source-traced only; encryption/upload remains **not_verified**.

## Adapter Scope

`tests/daemon/characterization/WorkloadAdapter.h` is a test-only adapter, not
canonical vNext. Workload message/user completion maps to `user.message`; assistant
message maps to `agentMessage`; assistant text_delta maps to delta. Command maps
to `commandExecution` with full command/status preserved. Payload top-level copies
match the current normalizer. Cursor and turn metadata are not imported into SQL:
the journal assigns them. The giant fixture receives one explicitly recorded
synthetic user prelude to establish turn 1.

The command fixture's separate aggregate record has no production event equivalent.
The batch adapter folds stdout into the next matching command completion's nested
`aggregatedOutput`. Only stdout with empty stderr and immediate matching completion
is supported. Standalone aggregate adaptation, unknown event/item/role and media
shape throw unsupported. This folding is verified by raw journal marker assertions.

| Workload | Production mapping in this PR |
| --- | --- |
| items-1k / items-10k | All events appended and compact-replayed; one selected conversation |
| giant-turn | 513 accepted events plus user prelude; final >=8 MiB item rejected by v1 |
| command-output-cloud-strip-marker | Four workload records become three normalized events; aggregate folded explicitly |
| multi-assistant-tool-items | Generic tool_result adapter unsupported; interleaved agentMessage/mcpToolCall characterization uses explicit production-shaped synthetic items |
| 20 conversations / 100 switches | Node workload only; UI selection operations not production journal APIs |
| stream overlap / stale / negatives | Node workload only; no invented page/live consumer adapter |
| rich Markdown/media | Media adapter explicitly unsupported; reference parsing only, no decode/render evidence |

## Invariant Matrix

| Invariant | Production symbol | Fixture / assertion | Result |
| --- | --- | --- | --- |
| Full command, Unicode, status, exitCode survive | projectEventPayloadForCloud | started/completed/failed, >180 characters, actual serialized JSON decoded and compared | PASS |
| Output absent from projected bytes, present in raw SQLite | projector + appendEvent/eventsAfter | output delta and nested aggregated marker, command actions | PASS |
| Non-command content preserved | projectEventPayloadForCloud | agentMessage/mcpToolCall with output fields; exact serialized equality | PASS |
| Adapter aggregate shape | adaptCommandScenario + production projector | fold into nested aggregatedOutput; raw contains marker, projected bytes do not | PASS |
| Unknown shapes fail explicitly | test adapter | unknown type, standalone aggregate and media throw | PASS |
| Newest three, older order, multi-item turn | transcriptPage | five turns with assistant/tool/assistant; latest 12 events, older 8, ascending cursor | PASS |
| Generation isolation and restart | appendEvent/eventsAfter/transcriptPage | generation 2 restarts cursor at 1; stale append rejected; generation 1 data remains scoped after reopen | PASS |
| Duplicate semantics characterized | appendEvent/storeCommandResult | same event payload appends twice; command-result key keeps first value | PASS |
| Ack is not rendered coverage | acknowledgeCursor/compactedEventsAfter | raw pruned after ack while compact replay remains | PASS |
| 1k/10k production replay | appendEvent/compactedEventsAfter | complete count, six events in newest three turns | PASS |
| Giant item bounded persistence | appendEvent/largeEventFragment | actual source >=8 MiB; 32 MiB normalized JSON bound; 96 KiB raw cloud chunks; 128 KiB reads | PASS (exact compact bytes restored) |
| Giant turn complete page | transcriptPage | user/start/terminal manifest only; manifest remains partial until its body is verified; no 500-event false continuation | PASS (local Desktop resolves before publish) |
| Encryption/upload end-to-end | uploadSessionEvents | source trace only | not_verified |
| Native frames/layout/WebView/RSS | no capture implementation | headless test only | not_measured |

## Findings And Decisions

The prior oversized-item rejection and false 500-event partial-turn result are
replaced by ADR 0006's durable bounded chunks plus terminal manifest. The production
fixture now reconstructs the exact normalized >=8 MiB payload from bounded SQLite
fragments. Direct append is not idempotent by payload;
command retry deduplication is a separate message-id API. No command-output leak was
observed for the tested normalized command shapes.

The cloud/mobile cold-history consumer remains a separate contract: the backend is
outbound-only and must use the encrypted idempotent `transcript.restore_window`
mailbox described in ADR 0006, never an inbound synchronous daemon proxy.

Backend decisions blocking bounded paging: stable item identity/order/revision;
per-generation opaque continuation including intra-turn/item splits; encoded byte
and item budgets; coverage/expired/unavailable ranges distinct from snapshot/ack;
page/live duplicate conflict and stale-response policy; capability negotiation;
command-output projection expectations and Backend-owned conformance fixtures.
No paging implementation starts before contract and cross-team interface freeze.

## Reproduction And Measurement Scope

```sh
node tests/fixtures/transcript-refactor/generate-transcript-workloads.mjs --materialize /tmp/r03-fixtures
cmake -S tests/daemon/characterization -B /tmp/r03-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_ROOT"
cmake --build /tmp/r03-release
R03_FIXTURES=/tmp/r03-fixtures R03_METRICS=/tmp/r03-metrics.json /tmp/r03-release/r03_characterization
git diff --check
```

The isolated CMake project compiles the unchanged production projector, journal and
protocol sources with Qt Core/Sql/Test. It does not build or start an installed daemon.
The existing DaemonProtocolTest gains a shared helper slot; root registration is
unchanged. Integration may run this isolated Release command in its serial CI patch,
after the two Node suites requested in `r03-c-baseline-resolution.md`. Hosted CI
coverage is pending; local PASS must not be interpreted as hosted execution.

Raw measurements include three sequential samples per scenario, no percentiles.
Each uses QTemporaryDir and a fresh SQLite WAL database with production synchronous
FULL settings. OS/disk caches are uncontrolled; this is not cold-start system evidence.
Timing excludes Node generation and fixture adaptation. Append timing includes SQL
transactions and projection maintenance; giant append includes the failed final
serialization. Replay timing includes JSON array serialization plus projector work;
the nested projector timer includes payload serialization. Byte counts are compact
UTF-8 QJsonDocument bytes (no newline); replay counts include brackets for every page.
The giant normalized payload repeats text at top level and inside item, matching
the normalizer's shape. Reopen includes initialization/backfill checks.

Compiler/Qt/platform/build SHA, seed and exact fixture hashes are in the raw JSON.
Local hardware: Apple M1 Pro, 32 GiB physical memory; macOS 26.2 arm64.
RSS is not sampled. Native viewport/frame/layout/WebView metrics remain not_measured.
Legacy restore was not run; it uses its own built-in fixture and has no measurement
association with these scenarios. No real provider or cloud endpoint is used.

## Recorded Release Run

Raw artifact: [r03-c-raw-release.json](r03-c-raw-release.json).
Clean build/test source SHA: `56749d0d74b9fb579cebae1803bbc8d4ba2b0fda`.
Historical evidence commit `b3c75eb256e767e353334c1ac54a23fa377254d5` added this report
without changing that build's sources. R03-D changes metrics tests/publication;
the historical report is preserved and is not relabeled with the current base/SHA.
AppleClang 21.0.0.21000101, Qt 6.11.1, Release, macOS 26.2 arm64, Apple M1 Pro,
32 GiB RAM. Fixture seed: `ketplus-transcript-refactor-wave1`, format v3. Full
manifest with exact hashes/byte counts is embedded in the artifact.

The executable exited 0: four test slots plus init/cleanup PASS, zero failed or
skipped (QtTest total: 6 passed). The shared new checks ran in the isolated target;
the full existing daemon protocol/service suite was not rerun. Node baseline checks
passed locally (18); hosted CI registration remains pending.

| Scenario | Append ms, samples 0/1/2 | Newest page ms, samples 0/1/2 | Compact replay + projection ms, samples 0/1/2 |
| --- | --- | --- | --- |
| 1k | 218.327 / 192.053 / 165.595 | 0.362 / 0.281 / 0.343 | 7.191 / 7.375 / 7.623 |
| 10k | 1620.011 / 1527.257 / 1590.887 | 2.990 / 3.268 / 3.122 | 73.794 / 69.732 / 71.295 |
| Giant (rejected final item) | 109.739 / 106.099 / 131.422 | 1.864 / 1.844 / 1.877 | 3.321 / 3.164 / 3.029 |

These are individual observations, not percentile estimates or native application
latencies. The giant numbers are not a successful >=8 MiB item replay benchmark.

R03-D hardening and new validation results: [review resolution](r03-d-resolution.md).
Implementation remains gated by [R03-E plan and execution prompt](r03-e-implementation-plan.md).
