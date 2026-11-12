# 0015: Versioned provider CLI compatibility

Status: accepted (implemented)
Date: 2026-09-25

## Context

The daemon drives three agent CLIs, and none of them is version-checked today:

| Provider | Binary | Version source | Wire protocol |
|---|---|---|---|
| Codex | user-installed `codex` (ADR 0003: not bundled) | whatever is on `PATH` or a well-known path | `app-server --stdio`, JSON-RPC, `experimentalApi: true` |
| Cursor | user-installed `agent` (ADR 0003: not bundled), self-updating | same | ACP `protocolVersion: 1`, plus `agent models` text output |
| Claude | Agent SDK `0.3.281` with native Claude Code `2.1.281`, bundled | fixed per KetPlus release | SDK API inside the sidecar |

Codex and Cursor can therefore be older or newer than the build KetPlus was
developed against. Compatibility is handled case by case: a `thread/turns/list`
fallback to `thread/read` keyed on `-32601`, a visible error for
`thread/compact/start`, a Cursor `[fast=false]` model-value special case.
Unknown Codex server requests are answered with `-32601`. Renamed or removed
fields in Codex's experimental API are read as empty values without any error.
Users get "Codex initialization failed" rather than "update Codex".

The Desktop↔daemon protocol has the same problem in a smaller form: an exact
`protocol_version == 0` match plus ad hoc per-feature versions
(`transcript_window_version`, `transcript_large_event_version`).

## Decision

Support a declared range of versions per provider through capabilities and
per-version dialects, verified against stored protocol snapshots.

### 1. Detect the version

Before starting the long-lived process, the Codex and Cursor adapters run the
CLI's version command (`codex --version`, `agent --version`) with a 5 second
timeout (`providers/compat/ProviderVersionProbe`). The result is cached by the
executable's canonical path, size and modification time, so later launches check
without a new process and an update on disk is noticed at the next launch. A
probe that prints nothing is not cached.

- `outdated` fails the pending requests with a message naming the version, the
  minimum and the update command. The turn ends with the existing
  `runtime.error`, so Desktop shows it without new UI.
- `newer` and `unknown` start normally. The session journals one
  `provider.runtime` event (the runtime JSON: provider, status, version, range,
  update command, message; no prompt or account data) the first time a turn
  meets that provider/version/status in this daemon process. Desktop shows it as
  a muted "Provider version" notice.
- `supported` and `missing` journal nothing; a missing CLI keeps its existing
  launch error.

The daemon query `provider.runtime_status` returns every provider's runtime,
including Claude's bundled SDK and Claude Code versions from the sidecar's
`runtime.json`. Settings > Providers shows it.

### 2. Declare the supported range

One daemon-owned manifest,
`apps/daemon/src/providers/compat/provider-compatibility.json`, is compiled in
as a Qt resource. Per provider it lists:

- `minimum`: oldest supported version.
- `tested`: versions KetPlus was verified with; the highest is the newest tested.
- `update` and, for Codex, `update_homebrew`: the command shown to the user. The
  Homebrew command is used when the executable resolves into `Cellar` or
  `Caskroom`.

- `dialects`: `{id, from, capabilities}` entries. A version uses the dialect with
  the highest `from` it reaches; an unreadable version uses the newest dialect.
  Outdated versions get no dialect.
- Claude has `bundled: true` and a fixed `capabilities` list instead of a range.

Initial dialects: Codex `codex-app-server-v2` from `0.142.1`
(images, modelList, rateLimits, threadFork, threadRename, threadResume,
turnInterrupt, turnSteer); Cursor `cursor-acp-v1` from `2026.06.25` (images,
modelList, threadResume, turnInterrupt); Claude bundled (images, modelList,
threadFork, threadResume, turnInterrupt, turnSteer). `threadFork` was added
with ADR 0016. `threadCompact` is in no list because the daemon has
no compaction path yet.

Support status follows from the detected version:

| Status | Condition | Behaviour |
|---|---|---|
| `supported` | from `minimum` up to the newest tested version | normal |
| `newer` | above the newest tested version | run, show "newer than tested" |
| `outdated` | below `minimum` | do not start; show the update command |
| `unknown` | version could not be parsed | run, show a warning |
| `missing` | no executable | existing launch error |
| `bundled` | Claude | always the version KetPlus ships |

### Support window and cadence

