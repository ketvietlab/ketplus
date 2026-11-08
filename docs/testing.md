# Testing

## Default suite

The default suite is deterministic, headless, and must not consume model quota:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Every Pro/daemon ctest test runs with `QT_QPA_PLATFORM=offscreen`: `ketplus_tests_offscreen()`
(`cmake/OffscreenTests.cmake`) sets it for each test in `tests/` and `tests/daemon/`, over the
test's own environment and the shell's, so a test run never opens windows on the screen. A
test binary run directly needs it exported. This override does not cover Lite tests in
`core/`. The Qt build and Release
characterization run on macOS only; Ubuntu builds are paused to cut CI cost and
must be restored before a Linux release. The browser regression and design
evidence checks still run on Ubuntu. Private Windows CI remains deferred until
the required Qt 6.11 WebView development packages are available from Qt's
public repository. Pull requests into `integration` and branch pushes run no
CI; run the targets for the area you changed before pushing and list them in the
pull request. CI runs on promotion pull requests into `develop`, on pull
requests into `main` and on pushes to `main`; a newer commit cancels the
obsolete run. See `docs/release-process.md`.

## Target map

| Area | Build/test target | Primary coverage |
| --- | --- | --- |
| Desktop search and routing | `ketplus_spotlight_tests` | Spotlight file/Atlas search and repository routing |
| Live conversation discovery | `ketplus_desktop_session_discovery_tests` | Another client creates a session through a real local daemon/fake provider; catalog races, event coalescing, cross-workspace visibility and stable selection with a scripted daemon |
| Transcript file activation | `ketplus_transcript_file_opening_tests` | Text/source-line editor routing, MIME/content classification, external media/PDF/app handoff, encoded paths, symlinks and missing files; platform opens intercepted without launching apps |
| Repository navigation cache | `ketplus_repository_metadata_tests` | SQLite persistence, invalidation, unavailable cache |
| Conversation-owned worktrees | `ketplus_conversation_worktree_tests` | SQLite path map, hide/remove, turn-delta capture |
| Editor features | `ketplus_editor_features_tests` | CM 0.2.0 commands in the Pro shell: recent and closed tabs, Save All, command palette, Spotlight files and back navigation, split view, search and replace in files, file format controls |
| CM document modes | `ketplus_desktop_document_modes_tests` | Markdown defaults to Preview, floating mode control/menu synchronization, CM terminal search/clear/restart controls |
| Claude API key | `ketplus_claude_credentials_tests`, `ketplus_claude_credentials_settings_tests`, `ketplus_claude_sidecar_tests`, `ketplus_desktop_daemon_client_tests` | local IPC authentication, memory-backed secret store, fixed endpoint and network failures, SDK credential scope, background rotation, disconnect without credential replay, native form states; no live API or real Keychain |
| Storage preferences | `ketplus_settings_storage_page_tests`, `ketplus_desktop_daemon_client_tests` | Immediate detail saves and explicit size-target saves, independent revisions, old-daemon capability fallback, read-only loading, conflict/error reload, malformed policy, advisory usage failure, late replies, keyboard/accessibility, narrow/wide native layout; capability negotiation and no reconnect replay; real local-daemon bridge read/apply |
| Settings composition | `ketplus_settings_integration_tests` | Shared appearance persistence across Pro shell surfaces; Providers page |
| Atlas process/WebView | `ketplus_atlas_tests` | Atlas process and tab behavior; complete selected-port readiness, malformed/truncated banners, stop-time suppression and repair/restart with a synthetic CLI |
| Atlas shell integration | `ketplus_atlas_workspace_tests` | Desktop and Atlas composition |
| Agent Workspace | `ketplus_agent_tests` | transport, sessions, rendering, interaction, history, fork menus |
| Sidebar attention | `ketplus_conversation_attention_tests` | background blocking/async questions and confirmations, answer/expiry transitions and spinner lifecycle; fake provider only |
| Background attention metadata | `ketplus_conversation_attention_daemon_tests` | real local daemon and fake provider: async question updates the inactive row without hydrating its transcript |
| Desktop zoom | `ketplus_desktop_zoom_tests` | repeated Small/Medium/Large transitions, QWidget and QML geometry, dynamic rows, elision, multi-window appearance, monospace changes and settings draft merging |
| Interface font stack | `ketplus_interface_font_tests` | embedded KÉT Sans weights, Vietnamese glyphs, tabular digits, shared QWidget/QML resolver and editor mono |
| Streaming presentation | `ketplus_agent_streaming_tests` | delta coalescing, final/stop flush, scope and prepend fencing, scroll/Latest, skill refresh isolation |
| Transcript viewport | `ketplus_agent_viewport_tests` | offscreen formatting deferral, partial source, source pin while reading, prefix anchors |
| Daemon activity groups | `ketplus_daemon_activity_groups_tests` | summary privacy, output watermark, evidence barriers, bounded snapshot pages, compaction, restart, fork/delete isolation |
| Open activity group loader | `ketplus_activity_group_loader_tests` | single in-flight request, coalescing, upserts, close/reopen fencing, snapshot restart, explicit retry |
| Desktop activity group IPC/UI | `ketplus_desktop_activity_groups_tests` | authenticated summary transport, native Qt expansion, live append while open, collapse, full export and explicit raw read |
| Local transcript storage window | `ketplus_daemon_transcript_window_tests` | indexed bounded seeks, giant turns, snapshot/compaction fencing, current approvals |
| Desktop transcript windows | `ketplus_desktop_transcript_window_tests` | selected-only hydration, older pages/export, cache eviction, pure event projection, provider notices and unknown item rows |
| Conversation navigation | `ketplus_desktop_transcript_navigation_tests` | bounded initial read-ahead and cancellation before idle publication |
| Transcript catch-up | `ketplus_desktop_transcript_catchup_tests` | retained reader on return, revision refresh, cancellation, live buffering, overflow and retries |
| Transcript row window | `ketplus_transcript_window_model_tests` | delegate retention and reentrant range changes during history reset |
| Provider limits | `ketplus_rate_limits_tests` | Codex rate-limit parsing and state |
| Chat Markdown | `ketplus_chat_markdown_tests` | raw HTML escaping, bare HTTP(S) autolinks without changing code/existing links, whole answer visible, URL hover/open and source copy in light/dark transcript delegates |
| Chat Mermaid | `ketplus_chat_mermaid_tests` | Markdown and Mermaid integration |
| Long-lived provider stderr | `ketplus_provider_stderr_flood_tests` | Codex/Claude/Cursor fake processes each emit two 32-MiB floods with eight mandatory stdout round trips per flood; drain and 1-MiB newest-tail bounds are observed directly, then a normal call succeeds on the same process |
| Workspace boundary | `ketplus_workspace_tests` | Pro shell with CM explorer behavior |
| Async question lifetime | `ketplus_async_question_lifetime_tests` | turn expiry, session/generation isolation, late items and stale answers |
| Local turn acceptance | `ketplus_turn_acceptance_tests` | Raw/projection/state/configuration abort and ignored writes, deferred-FK commit failure, no rejected prompt publication, independent-reader visibility, repair/retry/reopen, duplicate result replay, counter/revision exhaustion, bounded oversized-payload refusal (ADR 0076) |
| Session/provider deletion | `ketplus_provider_deletion_tests` | Failed commit releases the transaction; ignored tombstone/work/history mutations roll back; retry/reopen preserves history, conversation number, cleanup work and unrelated sessions; shared provider threads and unsupported providers remain protected; query/mid-fetch ownership failures prevent fake-provider deletion, preserve pending work/history and recover after repair/restart; malformed/unknown ownership JSON blocks queued and local deletion for both source and retained sessions |
| Provider response identity/envelopes | `ketplus_provider_response_id_tests` | Fake-provider handshake barriers, typed id isolation, valid-response recovery, exactly-once callbacks and Codex oversized responses; numeric limits; malformed result/error shape, initialization refusal, outstanding-call isolation, duplicate replies and subsequent recovery; POSIX launcher cases skip on Windows |
| Provider version probing | `ketplus_provider_version_probe_tests` | Incremental stdout/stderr cap, excessive-output termination, exit status, timeout/EOF, retry/cache, distinct concurrent executables and same-path replacement; local helper only |
| Claude/Cursor framing | `ketplus_provider_framing_tests` | Coalesced real QProcess output, 16-MiB per-line boundary, aggregate valid frames, oversized complete/partial suffixes, failure drain and fresh-generation recovery; fixture process only |
| Malformed provider frames | `ketplus_provider_malformed_frame_tests` | Fake Codex/Claude/Cursor processes: initialize and pending calls, UTF-8/binary/control/JSON failures, suffix quarantine, once-only error callbacks, payload-free diagnostics and fresh-generation recovery; POSIX process fixtures skip on Windows |
| ACP capability defaults | `ketplus_provider_compatibility_tests`, `ketplus_daemon_cursor_adapter_tests` | Missing/null/wrong-type capability fields, boolean controls/reset and actual session/load suppression; fake Cursor explicitly advertises implemented features |
| Provider line scanning | `ketplus_provider_line_reader_tests` | Deep array/object streams, chunk/depth bounds, JSON escapes/Unicode, invalid metadata, dotted-key isolation, next-line/reset recovery and a fresh-child POSIX peak-memory bound; no live provider |
| Daemon protocol/storage | `ketplus_daemon_protocol_tests` | framing, journal, deduplication, approval expiry |
| Daemon challenge v2 (#85) | `ketplus_daemon_machine_challenge_tests` | relay conformance bytes/signature, strict schema and machine/key binding, no legacy retry or credential request on rejection, and real HTTP serialization/signing with an in-memory credential store |
| Journal usage retention (#128) | `ketplus_daemon_usage_retention_tests` | final usage samples, unchanged content/order, replay/page agreement, revision fencing, latest 20 actual turns including sequence gaps, raw-event acknowledgment, active/open/recent/question/approval/generation guards, bounded scan progress, transactional rollback and nonblocking writer contention with timeout restoration |
| Acknowledged delivery cleanup (ADR 0049) | `ketplus_daemon_ack_retention_tests` | Qt backlog ACK, row/byte budgets and actual prepared plans; slowest/new client and generation fences; queue fairness/reopen; NUL/oversized/binary values; capability deferral; atomic ACK/queue writes and rollback/retry |
| Cloud cache retention (ADR 0053) | `ketplus_daemon_cloud_cache_retention_tests`, `ketplus_daemon_cloud_privacy_tests` | ACK row/byte/reopen bounds; exact ciphertext receipts, pending resets, stale cursor/account callbacks and legacy migration; isolated invalidation; unsupported rows; actual prepared plans; ignored/aborted/commit rollback and busy restoration; atomic scoped recovery cursor, receipt failure through retry/reopen and credential failure through restart; transport fails closed on failed policy/turn invalidation |
| Journal metadata retention (ADR 0050/0052) | `ketplus_daemon_metadata_retention_tests` | completed deletion work GC, exact indexed round-robin pending pages, malformed targets and ignored completion writes; actual 256th command/audit backlog path and clock rollback; shared age/cap row/byte bounds; cursor reopen, malformed-row fairness and orphan generation guards; actual prepared query plans; abort/ignored-write/commit rollback, busy writer and missing byte-length capability |
| Staged attachment expiry/cancellation (ADR 0051/0054) | `ketplus_daemon_attachment_retention_tests` | actual upload-begin and explicit cancellation backlog paths, bounded candidates across 100k other-revision chunks, invalid-input side effects, manifest/chunk row and byte budgets, retirement/reopen/ID reuse, current/unknown parent protection, revision/generation isolation, actual prepared query plans, ignored/aborted writes, commit rollback and busy/capability deferral |
| Global payload GC (ADR 0048) | `ketplus_daemon_garbage_collection_tests` | Qt reopen/256th-command raw-backlog regression; actual indexed query plans and same-item replay backlog; metadata/chunk work and byte budgets; shared/generation fences, cursor restart/wrap, unsupported rows; abort/ignored-write/commit rollback; busy writer and WAL reader |
| Journal space (#128) | `ketplus_daemon_journal_space_tests` | the move to incremental vacuum keeps every row, drops the free pages and the copy is owner-only; an interrupted copy is discarded; an unreadable journal, or one open in an older daemon, is left as it is; new journals start on incremental vacuum; batched reclaim, the file shrinking while open, readers deferring the checkpoint, then the log emptying, without a wait, and the minimum; one daemon per journal |
| Economic context harness | `ketplus_context_harness_tests` | strict window validation, journal migration/inheritance, Codex start/resume/fork/reset, idle configuration fences, Claude inherited turn options; fake providers only |
| Browser provider wiring | `ketplus_browser_provider_integration_tests` | host routing instructions for direct/daemon Codex and new/resumed Claude/Cursor, prompt/image preservation, scoped approvals, disabled/damaged/missing runtime |
| Daemon Claude adapter | `ketplus_daemon_claude_adapter_tests` | sidecar turns, approvals, handoff |
| Provider handoff history | `ketplus_provider_handoff_tests` | history after delivery acknowledgment, compacted cursor gaps, event/byte-limited paging, session/generation isolation |
| Agent messaging (ADR 0027) | `ketplus_agent_messaging_tests` | number allocation, uniqueness, restart stability and legacy-journal backfill; idle delivery and automatic reply from the final answer; busy receivers batched at turn end; `urgent` steering of a running turn; hop, chain and hourly holds released with the next user turn, and the 24-hour request expiry; `#NNNN` references; token, number, size and reply validation; deletion quarantine and token revocation; per-provider `ketplus_agents` injection (Claude `mcpServers`, Codex dotted per-thread config, Cursor ACP); agent-token IPC scope without broadcasts; MCP tool mapping; `sent` and `replied` notices and the running automatic turn's sender in `session.list`; the number and that sender sealed into the phone's session metadata (`ketplus_daemon_cloud_privacy_tests`); Desktop notice text (`ketplus_provider_transcript_tests`) and the sidebar "from #N" label (`ketplus_workspace_tests`). Fake providers only; the one authorized live run is recorded in ADR 0027 |
| Conversation fork | `ketplus_daemon_session_fork_tests` | `threadFork` gating, Codex/Claude native fork, transcript copy at the same cursors, idempotent retry, active source isolation and unknown refusal, next turn resumes the forked thread |
| Daemon Cursor adapter | `ketplus_daemon_cursor_adapter_tests` | ACP turns, login/install errors, approvals, missing/false/mistyped `loadSession` skips `session/load`, `agent models` fixtures |
| Provider CLI compatibility | `ketplus_provider_compatibility_tests` | version parsing and statuses, manifest, Homebrew update command, version probe cache, outdated CLI blocked, untested CLI journaled once, `provider.runtime_status`, dialects and capabilities, `-32601` narrowing and steer gate, unknown Codex input (ignored notifications counted, protocol errors, declined requests, unknown item types) |
| Cursor catalog output | `ketplus_cursor_catalog_bounds_tests` | stdout/stderr draining before exit, shared 1-MiB boundary/excess, unsuccessful exit, coalesced callers/cache, timeout after output and failed launch; self-hosted CLI fixture only |
| Codex protocol contract | `ketplus_codex_contract_tests` (Node) | declared Codex usage exists in every tested schema snapshot; daemon code names only declared methods |
| Relay browser sign-in | `ketplus_daemon_browser_sign_in_tests` | relay paths appended to a base path, loopback redirect, PKCE S256 challenge, wrong state/path refused, one-time callback |
| Daemon lifecycle/IPC | `ketplus_daemon_lifecycle_tests` | authentication, protocol minor and features, reconnect replay, isolation, fencing; failed stream ordering, bounded recovery queues, deletion/shutdown and SQLite writer-lock retry |
| Daemon upgrade | `ketplus_daemon_upgrade_tests` | version advertisement, idle replacement, active-turn deferral |
| Desktop daemon attach | `ketplus_desktop_daemon_client_tests` | detached UI lifetime, discovery, normalized transcript replay, negotiated features, provider capabilities, fork through the daemon |
| macOS bundle layout | `ketplus_macos_bundle_layout_tests` | embedded matching daemon, loadable Qt runtime plugins, and absence of bundled Codex |
| Computer use host side | `ketplus_computer_tool_launch_tests`, `ketplus_computer_provider_integration_tests` | settings, host key permissions, token vector shared with the agent, remote-turn markers, server merge, Claude/Cursor injection by settings only (fake providers) |
| Computer use settings | `ketplus_settings_computer_page_tests` | opt-in disabled without the agent app, persisted toggle, deny list and work display, reverted toggle on save failure |
| Computer use agent | `cd apps/computer && swift test` | tiers and actions, token vector, key parsing, control lease and Esc revocation, geometry, owner-only socket, background click planning, stale references, Esc scope, keyboard target, menu shortcuts, scroll steps, work display choice and window placement |
| Standalone browser feasibility | `cd apps/browser && npm test` | Chromium observe/act fencing, node re-verification, proxy-enforced origin policy, secret redaction, structured comparison, JSON-RPC, and MCP discovery |

Run the relevant target while iterating. The manual compact-window picker has
a deterministic native/offscreen lane:

```sh
cmake --build --preset dev --target ketplus_agent_tests
QT_QPA_PLATFORM=offscreen build/dev/ketplus_agent_tests manualCompactWindow composerPreview selectsModelsInUnifiedPicker
KETPLUS_DISABLE_DAEMON_AUTOSTART=1 KETPLUS_COMPACT_WINDOW_CAPTURE=build/context-harness-mock/native-desktop build/dev/ketplus_agent_tests manualCompactWindow
```

The capture prefix's parent directory must exist. The test uses a fake catalog
and configuration receipts; it does not launch a model turn. It checks draft
Cancel/Apply/Auto, invalid input, revision-conflict Retry, foreground/background
locks, offline/old-runtime compatibility and keyboard focus/arrow/Escape. Its
15-state matrix covers 1000×900 and 420×480 windows in both themes and measures
popup/footer bounds. Platform renderer and DPR are recorded with captures.
These are framework key-event tests, not certification of OS foreground activation.

Example for the Atlas module:

```sh
cmake --build build/dev --target ketplus_atlas_tests
build/dev/ketplus_atlas_tests
```

Run the full suite for changes to CMake, shared UI, application composition,
`core/`, or any cross-module interface.

Claude usage/budget regressions additionally run `ketplus_claude_account_usage_tests`
(Admin storage separation, pagination, cents-to-USD conversion, validation, cache,
rotation and sanitized authorization failure), sidecar `session-usage.test.mjs` and
`usage.test.mjs`, the daemon Claude adapter suite, and native workspace/settings suites.
These use fake SDK/network/storage only. The native panel test checks distinct query
budget/session total/account total and captures `/tmp/ketplus-claude-usage.png`.
Live organization reporting requires the user's separately configured Admin key; tests
never use a real key, Keychain item or inference quota.

Atlas tunnel completion (`ketplus_atlas_live_tunnel_tests`, ADR 0084) exercises
per-frame, half-window and delayed full-window credits against a synthetic
loopback origin. It requires complete binary response bytes and exactly one EOF
without a final credit grant. Run the runtime, cloud-privacy and remote-control
client targets alongside it; no real CLI/provider or relay is required.

Daemon Atlas readiness (`ketplus_atlas_live_runtime_tests`, ADR 0083) includes a
Unix synthetic Python CLI: wrong/longer port, embedded/partial/truncated banners,
LF/CRLF/slash/split compatibility, stop-time output and a one-shot start callback
when the process exits immediately after readiness. Invalid-output repair starts
a fresh owned runtime. These cases do not opt into a real CLI, renderer or
provider. Header changes require the full Pro matrix; keep the existing real
KetAtlas lane opt-in and report its skip separately.

## Minimum verification

Update-preparation changes run `python3 tests/packaging/test_update_archive_cli.py`
for invalid notes/version, output collisions and repair/retry using synthetic
Apple/archive/appcast tools. No signing key or network is used.

Local IPC receive-buffer changes run the lifecycle loopback flood and resume
regression before/after hello, plus the directly affected protocol, headless,
credential, upgrade and desktop IPC targets. The fixture uses valid exact-limit
frames and measures actual unread bytes, rather than only checking a configured
property; no provider account is required (ADR 0077).

Changes to macOS signing also run `python3 tests/packaging/test_signing_cli.py`.
These fixtures stub signing/Apple tools and never use a private key or notarization
account. A replacement artifact additionally needs real Developer ID authority,
Team ID, certificate-based designated requirement and strict nested-signature
verification. Run the signed daemon `--build-info` and bundled Node `--version`.

Local image-activity regressions cover retaining viewed/generated PNG bytes after
source deletion, relative paths and unavailable/oversized files in the daemon
protocol suite. The daemon lifecycle suite reads a viewed image through the
session media API after cleanup and rejects another session's read. The Agent
Workspace suite renders both retained activity types to `Image.Ready` in native
QML after deleting their source path. Mobile's `SentImageTests` verifies the same
reference envelope is consumed without treating the Mac path as a phone file.

The Linux headless lane uses `-DKETPLUS_EDITION=pro -DKETPLUS_BUILD_DESKTOP=OFF`; it runs all daemon
tests and requires no provider account or display. The new focused targets are
`ketplus_headless_daemon_tests` (zero-session catalog, local auth, bounded
credential admission/preservation/repair at daemon startup and client hello,
isolated real daemon/CLI, SIGTERM), `ketplus_device_authorization_tests` (PKCE, polling,
cancellation and expiry) and `ketplus_remote_control_client_tests` (host/client
E2EE, key-bound pairing, refresh recovery, immutable command retries and signed
results). The normal full Desktop suite still covers the shared build/interface
changes. Also run the standalone Release cloud-privacy characterization, then
stage/install and check the runtime outside the Qt SDK; see
[`linux-headless-daemon.md`](linux-headless-daemon.md).

The worktree-context lane is isolated: `ketplus_daemon_worktree_context_tests`
uses temporary repositories/journals and a fake Codex process;
`ketplus_daemon_context_relay_tests` uses production relay encryption/serialization
against in-memory and private loopback relay fixtures, with in-memory credentials. It never
attaches to the user's running daemon or relay. Do not install/restart the app or
enable live-provider flags when running this lane. UI changes require updating
the corresponding Atlas mock first.

- Documentation-only: `git diff --check`; validate any edited YAML/JSON.
- Multirow journal append changes: the daemon protocol fault fixture verifies
  complete/partial ignored writes, rollback, repair and reopen; activity tests
  also exercise running-turn lookup plans over 100,000 historical rows.

- Journal startup changes: deferred commit-failure fixtures cover turn/replay/
  activity backfills, unchanged stored rows, diagnostic preservation, write-lock
  release while the failed object remains alive, repair and reopen.

- Journal migration copy protection (ADR 0087): the journal-space target observes
  a synthetic 64-MiB copy while SQLite writes under umasks 0022 and 0077. It checks
  private staging, final POSIX permissions, retained hashes, integrity, bounded
  stale-artifact cleanup, preservation of unknown entries and repair/retry.
  POSIX-specific cases skip Windows; this is not Windows ACL or multi-GB evidence.

- Media cleanup changes: temporary reference catalogs exercise malformed data,
  file/directory symlinks, unsupported filenames, per-file/catalog limits, preserved
  shared bytes and metadata, repair/retry and absent/empty-catalog compatibility.

- Media dedup changes: attachment/inline-item fixtures cover valid files, corrupt/
  short/oversized bytes, directories, symlinks, unreadable files, owner-only
  permissions and repair/retry without changing unsupported entries.
- Media publication changes: attachment/inline-resource/local-image fixtures
  verify unsupported ownership preservation, no false publication, dedup and
  shared repair/retry. An isolated POSIX child with a file-size limit checks real
  partial-write rollback; other platforms explicitly skip that one fixture.
- Fork media changes: fake-provider tests cover invalid/unsupported source and
  target metadata, retained conflicts, rollback, matching retry, missing-source
  compatibility and reopen cleanup following a deferred journal commit failure.
- Reference-preview changes: `ketplus_daemon_media_preview_tests` exercises
  canonical references, ownership bounds/types/links, blob identity, byte limits,
  unchanged invalid data and repaired JPEG output; no UI or live media is used.
- Tombstone catalog pagination: `ketplus_provider_deletion_tests` covers actual
  keyset plans, row/byte limits, removed cursor rows and malformed storage;
  `ketplus_desktop_daemon_client_tests` covers real local IPC with 30,000
  tombstones, legacy fallback, malformed pages, fresh live-catalog ordering and
  stale reconnect callbacks. Opt in to 100k/300k synthetic deletion/catalog
  probes with `KETPLUS_RUN_DELETE_BACKLOG_TESTS=1`; they use temporary journals.
- Staged fork copy (ADR 0094): `ketplus_daemon_session_fork_tests` covers one
  read snapshot while the source changes, byte-exact malformed TEXT, interrupted
  copies recovered as purge work at open, ignored purge/staging writes on abandon
  and at open, duplicate destinations and source
  deletion during a host fork. Opt in to 100k/300k timing with
  `KETPLUS_RUN_FORK_BACKLOG_TESTS=1`; begin, each pass and finish must stay
  under 250 ms.
- Bounded history purge (ADR 0093): `ketplus_provider_deletion_tests` covers an
  ignored purge marker, tombstone retention while purge is pending, a stalled
  pass, an ignored marker delete, event-loop draining and reopen. The opt-in
  backlog probe asserts the delete command stays under 500 ms and each purge
  pass under 250 ms. `ketplus_daemon_cloud_privacy_tests` covers 413 retry, late
  202 and a fresh upload of a deleted generation's residual history.
- Live-only discovery (ADR 0092): run provider deletion, Desktop daemon client,
  Desktop session discovery, lifecycle and headless CLI targets. Real temporary
  daemons with fake providers cover 20k/100k retained tombstones and zero repeated
  deletion signals; scripted peers cover legacy/paged fallback and replay gaps.
  Server tests prove tombstone tables are not read and retain the wire limit.
- One Pro module: focused target plus its directly affected tests.
- Shared header or desktop shell: full build and full Pro test suite.
- Client transport-buffer changes: `ketplus_daemon_client_upgrade_policy_tests`
  observes unread bytes with dispatch paused before/after hello, then verifies
  exact-limit pipelined frames and subsequent delivery without reconnect.
- `core/` change: the Lite suite (`ctest --preset lite-dev`), then the full Pro suite.
- Git/workspace symlink fixtures use native filesystem symbolic links, including
  directory and dangling links. `QFile::link()` creates Windows shortcuts and
  must not substitute for these fixtures. Git fixtures set repository-local
  `core.symlinks=true`; inability to create a real link fails setup rather than
  silently skipping the boundary assertion.
- QML layout or native rendering: offscreen tests plus the relevant native
  screenshot/pixel check documented in `agent-workspace.md`.
- Performance-sensitive behavior: Release build and the matching script under
  `benchmarks/`; record hardware and raw results.

Browser and SDK wiring changes also run:

```sh
(cd apps/browser && npm test)
node --test apps/daemon/providers/claude-sidecar/test/browser.test.mjs apps/daemon/providers/claude-sidecar/test/transcript.test.mjs apps/daemon/providers/claude-sidecar/test/input.test.mjs apps/daemon/providers/claude-sidecar/test/tasks.test.mjs
```

Computer-use changes also run `(cd apps/computer && swift test)`. TCC grants
cannot run in CI. After changing the agent, run `apps/computer/scripts/e2e.mjs`
on a Mac against a `--testing` build and the fixture app (see
`apps/computer/README.md`). It covers background input under a covering window
and foreground input on the work display. With two displays connected it also
checks that granted apps move to the secondary display. Use `--manual-esc` when
the kill switch or Stop changed.

The sidecar test uses a temporary fake SDK, never an authenticated model. The
browser suite includes reset-after-denial socket regressions and read-only engine
readiness checks. CI installs pinned Playwright Chromium for this lane.

The economic context harness also runs fake-SDK tests with no provider quota:

```sh
node --test tests/daemon/claude-sidecar/compact-window.test.mjs tests/daemon/claude-sidecar/usage.test.mjs tests/daemon/claude-sidecar/background-tasks.test.mjs tests/providers/codex/contract.test.mjs
```

They check Claude's per-launch flag on new/resumed queries, inherited environment,
invalid input refusal, background-task preservation, and the resolved threshold
remaining separate from physical capacity. Wire assertions do not demonstrate
native threshold resolution, boundary-only compaction, actual cache hits or cost
savings; those require an explicitly authorized provider experiment.

A 2026-10-07 metadata-only check against native Codex CLI 0.160.0 accepted compact
config on start and successive resumes (272k then 500k). An empty generated
rollout was flushed with archive/unarchive before resume, then archived and
removed. No `turn/start` or model request was sent. This does not verify the
provider's unreported resolved threshold or actual compaction behavior.

## Opt-in tests

Authenticated and native tests are excluded from routine automation:

```sh
KETPLUS_RUN_LIVE_AGENT_TEST=1 \
  build/dev/ketplus_agent_tests liveCodexSmoke

KETPLUS_RUN_LIVE_DAEMON_TEST=1 \
  build/dev/ketplus_daemon_lifecycle_tests liveCodexDaemonSmoke

KETPLUS_RUN_LIVE_DESKTOP_DAEMON_TEST=1 \
  build/dev/ketplus_desktop_daemon_client_tests liveDesktopDaemonSmoke

KETPLUS_NATIVE_ATLAS_TEST=1 \
  build/dev/ketplus_atlas_tests nativeAtlasSmoke
```

The live Codex test consumes provider quota and requires explicit user intent.
Do not enable it merely to obtain broader coverage. Native UI checks require an
interactive graphical session and must not replace deterministic tests.

Provider transcript changes also run `ketplus_provider_transcript_tests` and
`ketplus_claude_sidecar_tests`. The latter runs Node's built-in test runner with a
local fake SDK; it never authenticates or consumes provider quota. It covers SDK
snapshots/deltas, tool results, AskUserQuestion, interrupting a pending tool
permission, and allowing `ketplus_agents` tools only when the daemon injected the
server. Conversation navigation tests cover background approvals followed by
Confirm or Stop. Cursor adapter tests cover provider-specific permission option IDs
and cancellation while a permission is pending.

The September 2026 local Release verification has a known baseline in
`ketplus_agent_tests` (14 existing UI assertions, including typography and
interaction expectations). Compare failure names against the pre-change run;
provider/daemon and transcript navigation suites must pass independently.

For a synthetic native zoom capture (no provider calls):

```sh
QT_QPA_PLATFORM=cocoa QSG_RHI_BACKEND=metal \
KETPLUS_DISABLE_DAEMON_AUTOSTART=1 KETPLUS_ZOOM_CAPTURE=/tmp/ketplus-zoom \
  build/dev/ketplus_desktop_zoom_tests fontAndGeometryFollowZoomWithoutCompounding
```

This checks the same geometry assertions on the native platform and writes
sidebar/chat captures at each zoom size. Inspect text clipping, spacing and
control alignment at Small and Large as well as the unchanged Medium baseline.

### Execution worktree UI lane

`ketplus_daemon_claude_worktree_tests` checks Claude capability/availability,
same-thread A → B → A continuation after host restart, stale revision refusal,
and turn/approval/background-task fences. Run the sidecar `resume.test.mjs` and
`tests/daemon/claude-sidecar/background-tasks.test.mjs` too: they cover newest-history
copying on a return visit and refusal to terminate tasks for a cwd change.
These use fake providers and temporary Git repositories; they do not test an
installed iPhone or consume provider quota.
The desktop daemon-client target also builds the daemon executable: discovery
checks that this binary exists even when the test runs its service in-process.

Build `ketplus_execution_worktree_ui_tests`, `ketplus_daemon_worktree_context_tests`, `ketplus_daemon_context_relay_tests`, and `ketplus_desktop_daemon_client_tests` in the isolated context worktree. These cover the 1,248-entry paged picker, usage attribution, signed encrypted read/switch commands through a fresh loopback HTTP relay, and stable desktop conversation identity across switch/reconnect. Fake providers only. The dialog test captures `/tmp/ketplus-worktree-desktop-ui.png`; this is a widget interaction artifact, not production theme/device parity evidence.

## Atlas live isolated lane

Build `ketplus_atlas_live_tunnel_tests`, `ketplus_atlas_live_runtime_tests`, and `ketplus_atlas_tunnel_fixture`. Run the first two with CTest; set `KETPLUS_KETATLAS_CLI` to a private KetAtlas 0.5.2 executable for the real-runtime test (otherwise that case explicitly skips). No provider invocation, credentials, user daemon or running desktop app is used.

The codec/tunnel suite checks tampering, replay, direction/channel binding, credit-window crossing, binary EOF integrity and unregistered-origin refusal. The runtime suite starts a temporary project and renderer, verifies live reads after changing source, reattaches with fresh keys and stable source/ports, and checks process cleanup.

The separate relay checkout has `apps/ketsuite/atlas-gateway/native-fixture.mjs`, which launches only the fixture executable. The iOS test `nativeAtlasBridgeTransfersBinaryPostSSEAndWebSocketThroughIsolatedDaemonAndRelay` exercises the real Swift bridge and C++ tunnel through that gateway. Keep its descriptor in a temporary directory and stop the fixture after testing. The native worktree XCUITest uses a new disposable simulator; it is a UI fixture test, separately from daemon/relay integration.

Confirmation questions with one single-choice Yes/No (or Có/Không) pair submit the
original option ID on one click. Other questions retain their choice/text flow.
Free-text-only questions have no synthetic Other option. Test the card with
`QT_QPA_PLATFORM=offscreen qmltestrunner -input tests/qml`.

### Claude turn completion and response preferences

`node --test tests/daemon/claude-sidecar/steer.test.mjs` includes a fake SDK
that returns a final result but leaves output open after prompt input closes.
The turn must complete once, while accepted pending steering still receives an
answer. It also covers queued/stale/task-notification results during startup,
synthetic no-response frames, prompt UUID matching, explicit startup errors, and
visible failure on EOF or a bounded wait without an answer. These use a fake SDK
and consume no provider quota. `ketplus_daemon_worktree_context_tests` verifies that the response
preferences reach the Codex developer instructions once, stay out of start and
steer input, and stay out of the journal; `ketplus_browser_provider_integration_tests`
checks that Claude, Codex and Cursor host instructions contain them exactly once. Claude/Cursor adapter fixtures retain the full provider prompt in
their logs and strip the suffix only when dispatching test commands.

### Claude background tasks

`node --test tests/daemon/claude-sidecar/background-tasks.test.mjs` runs a fake
SDK whose first turn starts a background shell. The turn must complete while the
query stays open, the next user turn must reuse that query, stopping a turn must
leave the task running, and `task/stop` must end with the agent's own reply turn
before the query closes. `apps/daemon/providers/claude-sidecar/test/tasks.test.mjs`
covers the `tasks.changed` snapshot: level replacement, ambient and foreground
filtering, progress throttling and orphaning.

`ketplus_daemon_claude_adapter_tests` (`reportsAndControlsBackgroundTasksAcrossRestarts`)
checks the daemon side with a fake sidecar: malformed rows are dropped, `session.list`
carries `background_tasks`, `session.task.output` returns at most the last 8 KiB,
unlisted and ended tasks are rejected without reaching the provider, and a restarted
daemon reports a task that was running as stopped with reason `worker_restart`.
`ketplus_agent_tests` (`backgroundTaskBar`) covers the desktop bar: confirmation
before Stop, the output pane ignoring answers for another task, a refresh keeping the
shown tail, the pane closing when its task leaves the report and a worktree switch
clearing the bar.

## Desktop transcript Atlas alignment verification

Validated with synthetic fixtures, without provider calls or replacing the running app:

- `ketplus_agent_tests`: focused history/live normalization, renderer, command details,
  copy, grouping, paging and anchor tests passed. `transcriptAtlasLayout` captures the
  native fixture when `KETPLUS_ATLAS_TRANSCRIPT_CAPTURE` is set; Cocoa capture passed.
- `ketplus_agent_viewport_tests`: 29 passed, including collapsed file groups, mixed
  image heights, warm conversation switches, streaming and historical reader anchors.
- `ketplus_agent_streaming_tests`: 12 passed; three opt-in performance benchmarks skipped.
- `ketplus_provider_transcript_tests`: 9 passed, including hiding commentary deltas
  when their settled item identifies the commentary phase.
- Row geometry, row measurement, window model and activity-group CTest targets passed.
- Desktop target built successfully; `git diff --check` passed. No installed bundle
  or live daemon was replaced. Real-history load benchmarks remain unmeasured.

The group IPC/UI fixture uses a temporary socket, token and journal with an
unavailable provider executable. To capture its rendered expanded group at
1100 and 520 px, set `KETPLUS_GROUP_CAPTURE_PREFIX` to a writable artifact prefix
and run `ketplus_desktop_activity_groups_tests` in a native graphical session.
It does not attach to the user's daemon or run a model turn.

`ketplus_daemon_client_upgrade_policy_tests` uses an authenticated loopback IPC
fixture to check older, matching, newer and incompatible daemon versions. Its
expectations follow `KETPLUS_AUTO_UPGRADE_DAEMON`; it never attaches to the user's
running daemon.

### WebRTC host phase 3

Build `ketplus_daemon_peer_transport_tests`, `ketplus_daemon_relay_wakeups_tests`,
`ketplus_daemon_cloud_privacy_tests` and `ketplus_daemon_context_relay_tests` in a
Release headless tree. The peer suite uses real libdatachannel loopback with signed
SDP, in-memory credentials, temporary SQLite and a fake provider that is never
started. It covers transfer caps/order/expiry, strict send priority, grants,
revocation, supersede/epochs, subscriptions, cloud-safe encryption and the same
command retried over both paths. No user daemon or provider account is accessed.
Physical devices/TURN require a separate explicitly configured local test stack.

## Conversation deletion and next-turn defaults

`ketplus_provider_deletion_tests` uses fake providers and temporary journals to
check historical-provider cleanup, shared references, restart retry and unsupported
provider rejection. `ketplus_desktop_daemon_client_tests` covers deletion reaching
another attached Desktop client. The standalone Release cloud-privacy lane checks
heartbeat deletion receipts and retry; the KetPlus deployment owns relay tombstone,
workspace/generation isolation and encrypted-history redaction tests.

`ketplus_context_harness_tests` checks active/attaching turns retain submitted
settings. Model-picker and iOS model-apply tests check that future defaults remain
writable. No provider-account or quota-consuming test is required for these fixes.

### Paired Claude usage

`ketplus_daemon_claude_adapter_tests::remoteAccountUsageIsReadOnlyAndSessionFenced`
checks the optional effective configuration report, unknown generation rejection, no key
mutation through a reporting payload, and no organization report in session history.
The separate analytics service tests cover report pagination, cents conversion, cached reads,
credential rotation and sanitized provider failures with fake networking. No inference calls.

### Usage retention backlog regression

`JournalUsageRetentionTest::rawBacklogUsesTurnLookup` creates 300,000 synthetic raw
rows in a later turn, then measures a 128-row idle pass over older replay turns.
It checks a covering ACK lookup on all three `events_turn` keys, preserves the
raw backlog, and covers both fresh and pre-existing journals missing the index.
The plan assertion is deterministic; a one-second timing ceiling has generous
runner headroom. No live journal or provider is used. Reproduce Release timings:

```sh
cmake --preset daemon-release -DKETPLUS_BUILD_TESTS=ON
cmake --build build/daemon-release --target ketplus_daemon_usage_retention_tests
benchmarks/run-usage-retention.sh > /tmp/usage-retention-results.txt
```

The script records hardware and three runs. Timings exclude fixture creation and
the one-time index build on journal open; they are not a multi-GB startup benchmark.

Command-output retention shares `ketplus_daemon_usage_retention_tests`. It runs
the eligibility matrix for both usage and command output, verifies UTF-8 head/tail
boundaries, explicit receipt notices after the 40-line cap, unchanged item metadata
and other replay rows, stale continuation fencing, idempotence, byte-budget resume
and atomic rollback of payload updates. Synthetic payloads are paged through the
real replay byte limit rather than assuming one page contains the entire journal.

`benchmarks/run-usage-retention.sh` also reports a warm command-retention pass on
230,000-byte outputs after a forced rollback, with fixture creation excluded.
This is a bounded-batch measurement, not a latency guarantee for arbitrary journals.

`ketplus_daemon_detail_compactor_tests` exercises the command/tool/reasoning
payload policy: aggregate UTF-8 text limits, media/resource/error/argument and
structured-field preservation, nonterminal/unknown/small shapes, reasoning summary
preservation, missing summaries and idempotence. The journal retention suite also
checks MCP/dynamic/reasoning against actual old/new, active and unacknowledged turns.

The retention benchmark script also prints elapsed time for eight bounded replay
passes on synthetic eligible MCP, dynamic-tool and reasoning fixtures (fixture setup
excluded). Qt transcript tests check the visible and accessible `Text trimmed` title
on ordinary MCP, browser and dynamic-tool rows. This does not replace native VoiceOver
or installed-device verification.

File-diff retention (ADR 0040) adds transformer cases for add/delete/update/rename,
unknown formats, full original statistics, UTF-8 excerpts, idempotence and invalid
stored counts. The journal guard matrix and Release benchmark include file changes.
Qt transcript tests assert the original counts, rename identity, and visible and
accessible `Diff trimmed` label in normal and compact single-file rows. The shared
statistics header requires the full Pro build/test matrix, in addition to the
focused daemon and render cases. Native VoiceOver, Windows and live multi-GB journals
remain separate acceptance work.

Adjacent-start retention (ADR 0041) extends the journal guard matrix and adds
`ketplus_desktop_start_retention_tests`. The latter compares actual desktop
projection content/order, activity group identity, replay/page agreement and stale
continuation rejection for adjacent versus interleaved tool lifecycles. It uses
synthetic temporary journals and no provider processes.

`ketplus_daemon_retention_policy_tests` verifies strict policy parsing, defaults,
revision-fenced persistence/reopen/rollback, actual configured retention, malformed
policy fail-closed behavior and local-only API/cache boundaries. ADR 0085 adds
TEXT/BLOB stored JSON boundary cases for both policies (4095/4096 accepted,
4097/1 MiB rejected), preserving row bytes/revision/history across reopen and
repair/retry. Run this target plus `ketplus_daemon_size_policy_tests`,
`ketplus_daemon_usage_retention_tests` and `ketplus_daemon_journal_space_tests`
in Release for stored-policy reader changes. The agent-token
IPC test also rejects both retention operations. Changes to the shared journal or
host headers require the full Pro build and suite. No provider quota is consumed.

### Tool-argument retention (#128)

Run `ketplus_daemon_detail_compactor_tests`, `ketplus_daemon_usage_retention_tests`
and `ketplus_agent_tests` after changing argument excerpts. Fixtures cover UTF-8
object/array/string prefixes, previous text-marker upgrades, unknown-marker and
active-item preservation, latest-turn/ACK/question guards, idempotence, and visible
normal/browser labels with accessible names. `benchmarks/run-usage-retention.sh`
includes fresh and prior-marker argument passes alongside the raw-backlog lookup.
No live provider or installed journal is used.

### Structured tool-result retention (#128)

Run `ketplus_daemon_detail_compactor_tests`, `ketplus_daemon_usage_retention_tests`
and `ketplus_agent_tests`. Fixtures cover explicit/sanitized UTF-8 object/array
excerpts, shared text budget, error/resource/depth preservation, idempotence, both
known-marker upgrades, the existing turn-guard matrix and rendered trim notices.
Use `structuredRetentionUsesTurnGuards` in the Release journal test for timing.
No installed journal or live provider is used.

### Opt-in multi-GB vacuum acceptance (#128)

After a Release build of `ketplus_daemon_journal_space_tests`, run
`benchmarks/run-journal-scale.sh <build-directory>`. This explicitly allocates an
isolated 9 GiB random-payload SQLite fixture under the build directory, deletes
5.5 GiB of payloads without vacuuming, then exercises the production migration.
It requires at least 24 GiB free at start and preserves 8 GiB while seeding.
The temporary directory is removed on exit and explicitly checked after success.

The gate requires a source file above 9 GB and a migrated file below 4 GB, identical
SHA-256 over every retained row ID and payload byte, all 3,584 rows, an `ok`
integrity check, incremental-vacuum mode, no free pages or leftover copy, and an
idempotent second migration. Logs record bytes and elapsed migration time. The
normal suite skips this disk-intensive case. It never opens an installed journal.
This verifies large-file free-page migration, not retention policy/size-pressure
acceptance or the result on a real user's 9 GB journal.

### Journal file and low-disk measurements (#128)

The journal-space suite compares actual file/sidecar lengths and verifies unchanged
files, a timestamp, path-free output and non-file-journal rejection. Retention API
tests cover additive metrics with existing save/cache/privacy behavior. Storage-page
tests cover the below/equal/above 5 GB boundary, missing/malformed metrics, accessible
warning text, recovery after reload and narrow layouts. Run the full Pro build/suite
for the shared journal header change; no real journal or provider is used.

### Size-pressure backend (#128)

`ketplus_daemon_size_policy_tests` covers strict policy/CAS/reopen/failure handling,
local API privacy/cache boundaries and pressure ordering. Its isolated 1.02 GB
zero-payload database lives under the test binary directory, requires 4 GiB free
and is removed by QTemporaryDir; insufficient space skips that one case. It checks
Forever/off, running/unacknowledged/question/latest-twenty guards, complete expired
sweeps before recent detail, and existing free-page priority. Agent-token denial
and disconnected-request tests include both storage API families. Shared headers
require full Pro build/CTest. No installed journal or live provider is used.

The usage-retention suite also exercises bounded large-tool manifests: real
fragment storage, the shared byte budget, latest-twenty and ACK/session guards,
corrupt/oversize chunks, identity/digest mismatches, unknown manifests, shared
references, stale snapshot fencing and rollback at blob deletion/projection
revision. These synthetic fixtures do not validate image preview conversion or
retention of payloads above 1 MiB (ADR 0047).

The large-payload GC regression also reopens a journal missing the transfer
expression index with 300,000 retained same-item updates. It verifies index
creation, correlated production-predicate query-plan selection with bound parameters,
25 exact GC lookups and retained-row preservation;
timings separate startup index construction from the idle pass.

The ADR 0058 `DaemonProtocolTest::appendRequiresEveryDurableWrite` fixture injects
ignored mandatory row writes and deferred-constraint commit failures for ordinary
and chunked appends. It verifies unchanged delivery/replay/projection state,
transaction release, repair/retry and reopen. Run the protocol target plus affected
activity, retention, compaction, lifecycle, worktree/fork and cloud privacy targets.
It does not exercise a live journal or spend provider quota.

ADR 0059 `DaemonProtocolTest::appendPreservesUnsupportedCounters` covers integer
storage/type guards, overflow refusal for normal/chunked appends, positive boundary
controls, unchanged history/counters after refusal, transaction release and
repair/retry/reopen. Use temporary journals and the affected backend matrix.

ADR 0060 `SessionForkTest` injects ignored copies/timestamp and commit failures,
verifies rollback of source/destination/context/catalog state, then retry/reopen
and exact copied values. Empty history and existing/self/deleted destinations have
separate controls. A gated fake Codex fork response exercises a destination claimed
by another start command while the provider call is pending. Run the fork target
and affected protocol/activity/transcript/lifecycle/worktree/retention targets.

ADR 0061 `SessionForkTest::journalForkPreservesUnsupportedSourceCounters` covers
negative, fractional, TEXT and BLOB cursor/turn values. It checks exact source
preservation, no destination, released transaction, fixture repair, retry and
reopen. Run the combined fork suite and its affected backend matrix.

### Failed journal reuse (ADR 0086)

`DaemonProtocolTest::startupCommitFailurePreservesErrorAndReleasesTransaction`
covers policy-read, policy-write and session-write attempts after turn/replay/
activity commit failures, plus the original no-operation controls. The failed
object stays closed, a separate writer can repair, and a fresh journal reopens
without retained-row changes. `failedAgentSchemaCannotBeUsedUntilReopened`
covers the final agent-schema stage and conversation-number ownership recovery.
Run the protocol, activity-groups, agent-messaging, retention-policy, journal-space
and headless-daemon targets in Release. This does not establish full startup
atomicity, disk-full/crash or multi-GB/platform acceptance. Synthetic journals only.

The ADR 0091 catalog regressions also cover deletion inserted behind the cursor,
revision rollback, retention/update invalidation, reopen with an old token,
malformed revision admission, three-retry exhaustion, discovery paging and
`ketplusctl sessions` emitting a complete 30k-tombstone JSON object. The latter
uses a fixture daemon, not an installed service or provider. A legacy catalog
above 960 KiB verifies the actual serialized IPC envelope still fits 1 MiB.
