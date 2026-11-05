# R03-E Bounded Transcript Paging Preparation

Status: the bounded giant-item daemon/local Desktop slice is implemented by ADR 0006.
Cross-turn continuation and the Backend/mobile hot/cold restore contract remain
blocked on interface freeze. READY below refers only to the remaining work.

## Immutable Inputs

- Desktop current base: `0d789192596d01a387d9dc69efae1ec53de13b25` (PR #19 squash).
- Desktop historical characterization build: `56749d0d74b9fb579cebae1803bbc8d4ba2b0fda`.
  The historical raw report remains unchanged and is not relabeled as a new build.
- Backend proposal branch `refactor/r02-control-vnext-contract`, fetched/read at
  **`e2a0844c5db717ff6ab0c700d123f4facc228d8c`**. Backend develop observed at
  `fef34104548917b7da0126faaf358382fa36fd8b`. Source worktree was not changed.
- Backend inputs at that exact proposal SHA:
  `contracts/ketplus-refactor-vnext/README.md`,
  `ketplus-control-vnext.proposal.openapi.json` in that directory,
  `fixtures/{manifest,conformance-fixtures}.json`, and
  `tasks/ketplus-refactor/wave-1/backend/audit-and-contract-handoff.md`.
- Proposal version is **0.4.0-proposal**, explicitly proposal-only; current v2
  OpenAPI is not replaced. Fixture manifest pins
  `d454b2825c7bdbf89c94d7ed7fc6222ecf13454fc10dcab178265d2d869415d3` for the conformance file.
- Desktop `tasks/desktop-refactor/02-api-alignment.md` is also proposal/audit, not
  freeze approval. Production symbols are traced in `production-characterization.md`.

## Implementation Readiness Matrix

| Concern | Current behavior | Proposed behavior (requires approval) | Owner | Prerequisite | Test evidence / implementation gate | Status |
| --- | --- | --- | --- | --- | --- | --- |
| Stable identity and revision | Journal scope is session/generation/cursor; turn increments on user.message; provider item IDs remain in payload | Durable conversation binding; canonical turnId/itemId scoped to conversation/generation; monotonic item revision and stable order; provider IDs only adapter references | Backend wire; Daemon/Core persistence; UI merge | Freeze ResourceScope mandatory conversation/session fields, turn allocation, revision authority, numeric cursor encoding | Current multi-item/reopen checks PASS; T1/T4/T6 must prove stable identity across restart | BLOCKED: scope fields are nullable in draft and revision allocation unspecified |
| Latest three turns with intra-turn continuation | transcriptPage limits three turns then 500 events/byte budget; truncated has no continuation | Select latest three turn identities at snapshot, traverse every item within them via opaque token before claiming that window complete; separate older-turn traversal | Daemon/Core query; Backend DTO; UI paging state | Freeze token scope/generation/snapshot/order/direction/expiry and meaning of snapshotComplete | v1 reproducer returns 500/514 with no older turn; Backend partial-turn-continuation-required; T1 | BLOCKED: token and window-completion semantics |
| Lossless >=8 MiB item | ADR 0006 stores <=32 MiB normalized JSON atomically, emits 96 KiB raw chunks plus terminal manifest, and local Desktop verifies bounded 128 KiB fragments before publish | Add authenticated hot-cloud fetch and cold `transcript.restore_window` mailbox re-upload for mobile | Backend fetch/mailbox; mobile consumer | Freeze cloud continuation/AAD/TTL/availability and mobile capability rollout | Exact duplicated production fixture reconstructs >=8 MiB; missing/conflict/gap tests pass | Daemon/Desktop DONE; Backend/mobile BLOCKED |
| Actual byte budget | Every raw chunk remains under the existing 256 KiB event cap; local IPC fragments are <=128 KiB; full item is <=32 MiB | Measure and freeze the encrypted cloud envelope and response-page accounting | Backend cloud encoding | Freeze what encodedBytes counts and envelope overhead | Local/raw event caps covered; encrypted upload remains not_verified | Local DONE; cloud envelope BLOCKED |
| Snapshot vs coverage vs cursor vs ack | Snapshot cursor reports tail even when truncated; compact gaps valid; ack prunes raw independently | Snapshot fixes read revision; coverage reports completeness of requested item/turn content; cursor orders raw transport; ack advances transport receipt only; rendered coverage belongs to client after body resolution/layout | Backend DTO; Daemon/Core retention; UI rendered state | Freeze partial-item coverage and snapshot retention/expiry; prohibit ack implying render completeness | Current ack/compact survival PASS; T1/T2/T4 | BLOCKED: draft CoverageRange only describes turns |
| Page/live overlap and duplicates | Raw append allocates new cursor for repeated payload; command-result dedup uses message ID; compact removes completed deltas | Validate scope/generation before batch mutation; merge by itemId/revision; same revision/same content idempotent, same revision/different digest explicit conflict; newer live revision wins | Backend semantics; Daemon/Core revision stream; UI selection epoch | Freeze conflict code, delta baseRevision, ordering and atomic batch rules | Backend pagination-page-live-overlap; current duplicate behavior PASS; T4 | BLOCKED: conflict/delta contract |
| Stale response and generation change | Journal rejects append to old generation; raw generation data stays separately scoped; selection race is client responsibility | Tokens and content references bound to generation; stale batches rejected; every callback (including error/finalizer) fences original scope/selection epoch; cancel old visible requests | Daemon/Core service; UI/mobile client owners | Freeze stale-generation/token errors and whether original inactive store may accept responses | Current generation/reopen PASS; T4/T6 | BLOCKED for implementation; reproduction READY |
| Cloud TTL and daemon offline | Hot content expires after24h; dev loopback bridge does not establish production history relay | Distinct retained, needs_mac, mac_offline, content_expired, unsupported, error; pending/timeout/cancel remain explicit; missing data never means exhausted | Backend relay/TTL; Daemon observe-only responder; UI availability | Freeze relay route, observation capability, request/response TTL, cancellation and availability on success | Backend mac-offline-vs-content-expired and relay-older-history-observe-only; T7 | BLOCKED: OlderHistoryRelayStatus lacks retained success availability |
| v1 compatibility and rollout | Current IPC v0/v1 transcript, no vNext negotiation | Separate negotiated transcript capability/version; preserve v1 requests; old peer gets bounded legacy + explicit incomplete state, never eager unbounded fallback | Backend + Daemon/Core negotiation; Integration rollout; UI | Freeze capability DTO, minimum versions, cursor integer/string representation, migration/rollback policy | Backend compatibility matrix; T8 | BLOCKED: exact capability DTO is an open decision |
| Command cloud policy | Production projector strips normalized command output, keeps full command/identity/status/nested exitCode; encrypted retry cache may bypass fresh projection | Only command/status/identity/exitCode reach cloud for command items; strip before encoding/encryption/reference allocation/outbox; no command-output chunk/reference route | Daemon/Core projector/outbox; Backend conformance; UI | Freeze canonical command kind mapping and retry-cache migration policy | Production serialized marker checks PASS; T5 must exercise new mapping and stored outgoing payload | READY: invariant; BLOCKED: vNext adapter and cache migration |

## Concrete Decisions For Backend

1. **Canonical identity/revision:** require non-null conversationId and sessionId on
   transcript requests after binding; assign turn/item IDs durably in daemon, preserve
   through restart; revision increments only on accepted semantic mutation. Choose
   safe integer constraints or versioned decimal strings for cursor/revision before
   clients implement int64/JavaScript conversion. Do not use title/path/turn as item key.
2. **Continuation and snapshot:** bind opaque token to full scope, generation, fixed
   snapshot revision, stable item order and position inside that turn/item. Prefer
   short-lived snapshot manifests referencing immutable item revisions over a SQLite
   transaction held open across network requests. Freeze snapshot lease/GC and
   `410 continuation_expired`; expiry requires a new snapshot with explicit incomplete
   coverage, never apparent end-of-history. Proposal fixture suggests 900s; not adopted yet.
3. **Large items:** prefer immutable item-revision storage + references, consistent
   with the proposal's oversized-item fixture. Freeze total logical UTF-8 bytes,
   whole-body SHA-256, chunk offset/length/hash, content version/MIME, authorized fetch,
   maximum chunk and envelope budget, lifetime and error codes. Proposed starting
   chunk size64 KiB is a test candidate, not a production limit. Transaction commits
   complete content/manifest then journal reference; crash before commit leaves no
   visible partial final item. Streaming revisions retain prior snapshot references.
   Transport fragmentation is the alternative only if Backend approves reassembly,
   duplicate/chunk ordering and durability semantics. Do not solve this by raising
   maximumEventPayloadBytes. Never create a cloud reference for command stdout/stderr.
4. **Byte accounting:** recommend encodedBytes count exact compact UTF-8 response JSON
   including metadata and encrypted/base64 fields; check the four-byte IPC prefix
   separately against the existing frame cap. Backend must approve per-transport
   interpretation. Account for encodedBytes' own decimal digits until stable, then
   verify final serialization; do not use source length or estimated envelope slack.
5. **Coverage:** define whether snapshotComplete means latest-window complete or
   entire available snapshot complete. Current newest-three fixture reports false
   despite complete latest turns, so these meanings must not be conflated. Add
   item/revision byte-range completeness or equivalent reference-resolution proof;
   a reference-only turn remains incomplete. Raw event gaps from compaction are legal.
6. **Merge and availability:** freeze conflict code and baseRevision for deltas;
   reject conflicting batches before mutation. Define relay responded+retained result
   (draft status enum currently cannot express retained) and distinguish definitive
   content expiration from Mac temporarily offline, token expiry and cancellation.
7. **Capability/migration:** approve vNext advertisement and per-surface unsupported
   errors. Keep schema migration transactional and resumable; define how incomplete
   historic v1 content is reported (already-rejected body cannot be recovered from
   a journal that never stored it). Canonical command projection/outbox migration
   must be specified before old cached envelopes are reused under a new capability.

Backend owns final wire schemas and conformance fixture hashes. Daemon/Core supplies
workload mappings and storage evidence; these suggestions are not another contract.
No ketjs extension is assumed or authorized if a database abstraction is missing.

## Implementation Test Plan

All new tests use fake clocks/providers, QTemporaryDir and synthetic SQLite only.
The former rejection/truncation reproducer is now a positive bounded-persistence
characterization. Keep mobile/cloud enablement capability-gated until its paired
consumer and hot/cold restore contracts ship.

| ID | Fixture and action | Required assertions | Current readiness |
| --- | --- | --- | --- |
| T1 | Single turn >500 retained events, multiple assistant/tool items; vary page count/byte budget; exhaust continuations | Every expected identity/revision exactly once; ordered hash equals complete source; token advances even within same turn; no turn-complete until all content covered; newest window and older traversal distinct | v1 reproducer READY; new token assertions BLOCKED on freeze |
| T2 | >=8 MiB Unicode item, chunked/ref storage; restart midway; reorder/duplicate/tamper/truncate/miss chunks | Exact whole-body bytes/SHA-256 reconstructed; offsets cover exactly once; hashes verified before final publish; atomic manifest visibility; missing/digest mismatch explicit, never silently truncated | Daemon/local Desktop PASS; cloud/mobile BLOCKED |
| T3 | Quotes, backslashes, control chars, combining characters, emoji; budgets just below/at/above boundary | Serialize actual IPC/cloud envelopes, assert every response within negotiated byte cap and frame cap; declared bytes equal measured bytes; reference metadata itself bounded | Synthetic generator preparation READY; wire byte definition BLOCKED |
| T4 | Page snapshot while50 deltas/s; live completion before older page; same/different-revision duplicates; crash/reopen; late callbacks after generation/scope switch | No old revision overwrites new; conflict rejected atomically; old-generation token/ref rejected; snapshot revisions available until expiry; no false coverage or ack advancement on failed body resolution | Workload and journal isolation READY; revised protocol BLOCKED |
| T5 | Command started/completed/failed, >100char Unicode command, output deltas/nested aggregates, outgoing references/cache | Actual serialized cloud/outbox payload contains no markers/output field/reference; full command/status/identity/exitCode exact; non-command assistant/tool text unchanged; test encryption boundary separately when wired | Existing projector assertion READY; vNext/outbox boundary BLOCKED |
| T6 |20 conversations,100 selections, overlapping cursor/item IDs in different generations; concurrent paging | Scoped storage/hash/count independent; wrong scope/token fails; late success/error/defer cannot mutate new selection; selected10k conversation fully traversable | Synthetic workload READY; consumer integration BLOCKED |
| T7 | Fake clock past24h, daemon offline then online, expired token, timeout/cancel and remote cache miss | Distinct availability/error codes; no missing range marked loaded/exhausted; read-only relay cannot start a provider turn; TTL content and control metadata lifetimes independent | Backend fixture intent READY; relay implementation BLOCKED |
| T8 | Old/new daemon/client matrix, interrupted migration/backfill, downgrade-compatible legacy read | Negotiated only; no unknown fields to strict v1 DTO; bounded incomplete legacy result; replay-safe migration, no payload limit increase; historic lost item remains explicit | Compatibility planning READY; version/capabilities BLOCKED |

After freeze: pin Backend schema/fixtures, implement storage/revision boundary first,
then bounded snapshot query/reference fetch, typed IPC mapping, projector/outbox
mapping, consumer merge, compatibility rollout. Integration owns common registration;
UI owns visible model/anchor behavior and native rendering evidence. Release headless
counts/bytes/timing remain separate from native frame/layout tests.

## R03-E Execution Prompt

> Continue Daemon/Core R03-E only after Backend and Desktop/UI publish explicit
> interface-freeze approval with immutable schema/fixture SHA and owners' decisions
> for identity/revision, continuation/snapshot coverage, large-item fetch, encoded
> bytes, availability, capability and command projection. Verify those artifacts;
> a draft PR or passing fixture checker alone is not freeze. If absent, report the
> unresolved decision and keep runtime untouched.
>
> Fetch latest develop; create an isolated implementation branch. Read AGENTS/docs
> and R03-D plan, preserve v1 characterization. Map Backend-owned conformance fixtures
> explicitly to current DTOs. Implement only approved durable item/revision storage,
> snapshot-bound keyset paging of latest three turns with intra-turn continuation,
> lossless approved large-item storage/fetch, exact envelope byte budgets, generation
> fences and capability-gated v1 compatibility. Keep raw cursor ack independent of
> content/rendered coverage. Strip all command output before cloud serialization,
> encryption, reference creation and retry persistence; retain full command metadata.
>
> Run T1-T8 appropriate to approved owning modules, then Release headless measurements
> with complete/failed/partial artifact validation and real source provenance. Record
> current v1 limits separately from vNext successful replay. Send common registration
> patches to Integration; request UI/native tests from their owner. No real provider,
> user journal, installed daemon, cloud, backend/mobile source, ketjs/ketatlas/vendor,
> deploy or merge. Deliver Draft PR, exact build/base/schema SHAs, invariant matrix,
> raw metrics and explicit remaining unsupported behavior.
