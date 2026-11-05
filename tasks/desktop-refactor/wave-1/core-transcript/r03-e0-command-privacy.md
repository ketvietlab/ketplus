# R03-E0 Command Privacy Boundary

**Overall privacy boundary: FAIL for legacy/unvalidated cached envelopes.**
Fresh normalization/encryption/upload and retry of freshly projected envelopes pass
the exercised invariants. This is not a declaration that all cached output is safe.

Dependency: open Draft PR #21 exact head
`79f802a1062936d469068f18bd121a0d9479c215`. R03-E0 starts from that exact revision
on `test/r03-e0-cloud-privacy`; develop verified at
`0d789192596d01a387d9dc69efae1ec53de13b25`. PR targets develop and inherits #21
until it merges; E0 additions are the cloud-privacy test directory, this handoff,
and the minimal transport seam. No metrics-hardening rerun or paging implementation.

## Production Trace And Test Boundary

Production symbols read at the dependency SHA (unchanged except request transport dispatch):

1. `CodexAdapter::notificationReceived` in `apps/daemon/src/providers/CodexAdapter.h`
   is emitted by a synthetic provider driver. The production connection in
   `runtime/SessionHost.cpp::SessionHost` invokes `handleProviderNotification`.
   Tests send item/started, item/completed and item/commandExecution/outputDelta
   with a synthetic thread ID, long Unicode command and output markers.
2. `SessionHost::handleProviderNotification` creates item_id/item_type/nested item
   and copies command/status/text, or maps delta to command.output_delta. Its
   `appendEvent` persists through `storage/LocalJournal.cpp::appendEvent` before
   eventReady. Tests inspect raw SQLite-replayed data and prove markers remain local.
3. `cloud/CloudControlClient.cpp::uploadSessionEvents` reads raw eventsAfter.
   It calls cloudEvent for each cursor. A matching turnSequence cache entry is
   appended directly to the outgoing batch; it bypasses fresh projection/encryption.
4. For a cache miss, `projectEventPayloadForCloud` whitelists command metadata;
   compact JSON becomes plaintext for `CloudCrypto::encrypt`. Production crypto uses
   AES-256-GCM with random nonce and Ed25519 signature over AAD/nonce/ciphertext/tag.
   `eventAssociatedData` binds workspace, session, generation, cursor, turn, event
   type and key/sender metadata. `LocalJournal::storeCloudEvent` persists the envelope.
5. `CloudControlClient::request` constructs HTTP headers and serializes the real
   POST /v2/events:append body. An injected QNetworkAccessManager captures those
   exact bytes at createRequest and returns synthetic QNetworkReply responses.
   The test parses the outgoing body, decrypts its content with production
   CloudCrypto and ephemeral test keys, then checks **plaintext**, not ciphertext
   substring absence. Exact key whitelists exclude output/reference fields.
6. A synthetic503 exercises callback failure/retry. Reopening SQLite and recreating
   the client with the same test key store gives byte-identical request content.
   Wrong-scope/generation acceptedThrough is ignored; matching202 ACK prunes the
   cache/raw records without allocating a new event cursor.

The only production seam adds a private friend test accessor and a non-owning
transport pointer initialized to the existing network_ member; request dispatch
uses that pointer. Default production transport/lifecycle behavior is unchanged.
The injected manager outlives the client and never delegates to Qt networking;
unexpected method/URL terminates the test instead of opening a socket.

`MemoryCredentials.cpp` replaces only CredentialStore method definitions at link
time in this isolated target. Production CredentialStore.cpp and OS keychain APIs
are not linked into the test core. Production CloudCrypto is compiled unchanged;
keys are ephemeral, generated into the test memory store. Reopen evidence is SQLite
and client recreation with retained test keys, not an OS-keychain durability test.

The fake provider starts at notificationReceived: the Codex JSONL subprocess parser
is not exercised. No provider executable runs. Tests call the private per-session
uploader through the accessor to avoid credential-enrollment/timer orchestration;
the actual uploader, request serialization, reply callback, encryption and durable
retry code run. No real credential, cloud authorization, TLS or deployed recipient
implementation is covered. This closes the local encryption/upload-body gap only.

## Invariant Results

| Path / invariant | Test | Result |
| --- | --- | --- |
| Started/completed/failed command metadata | freshUploadDecryptsToCommandMetadataOnly | PASS: full >180char Unicode command, item identity/type, status, exitCode and envelope scope/cursor retained |
| Delta/nested aggregate/stdout/stderr/outputRef absent from decrypted outgoing command payload | freshUploadDecryptsToCommandMetadataOnly | PASS: plaintext key allowlists; raw journal retains synthetic marker |
| Retry/reopen preserves exact envelope and semantics | retryAndReopenUseSameDurableEnvelope | PASS: byte-identical body after503/reopen, no added cursor/event; scoped202 ACK handles cleanup |
| Legacy cache with valid encryption but unsafe plaintext | legacyCachePrivacyViolationReproduced | **FAIL: decrypted outgoing envelope still contains command output** |
| Fresh missing session/generation | freshCryptoFailureAndMissingScopeDoNotUpload | PASS: no records, no request |
| Fresh key-store write failure and invalid signing key | freshCryptoFailureAndMissingScopeDoNotUpload | PASS: no request/envelope, raw event retained; no plaintext fallback |
| Wrong workspace/session/generation AAD, ciphertext/signature tamper | decryptRejectsTamperAndWrongScopeGeneration | PASS: production decrypt returns empty plus error |
| Cached wrong session/generation or tampered signature | poisonedCacheScopeAndTamperAreForwarded | **FAIL at uploader: forwarded unchanged**; production decrypt rejects each |
| Non-command assistant/tool payload | nonCommandPlaintextRemainsUnchanged | PASS: decrypted serialized bytes equal normalized raw payload, including text/output-like fields |