- KetPlus supports CLI releases from the last three months. The manifest is
  reviewed monthly: `minimum` moves to the release about three months back and
  the newest verified release is added to `tested`.
- Initial values (2026-09-25): Codex minimum `0.142.1` (2026-06-25), tested
  `0.154.0`; Cursor minimum `2026.06.25`, tested `2026.09.18`.
- The bundled Claude Agent SDK is bumped monthly.
- Codex and Cursor stay user-installed (ADR 0003).

### 3. Gate features on capabilities, not version numbers

Adapter and Desktop code ask for capabilities, never compare version strings.
The names are `images`, `modelList`, `rateLimits`, `threadCompact`,
`threadFork`, `threadRename`, `threadResume`, `turnInterrupt` and `turnSteer`
(`providers/compat/ProviderCapabilities.h`). A provider's capability set is:

1. the dialect's baseline from the manifest (before the version probe has
   answered, the newest dialect's); then
2. narrowed by what the CLI advertises at runtime: ACP `initialize` preserves
   baseline `threadResume` only when `agentCapabilities.loadSession` is boolean
   `true`, and baseline `images` only when `promptCapabilities.image` is boolean
   `true`. Missing, null and wrong-type fields remove support; ACP v1 defaults
   both capabilities to false (ADR 0090). Codex `initialize` advertises nothing
   usable yet; then
3. narrowed for the life of the CLI process when a call is answered with
   `-32601`. A new process starts from the baseline again.

Capabilities are provider-level, not per session: every session of a provider
shares one CLI process. They are reported in `provider.runtime_status` as
`dialect` and `capabilities`. Uses today:

- The daemon refuses `session.steer` with `unsupported_provider_operation` when
  the provider lacks `turnSteer` (this replaces the Cursor name check).
- The daemon refuses `session.fork` with `unsupported_provider_operation` when
  the provider lacks `threadFork`, and Desktop disables Fork this conversation
  (ADR 0016).
- Cursor skips `session/load` and starts a new ACP session when it lacks
  `threadResume`; the journal still holds the transcript.
- Desktop (`agent/ProviderRuntimeStatus`) fetches the status when the daemon
  connection becomes ready and hides `/compact` and the usage popover's compact
  button when the provider lacks `threadCompact`. Until the answer arrives,
  optional actions are hidden. The test-only direct Codex transport is not
  gated.

### 4. Isolate wire differences in dialects

When a method, request or field changes shape between versions, a dialect
translator converts between that version's wire format and the adapter's
internal model. Dialects live in their own files
(`apps/daemon/src/providers/codex/dialects/`, `…/cursor/dialects/`), so the
adapters do not accumulate version branches.

No translator exists yet: every supported version shares one wire format, so
a dialect is only an id and a baseline capability list in the manifest. The
folder is created with the first real wire difference. The existing special
cases stay where they are because they are not version differences:

- The `thread/turns/list` → `thread/read` fallback and `thread/compact/start`
  live in the test-only direct Desktop transport; production goes through the
  daemon, which calls neither.
- Cursor's `[fast=false]` model value is a value Cursor advertises in
  `configOptions`, handled as data rather than as a protocol version.

### 5. Handle unknown input safely

- Unknown notifications are ignored and counted in diagnostics.
- Unknown server requests get a defined safe answer (decline) instead of a
  stall, and the turn shows that KetPlus could not handle the request.
- Unknown item types are rendered as a generic row with their type name, not
  dropped.
- Required fields missing from a known message are reported as a protocol
  error for that item, never read as empty values.

This applies to the Codex app-server stream, which `SessionHost` reads directly.
Cursor and Claude input is translated by their adapters into canonical events
first.

- `SessionHost` counts every Codex notification it does not act on, whether
  unknown or known but unused, per method in memory (at most 128 methods, then
  one `(other)` bucket). The counts are not journaled. `provider.runtime_status`
  returns them as `ignored_notifications` on the Codex entry.
- Unknown server requests are answered with `CodexServerRequests`'s decline
  result and journaled as `provider.request_declined` (`reason:
  unsupported_request`). Desktop shows a muted "Request declined" notice.
- The daemon journals items of any type unchanged. Desktop's formatter renders
  an unregistered type as an `unknown` generic row titled "Used <type>".
