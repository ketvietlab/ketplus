# Testing

## Default suite

The default suite is deterministic, headless, and must not consume model quota:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

GUI tests run with `QT_QPA_PLATFORM=offscreen`. The Qt build and Release
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
| Settings composition | `ketplus_settings_integration_tests` | Shared appearance persistence across Pro shell surfaces; Providers page |
| Atlas process/WebView | `ketplus_atlas_tests` | Atlas process and tab behavior |
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
| Workspace boundary | `ketplus_workspace_tests` | Pro shell with CM explorer behavior |
| Async question lifetime | `ketplus_async_question_lifetime_tests` | turn expiry, session/generation isolation, late items and stale answers |
| Daemon protocol/storage | `ketplus_daemon_protocol_tests` | framing, journal, deduplication, approval expiry |
| Economic context harness | `ketplus_context_harness_tests` | strict window validation, journal migration/inheritance, Codex start/resume/fork/reset, idle configuration fences, Claude inherited turn options; fake providers only |
| Browser provider wiring | `ketplus_browser_provider_integration_tests` | host routing instructions for direct/daemon Codex and new/resumed Claude/Cursor, prompt/image preservation, scoped approvals, disabled/damaged/missing runtime |
| Daemon Claude adapter | `ketplus_daemon_claude_adapter_tests` | sidecar turns, approvals, handoff |
| Provider handoff history | `ketplus_provider_handoff_tests` | history after delivery acknowledgment, compacted cursor gaps, event/byte-limited paging, session/generation isolation |
| Agent messaging (ADR 0027) | `ketplus_agent_messaging_tests` | number allocation, uniqueness, restart stability and legacy-journal backfill; idle delivery and automatic reply from the final answer; busy receivers batched at turn end; `urgent` steering of a running turn; hop, chain and hourly holds released with the next user turn, and the 24-hour request expiry; `#NNNN` references; token, number, size and reply validation; deletion quarantine and token revocation; per-provider `ketplus_agents` injection (Claude `mcpServers`, Codex dotted per-thread config, Cursor ACP); agent-token IPC scope without broadcasts; MCP tool mapping; `sent` and `replied` notices and the running automatic turn's sender in `session.list`; the number and that sender sealed into the phone's session metadata (`ketplus_daemon_cloud_privacy_tests`); Desktop notice text (`ketplus_provider_transcript_tests`) and the sidebar "from #N" label (`ketplus_workspace_tests`). Fake providers only; the one authorized live run is recorded in ADR 0027 |
| Conversation fork | `ketplus_daemon_session_fork_tests` | `threadFork` gating, Codex/Claude native fork, transcript copy at the same cursors, idempotent retry, active source isolation and unknown refusal, next turn resumes the forked thread |
| Daemon Cursor adapter | `ketplus_daemon_cursor_adapter_tests` | ACP turns, login/install errors, approvals, `loadSession: false` skips `session/load`, `agent models` fixtures |
| Provider CLI compatibility | `ketplus_provider_compatibility_tests` | version parsing and statuses, manifest, Homebrew update command, version probe cache, outdated CLI blocked, untested CLI journaled once, `provider.runtime_status`, dialects and capabilities, `-32601` narrowing and steer gate, unknown Codex input (ignored notifications counted, protocol errors, declined requests, unknown item types) |
| Codex protocol contract | `ketplus_codex_contract_tests` (Node) | declared Codex usage exists in every tested schema snapshot; daemon code names only declared methods |
| Relay browser sign-in | `ketplus_daemon_browser_sign_in_tests` | relay paths appended to a base path, loopback redirect, PKCE S256 challenge, wrong state/path refused, one-time callback |
| Daemon lifecycle/IPC | `ketplus_daemon_lifecycle_tests` | authentication, protocol minor and features, reconnect replay, isolation, fencing |
| Daemon upgrade | `ketplus_daemon_upgrade_tests` | version advertisement, idle replacement, active-turn deferral |
| Desktop daemon attach | `ketplus_desktop_daemon_client_tests` | detached UI lifetime, discovery, normalized transcript replay, negotiated features, provider capabilities, fork through the daemon |
| macOS bundle layout | `ketplus_macos_bundle_layout_tests` | embedded matching daemon, loadable Qt runtime plugins, and absence of bundled Codex |
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

## Minimum verification

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
`ketplus_headless_daemon_tests` (zero-session catalog, local auth, isolated real
daemon/CLI, SIGTERM), `ketplus_device_authorization_tests` (PKCE, polling,
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
- One Pro module: focused target plus its directly affected tests.
- Shared header or desktop shell: full build and full Pro test suite.
- `core/` change: the Lite suite (`ctest --preset lite-dev`), then the full Pro suite.
- QML layout or native rendering: offscreen tests plus the relevant native
  screenshot/pixel check documented in `agent-workspace.md`.
- Performance-sensitive behavior: Release build and the matching script under
  `benchmarks/`; record hardware and raw results.

Browser and SDK wiring changes also run:

```sh
(cd apps/browser && npm test)
node --test apps/daemon/providers/claude-sidecar/test/browser.test.mjs apps/daemon/providers/claude-sidecar/test/transcript.test.mjs apps/daemon/providers/claude-sidecar/test/input.test.mjs apps/daemon/providers/claude-sidecar/test/tasks.test.mjs
```

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