QtTest explicitly logs four XFAIL privacy/cache invariants (one legacy plaintext
leak, three cache validation variants). Its successful process exit means the
characterization reproduces current behavior; it is **not a passing security gate**.
Each reproducer first asserts the observed unsafe behavior and then labels the unmet
privacy invariant with QEXPECT_FAIL. These failures must remain visible in Integration
handoff and release decisions; do not remove/relax assertions to declare the boundary safe.

## Findings And Separate Remediation Proposal

**F1, privacy policy violation:** compatible cached envelopes bypass current
projection. Preconditions: a raw event remains and its cached envelope has matching
turnSequence, but was encrypted with output present. The synthetic fixture creates
such an envelope using production crypto, persists it through storeCloudEvent and
observes output after decrypting the actual outgoing request. This exposes command
output to an authorized decrypting recipient; it does not prove relay operators can
decrypt content. No real journals were inspected, so deployed prevalence is unknown.

**F2, cache integrity/scope validation gap:** cache lookup SQL key is not checked
against cached sessionId/sessionGeneration/content before upload. Synthetic mismatched
scope/generation and tampered signature are forwarded. Recipient-side production
crypto rejects them in the test. Remote acceptance/exploitability is not established;
the uploader's requested fail-closed invariant is nevertheless unmet.

Proposed remediation, requiring a separate reviewed change:

- Persist a projection-policy version and validated scope/key metadata with cached
  envelopes. Crypto content.version is not a projection-policy version. Unknown or
  legacy policy must be quarantined rather than blindly retransmitted.
- Validate cached metadata against the requested session/generation/cursor/turn and
  expected sender/key context before use. Define how signature/content corruption
  is detected and reported without plaintext fallback.
- Re-project from authoritative raw data only when retry/server idempotency permits.
  Do not blindly re-encrypt under an already accepted cursor: ciphertext changes may
  conflict with immutable relay retry records, especially after an ambiguous ACK.
  Coordinate reconciliation of acceptedThrough/conflicting existing envelopes before
  selecting a replacement/invalidation strategy. Preserve event identity and command
  semantics; never invent a new user/provider event to repair a retry.
- If raw source/key material is unavailable, report explicit unrecoverable/quarantined
  state. Do not upload unknown-policy cache or silently drop data. Migration must be
  crash-safe and cover restart, partially accepted batches and key rotation.
- After remediation, retain legacy/corrupt synthetic seeds and convert the explicit
  failing invariants into blocking privacy assertions with approved migration expectations.

No mitigation or cache/schema policy was changed in E0. Paging vNext remains blocked
on Backend freeze and is independent of these privacy findings.

## Reproduction And Integration Request

```sh
cmake -S tests/daemon/cloud-privacy -B /tmp/r03-e0-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_ROOT"
cmake --build /tmp/r03-e0-release
/tmp/r03-e0-release/r03_cloud_privacy_tests
/tmp/r03-e0-release/r03_daemon_protocol_regression
ctest --test-dir /tmp/r03-e0-release --output-on-failure
git diff --check
```

Requirements: Qt6.11 Core/Network/Sql/Test, OpenSSL3, CMake/Ninja, C++20. The isolated
core compiles current daemon sources and links test-only credential storage; the
second executable runs existing DaemonProtocolTest.cpp without duplicating its tests.
Its service test uses only a temporary local endpoint/state path and nonexistent
provider executable. No installed daemon is started/replaced.

Integration owns serial registration of these exact targets/commands after the
dependency/source SHA is present. Root CMake and .github/workflows are untouched.
Keep verbose privacy test logs containing XFAIL; a green CTest result does not erase
F1/F2. Hosted target execution is not claimed until a named run/log proves it.
RSS and native rendering remain not_measured. No metrics benchmark was rerun.

## Recorded Local Verification

Clean Release source/build SHA: `d7f88ea0b947352c8e143ebe031a3848e2dfe95f`.
The worktree was clean at configure, build and test time (`sourceDirty=false`).
Subsequent evidence-only changes do not change the compiled source. This build SHA
is distinct from the dependency SHA and develop base above.

Environment: macOS26.2 arm64, AppleClang21.0.0.21000101, Qt6.11.1,
OpenSSL3.6.3, Ninja, CMake Release. Both isolated executables built successfully.
The verbose CTest run completed both registered executables:

- CloudPrivacyTest: 9 QtTest entries passed, zero unexpected failures/skips;
  **four XFAIL assertions remain unmet privacy/cache invariants**, not security PASS.
- DaemonProtocolTest: 14 QtTest entries passed, zero failures/skips.
- `git diff --check`: PASS; vendor gitlink unchanged.

Raw verbose test output: [r03-e0-ctest.log](r03-e0-ctest.log), with only local
source/build paths normalized. Reported test durations are runner diagnostics,
not daemon performance measurements. These are local results; hosted execution
remains not_verified pending Integration registration and an actual run.