- Required fields: `turn/started` needs `turn.id`; `item/started` and
  `item/completed` need `item.id` and `item.type`; the agent-message and
  command-output deltas need `itemId` and a string `delta`. Ids and types
  must be non-empty. A notification that lacks one is skipped and journaled
  once per session and method/field set as `provider.protocol_error`
  (`provider`, `method`, `missing`). Desktop shows a muted "Protocol error"
  notice. `turn/completed` and `error` have no required fields, so a turn can
  always finish.

### 6. Verify against protocol snapshots

- Codex: `tests/providers/codex/extract-surface.mjs` reduces
  `codex app-server generate-json-schema --experimental` output to every
  request, notification and server request with its params and response field
  paths. The reduced `surface.json` for each `tested` version is stored under
  `tests/providers/codex/schemas/<version>/` (the raw schema is 5 MB).
  `tests/providers/codex/ketplus-usage.json` declares the methods and fields the
  daemon sends or reads. `contract.test.mjs` (ctest
  `ketplus_codex_contract_tests`) checks that every tested version has a
  snapshot, that the declared usage exists in each, and that every Codex method
  named in daemon code is declared.
- Cursor: synthetic `agent models` samples per tested build under
  `tests/providers/cursor/`; the ACP `loadSession` narrowing is covered with the
  fake agent.
- Fake providers exercise `-32601` narrowing and the capability gates. No
  provider quota is used.
- Adding a new CLI release means: generate its snapshot, run the contract test,
  add the version to `tested`, and either extend a dialect range or add a
  dialect.
- Only `0.154.0` has a snapshot. The `minimum` release is covered by the
  declared dialect only; add its snapshot when it is next reviewed.

### 7. Desktop↔daemon protocol

The envelope `protocol_version` stays the major and must still match exactly
(0). `daemon.hello` adds a minor: the client sends `protocol_minor` (Desktop
sends 1), and the daemon answers with the lower of the two plus `features`.
A client that sends no minor gets minor 0 and no features, so older Desktop
builds keep working. Minor 1 features are `provider.runtime_status`,
`session.fork` (ADR 0016), `transcript.window/1` and
`transcript.large_event/1`. The per-response fields
`transcript_window_version` and `transcript_large_event_version` remain for
minor-0 clients. Later minor-1 features are additive the same way; for
example `daemon.host_identity/1` adds `host_key_fingerprint` to the hello reply
(ADR 0119 "Machine identity"). `DaemonClient` exposes `protocolMinor()` and `hasFeature()`;
Desktop reports `provider/runtimeStatus` as unsupported when the daemon lacks
the feature. ADR 0003's rollover stays the path for major changes.

## Consequences

- Users see which CLI version they run and whether it is supported, with an
  update command when it is too old.
- Features degrade by capability instead of failing at the point of use.
- Each supported CLI version has a stored snapshot and contract coverage, which
  makes the support window a maintained, reviewable list.
- Adding a provider release is a routine change: snapshot, test, manifest.
- A version probe adds a short process launch before each provider start.
- Claude is unaffected by user installs but only gains models and features
  when KetPlus bumps the bundled SDK, which needs a regular bump cadence.

## Alternatives considered

- **Bundle and pin Codex and Cursor**, as with Claude. Exactly one version per
  release removes drift, but reverses ADR 0003, adds redistribution, update and
  licensing work (Cursor's CLI is proprietary), and still lags provider releases.
- **Version-string checks in adapter code.** Simple at first, but spreads
  version branches through already large adapters and cannot react to what the
  running CLI actually advertises.
- **Runtime probing only, no manifest.** Handles missing methods but not changed
  field shapes, and gives users no supported/outdated signal.

## Rollout

1. Version detection, manifest, support status, Settings display and
   `provider.runtime` journal event. Implemented.
2. Capabilities: manifest dialects, runtime narrowing, steer and Cursor resume
   gates, Desktop `/compact` gating. Implemented.
3. Codex surface snapshot, declared usage and contract test. Implemented; the
   dialect folder waits for the first wire difference.
4. Cursor ACP `agentCapabilities` narrowing and `agent models` fixtures.
   Implemented.
5. Desktop↔daemon minor negotiation and features. Implemented.

6. §5 unknown-input handling: ignored-notification counts, protocol errors,
   declined requests and generic rows for unknown item types. Implemented.

## Resolved questions

- Window: releases from the last three months, reviewed monthly.
- `outdated` blocks the provider; `newer` and `unknown` warn.
- The bundled Claude SDK is bumped monthly.
