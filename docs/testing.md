# Testing

## Default suite

The default suite is deterministic, headless, and must not consume model quota:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Team provider switch (T-2, `team.provider.set`): run `ketplus_team_provider_switch_tests`
with the team protocol, remote, flow and agent-tools fixtures and the provider handoff
tests (`ketplus_daemon_claude_adapter_tests`, `ketplus_turn_acceptance_tests`). It runs
whole-team Codex → Claude and back, a chosen part of the team, a switch held while a turn
waits for an approval, the Codex reviewer's Full access, Full access given with the switch
only when acknowledged (T-3), a provider that is not installed
and a member in a switched worktree, against the scripted fake providers only.

Team route tools (ADR 0130): run `ketplus_team_routing_tools_tests` together with
the team risk-policy, agent-tools, agent-tasks, protocol and local/remote team
fixtures. The positive case uses temporary real Git objects → the actual bounded
prose-reflow producer → the runtime receipt store → coordinator policy. Unsupported
semantics, forged facts/IDs, wrong actors, dirty/stale revisions and user-client
impersonation fail closed. It starts no real provider and executes no repository
scripts. Real Git attack fixtures set `diff.ignoreSubmodules=all` to conceal an
added gitlink, or replace the base tree to conceal an authentication-code change.
The producer must reject both complete deltas and the authenticated tools must
issue no receipt or low-risk route. A shared host header/build-wiring change also
requires the full build and full Pro suite below; focused fixtures alone are not
the complete matrix.

Every Pro/daemon ctest test runs with `QT_QPA_PLATFORM=offscreen`: `ketplus_tests_offscreen()`
(`cmake/OffscreenTests.cmake`) sets it for each test in `tests/` and `tests/daemon/`, over the
test's own environment and the shell's, so a test run never opens windows on the screen. A
test binary run directly needs it exported. This override does not cover Lite tests in
`core/`. The Pro desktop build and Release
characterization run on macOS. Linux Pro desktop packaging is restored on native
Ubuntu 24.04 amd64/arm64 through the shared package recipe. The headless daemon,
browser regression and design evidence checks still run on Ubuntu. Pro Windows
CI remains deferred until the required Qt 6.11 WebView development packages are
available from Qt's public repository. Integration PRs have the public-tree guard;
Linux packaging PRs additionally run the native ARM build-only job. Other native
integration builds and branch-push builds remain local; run the targets for the area you changed before pushing
and list them in the pull request. CI runs on promotion pull requests into
`develop`, on pull requests into `main` and on pushes to `main`; a newer commit
cancels the obsolete run. See `docs/release-process.md`.

### CI operating-system matrix

`build.yml` calls `os-suite.yml` once per edition and OS. Its `build` job
compiles the suite once, on the oldest runner of the set so the binaries load
on the newer ones, and uploads the build tree (without object files or static
libraries) as a three-day artifact. `test-shards` test jobs per runner version
(`fail-fast: false`) restore the same Qt and Node, unpack the tree and run
`ctest` with `QT_QPA_PLATFORM=offscreen`, each on its share of the suite. A
`tests-passed` job reports one result for the whole matrix.

The macOS Pro build configures `Python3_EXECUTABLE` as the checked-in
`packaging/macos/ci-python3` launcher. CTest keeps this workspace-stable command
instead of a build runner's versioned Python toolcache path. The launcher resolves
`python3` from the executing runner's PATH and preserves arguments and exit codes;
the test job checks that interpreter before CTest. This applies to the signing CLI,
team translation catalog and SSH transaction fixtures, without altering their
assertions or platform skips. `python3 tests/packaging/test_ci_python_launcher.py`
checks configuration with one interpreter and execution after it is removed and
replaced on PATH, plus missing-interpreter and child failure/skip behavior.

| Job | Built on | Tested on |
| --- | --- | --- |
| `lite-linux` | `ubuntu-22.04` | `ubuntu-22.04`, `ubuntu-24.04` |
| `lite-macos` | `macos-15` | `macos-15`, `macos-26` |
| `lite-windows` | `windows-2022` | `windows-2022`, `windows-2025` |
| `pro-macos` | `macos-15` | `macos-15`, `macos-26`, 16 shards each (serial CTest) |

### Test labels, costs and CI shards

`ketplus_label_tests(<area>)` (`cmake/TestLabels.cmake`) labels every test of
its directory `desktop`, `daemon`, `runner` or `packaging`. `tests/test-costs.txt`
lists measured durations of the longer tests. Each becomes the test's CTest
`COST`, so long tests start first, and a test of 30 seconds or more is also
labelled `slow`:

```sh
ctest --preset dev -L daemon       # one area
ctest --preset dev -LE slow -j4    # quick pass without the slow tests
ctest --preset dev -L slow         # only the slow ones
```

A sharded CI job runs `tools/ci/shard_tests.py plan`, which reads the tests
and costs from the build tree (`ctest --show-only=json-v1`), puts the longest
first, each into the shard with the least total cost (unlisted tests count 3
seconds), and prints the name regex of its own shard. Every shard plans from
the same tree, so all tests run exactly once. The Pro suite runs serially on one
runner for about 35 minutes. On 16 shards each takes about as long as its
longest test, and more shards would only add the 3 to 4 minutes of setup per
job. Each test job keeps its `CTestCostData.txt` as a `test-costs-*` artifact
for 14 days. When the shards drift apart, refresh the file from a run's
artifacts or a local tree:

```sh
python3 tools/ci/shard_tests.py costs path/to/CTestCostData.txt... > tests/test-costs.txt
python3 tests/ci/test_shard_tests.py
```

The public-tree workflow runs `tests/ci/test_shard_tests.py` on every pull
request.

The FetchContent cache (`.cache/fetchcontent`) is keyed on the suite, OS,
architecture, compiler version, Qt version and the hash of `CMakeLists.txt`,
every `*.cmake` file and the Claude sidecar `package-lock.json`; compiler
objects go through sccache. CTest files hold absolute paths, so the build job
records the checkout, Qt and Node paths in `build/ci-paths.txt` and each test
job fails with a named mismatch, instead of running stale or missing binaries,
when a runner image puts any of them elsewhere. Node is pinned to an exact
version for the same reason. No job in the matrix uses provider quota.

### Lite on a local Windows machine

`tools/windows/win-run.sh` (see `docs/development.md`, Windows) runs the Lite
suite on a Windows build machine with MSVC 2022 and Qt 6.11:
`cmake --preset lite-dev`, `cmake --build --preset lite-dev`, then
`ctest --preset lite-dev -j4 --timeout 180`. All 28 Lite tests pass on Windows.
The two tests that start real `cmd.exe` shells (`ketplus_pty_backend_tests`,
`ketplus_terminal_tests`) are `RUN_SERIAL` there, because a cold shell start can
exceed their wait windows when it competes with the rest of the suite.

macOS runs 29 tests: `ketplus_terminal_app_tests` is built only on Apple
platforms. Run it with `QT_QPA_PLATFORM=offscreen ctest --preset lite-dev -j4`.

Two cases inside `ketplus_preview_tests` are skipped on Windows (QTest `SKIP`,
which CTest does not list): `discoversMmdcInstalledByNvm` (the nvm layout and
`HOME` are POSIX only) and `rendersMermaidAndOpensThePopup` (its fake `mmdc` is
a `/bin/sh` script). Windows `mmdc` discovery and rendering are not covered yet;
a Windows version of the first test must set `USERPROFILE`, which
`QDir::homePath()` reads on Windows, alongside `HOME`.

Open, edit and save, Git status and the Markdown preview are covered by
automated offscreen tests that ran on Windows: `ketplus_lite_window_tests` (a
real Git repository opened in the window), `ketplus_tests` document save,
`ketplus_editor_file_refresh_tests`, `ketplus_git_tests`,
`ketplus_git_changes_panel_tests` and `ketplus_preview_tests`. Theme includes
from a UNC share (`\\localhost\C$`) run in `ketplus_theme_input_tests`. The
case with a filesystem root as the source (`C:\` or `/`) writes a scratch
directory there, so it runs only with `KETPLUS_TEST_WRITE_FILESYSTEM_ROOT=1`
and is skipped otherwise. Rendering Mermaid needs the external `mmdc` tool.

Launching the app for a smoke check needs a desktop. An ssh session has none,
so the default `windows` Qt platform plugin aborts at startup. Deploy the
offscreen plugin next to the executable and use the review capture (see
`docs/design-system.md`):

```bat
windeployqt --no-translations --compiler-runtime build\lite-dev\core\KetPlusCM.exe
copy %QT_ROOT%\plugins\platforms\qoffscreend.dll build\lite-dev\core\platforms\
set QT_QPA_PLATFORM=offscreen
set KETPLUS_CAPTURE=smoke.png
build\lite-dev\core\KetPlusCM.exe <repository-folder>
```

The Debug executable is `core\KetPlusCM.exe` and needs the OpenSSL `bin`
directory on `PATH`. A capture of a folder holding a Markdown file and a Git
repository shows the Markdown preview, `Git: <branch>` with the changed-file
count, the file's CRLF and UTF-8 BOM, and the light and dark themes. The
Mermaid block falls back to code with a notice until `mmdc` is installed.

## VS Code importer fixtures

`ketplus_vscode_import_tests` covers original dark/light/include themes, standard
DOCTYPE tmTheme resources, explicit style resets, ordered fallback chains, ANSI16,
semantic metadata without application, unsupported selectors and multi-variant
VSIX. Failure cases cover unsafe paths/archives, malformed data, entity expansion,
include cycles and shared byte/canonical-output limits. Fixture provenance is in
`core/tests/fixtures/vscode/README.md`. Run importer, common I/O and canonical model
targets with `QT_QPA_PLATFORM=offscreen`; no extension or provider runs.

## Color scheme management UI

`ketplus_scheme_library_tests` covers format dispatch, multi-variant choice,
review-before-write and cancellation, native round-trip/export errors, catalog
search, built-in deletion protection after selection/theme updates in both reserved
ID namespaces, duplication/deletion, active and preferred fallback,
selection locks, live selection, in-memory import preview restoration and the
shared keyboard picker. It checks both minimum/large Appearance layouts at every
supported zoom in light/dark. `ketplus_settings_tests` retains existing settings
contracts; `ketplus_lite_window_tests` checks command registration.

For native review, run only `appearanceLayoutAndReviewCaptures` with
`KETPLUS_SCHEME_CAPTURE_NATIVE=1` and `KETPLUS_SCHEME_CAPTURE_DIR=<output directory>`
on the scheme library test executable (without the offscreen QPA override).
The harness refuses to show a native window unless the secondary DELL display is
present, sets that screen before showing, captures light/dark/report, and removes
its isolated test profile. The normal ctest path uses offscreen rendering and does
not open a native window.

## Bundled color-scheme fixtures

Run `ketplus_builtin_scheme_tests`, `ketplus_color_scheme_tests`, and the two
importer/common-input targets offscreen after changing bundled palettes or the
catalog. These checks use embedded resources and committed import reports without
network or provider access. Explicit source regeneration/byte verification is
separate: follow `docs/color-schemes.md` with the locked upstream cache.

## JetBrains importer fixtures

`ketplus_jetbrains_import_tests` converts original synthetic dark/light `.icls`
fixtures, a named-color UI theme referencing an editor scheme, and an in-memory
multi-variant JAR. It checks primary-role precedence in alphabetic and reversed
attribute order, missing-primary fallbacks, effect-color presence/validity in both
option orders, syntax/style resets, all 16 console ANSI mappings,
editor/diff tokens, canonical serialization, per-key omission reports, root-scoped
resource references, palette cycles, malformed input, internal DTD/entity rejection and
archive traversal rejection. Fixture provenance/license is in
`core/tests/fixtures/jetbrains/README.md`. Run it and the common I/O/model tests
with `QT_QPA_PLATFORM=offscreen`; no IDE, Java runtime or provider is invoked.

## Theme input security fixtures

`ketplus_theme_input_tests` runs headlessly with no provider or network access.
It constructs small ZIP fixtures in memory, including patched hostile metadata,
and tests JSONC comment/string boundaries and error locations, ZIP traversal,
links/collisions/CRC failure, all four archive budgets, standard plist DOCTYPE acceptance without external resolution, internal DTD/entity rejection,
and local/archive includes with cycles, escapes and the 16-level boundary.

## Target map

| Area | Build/test target | Primary coverage |
| --- | --- | --- |
| Desktop search and routing | `ketplus_spotlight_tests` | Spotlight file/Atlas search and repository routing |
| Live conversation discovery | `ketplus_desktop_session_discovery_tests` | Another client creates a session through a real local daemon/fake provider; catalog races, event coalescing, cross-workspace visibility and stable selection with a scripted daemon; archive, restore and worktree archive reach the daemon, earlier local archive state is pushed once, a newly seen conversation keeps the daemon's archive time, a second desktop's refresh adopts the first one's archive or restore, and a rejected restore stays archived with an error (ADR 0100) |
| Transcript file activation | `ketplus_transcript_file_opening_tests` | Text/source-line editor routing, MIME/content classification, external media/PDF/app handoff, encoded paths, symlinks and missing files; platform opens intercepted without launching apps |
| Repository navigation cache | `ketplus_repository_metadata_tests` | SQLite persistence, invalidation, unavailable cache |
| Conversation-owned worktrees | `ketplus_conversation_worktree_tests` | SQLite path map, hide/remove, turn-delta capture |
| Runner worktree snapshots | `ketplus_runner_snapshot_tests` | Temporary real Git repositories and the system git: snapshot content (edits, staged and unstaged, deletions, renames, executable bit, symlinks, ignores, linked worktrees), the user's repository unchanged byte for byte, delta packs, applying into a workspace, rejected packs leaving it unchanged, hostile hooks, filters and configuration not running, forged trees and symlinks not writing outside it, the user's excludes and attributes files compared with `git ls-files`, submodules, LFS and other filtered paths reported but their worktree content not sent; builds in the `daemon-dev` preset too; on Windows `ketplus_runner_snapshot_windows_tests` runs the sealing checks that need no symlinks, mode bits or sh fixtures (NUL replaces `/dev/null`, hostile hooks, filters and global configuration never run, raw bytes with CRLF, a read-only file dropped by a newer snapshot is removed, a git that refuses `attr:` pathspecs reported as too old) and needs Git 2.47 or newer |
| Desktop snapshot sync | `ketplus_runner_snapshot_store_tests`, `ketplus_snapshot_keys_tests`, `ketplus_snapshot_preparation_tests`, `ketplus_snapshot_retention_tests`, `ketplus_snapshot_sender_tests`, `ketplus_snapshot_sync_integration_tests` | Store refs and pruning with the user's repository unchanged, source `HEAD` read from files, workspace keys against fixed vectors, preparing a job's snapshot, the pack cache's restart, age and size limits, the sender's offer, chunk, ack, error and ready rules over fake ports, and a real snapshot sent, released and applied into a workspace; a job cancelled while a thread has its pack open (hashing or reading) leaves no pack file: Windows cannot delete an open file, so `SnapshotWorker` removes the pack again once the thread has answered, and `cancellingWhileHashingLeavesNoRefOrFile` only fails on Windows without that |
| Runner pairing | `ketplus_runner_pairing_tests` | Fixed vectors computed with Python's standard library and the OpenSSL command line: pairing token encode and parse (each malformed, missing, mistyped and oversized case refused, the token never in QDebug output or error codes), runner and desktop fingerprints, pairing digest and confirmation code, each digest input changing the code, the desktop key check, canonical bytes and Ed25519 signatures of each signed payload with every field, key, key version and issuedAt window failure, pin records round trip and corrupt records refused; builds in the `daemon-dev` preset too |
| Runner peer protocol frames | `ketplus_runner_protocol_tests` | `ketplus_runner_protocol` only, no network: round trip of every frame including optional and null fields, exact-limit and limit-plus-one values for every frame size and field limit, malformed JSON, invalid UTF-8, wrong field types, unknown fields and types, channel and direction routing, error code strings |
| Runner process | `ketplus_runner_app_tests` | `ketplus-runner` against an in-process Relay on 127.0.0.1 with a fake clock: state directory mode, owner, lock, atomic writes and corrupt files; register and pair until the pin is active (token from argument, stdin and file), local refusals with no Relay call, desktop key mismatch, approvals that do not verify, ended pairings and lost answers; access token challenge signature, refresh credential rotation, lost answers and stops between rotation and store; each Relay refusal (exit code or backoff capped at 60 seconds), the 30-second heartbeat through the production `run` wiring (which reports `draining` while no command runner is ready), `run` from the command line with the real event loop (removed runner, stop), unpair, deregister, status, every exit code in `--help`, and no secret in output, logs or the state directory |
| Runner run loop | `ketplus_runner_run_tests` (its real Linux jobs with `KETPLUS_RUN_LINUX_JOB_TESTS=1`) | `ketplus-runner run` over a fake Relay, socket, peer transport, command runner and clock, with the real job pieces, journal, accepted-jobs file and pins: start-up clean-up before the journal and any acquire (bounded at 90 seconds), `draining` until started and while the command runner is not ready or a record cannot be read, acquire only on `pending` or `job.available` with every condition holding and once per answer, a heartbeat 2 seconds after a state change, a lease without spec given back while draining, restart from a journal step, Relay calls never nested (a nested event loop counts the depth), at most one waiting heartbeat, `job.cancel` renewing once, whole jobs, refusals, reconnection, cancel and timeout, stop signals (idle, acquire in flight, with a job, second signal and the restart after it), fatal refusals during a job, no secret in logs; then one job with a real git snapshot through the snapshot receiver, and one over a real WebRTC connection on this machine |
| Runner job session | `ketplus_runner_session_tests` | `ketplus_runner_session` over fake channel, lease, snapshot receiver, command runner and clock: handshake, broken frames, spec refusals, start/accepted/started order, at-most-once launch, output paging, acknowledgement, resume and the 16 MiB ring, cancel, deadline, lost connection and lease, result delivery and finish. |
| Runner command executor | `ketplus_runner_exec_plan_tests`, `ketplus_runner_exec_tests`, `ketplus_runner_exec_native_tests` (macOS; Linux with `KETPLUS_RUN_LINUX_JOB_TESTS=1`), `ketplus_runner_linux_job_tests` and `ketplus_runner_assembly_linux_tests` (Linux with `KETPLUS_RUN_LINUX_JOB_TESTS=1`) | Launch, mount (including state, work and runtime directories nested in any order), task id and result tables; the Linux policy refusing nested runner directories and becoming ready once they are apart; job directories (mode, identity, refused and unidentified ones); the command runner over a fake executor and policy: readiness states, the closed policy, launch refusals, the busy refusal, the backstop, 32-page output pacing, lost bytes, draining before the end, one end per command, kill and its 60-second wait, unverified ends, restart cleanup, and no command data in logs; then the real job executor: the request's environment only, cwd, exit codes, a 6 MiB binary and UTF-8 output by digest, kill of a group ignoring SIGTERM, and a job left by a runner that went away; then `run` as the runner's main assembles it, with the paired desktop played over the fake Relay: a job that exits 0, one that exits non-zero and one that times out, and a runner killed during its job and started again, which ends the job at the Relay as interrupted once without running it twice |
| Runner snapshot receiver | `ketplus_runner_receiver_tests` | `ketplus_runner_snapshot_receiver` with real packs from the snapshot library and the system git: key checks, have/need and resume after restart, chunk pairing, ordering, repeats and corruption, error codes leaving the workspace unchanged, incremental apply and clean, base_missing, disk_full, foreign owners, cancel at each step, partial pack expiry, budget eviction and the held list. |
| Runner Relay socket and peer connection | `ketplus_runner_peer_tests` | `ketplus_runner_peer` over fake socket, token source, clock, transports, signer and pins, then real links on 127.0.0.1: ready and role, 401 renew once, 403 and unknown refusals with backoff, close codes 4001, 4003 and others, the wake-up table, signed offer bytes against the pairing vector, every answer and `signal.error` failure, desktop `signal.end` reasons, candidate limits and order, the 20-second deadline, retry delays and start spacing, open and lost reported once, frame and transfer limits on the three channels, a 4 MiB transfer and four interleaved ones, a desktop `WebRtcPeerLink` answerer through an in-process Relay, a tampered answer that never opens, the Relay socket adapter against local WebSocket and HTTP servers, and no token, TURN credential, SDP, candidate or signature in any log line; builds in the `daemon-dev` preset too |
| Runner job delivery | `ketplus_runner_delivery_tests` | `ketplus_runner_delivery` over fake Relay calls, session, journal, pins and clock: acquire triggers, acquireId journaled first and repeated after a lost answer, each acquire refusal, `runner_busy`, requester pin and label checks, 60-second renewal and each renewal answer, start, finish and release repeated unchanged, journal steps before their effects and failed writes, restart from each journal step, draining and heartbeat fields. The job calls, the journal and accepted-jobs files and pin trust run against the in-process Relay in `ketplus_runner_app_tests`. |
| Desktop runner pairing | `ketplus_runner_relay_tests`, `ketplus_runner_pairing_daemon_tests` | The daemon's Control v2 client against a fake Relay on a loopback port, with time driven by the test and in-memory credentials: only runner routes accepted by the route seam, each desktop runner operation with its route, body and refusal codes, the runner.relay/1 capability gate, schema checks, runner pins (format, corrupt and other-version records, removal on sign-out and re-enrollment), the pairing flow from ticket to pin with claim signature, claim time window, changed claims, wrong codes, polling backoff and expiry, remove and unpair dropping the pin first, and no token, ticket secret or machine credential in logs, events, replies or stored credentials. `ketplus_headless_daemon_tests` runs the `ketplusctl runner` commands against a fixture daemon |
| Desktop runner peer links | `ketplus_daemon_runner_link_tests` | `ketplus_daemon_runner_link` over fake clock, transports, machine key, pins and signaling, then the runner's `ketplus_runner_peer` connector and channels against it through an in-process Relay with real DTLS on 127.0.0.1: every offer refusal in its order (unreadable and replayed ids dropped, `invalid_offer`, `not_paired`, `invalid_signature`, `offer_expired`, `peer_limit`, the job side's code), used ids taken only after the signature, the signed answer against a fixed vector, candidate limits both ways, the 20-second deadline, newest offer and newest connection replacing the old ones, the 64-attempt and 64-connection limits, frames and channel violations, transfers of exactly 4 MiB and four in a row, the bulk buffer limit, writable once, a 4 MiB transfer to the runner, an offer changed after signing that is never answered, and no SDP, candidate, TURN credential, signature or frame content in any log line |
| Desktop remote job records | `ketplus_remote_job_relay_tests`, `ketplus_remote_job_record_tests`, `ketplus_remote_job_flow_tests` | The daemon's job calls (`job.submit`, `job.get`, `job.cancel`) against a fake Relay on a loopback port: routes, bodies, refusal codes, network and 5xx failures, schema checks and the route seam; the `runs_on` label rule; the remote job record on in-memory ports with time driven by the test: placements, terminal mapping, Stop, peer loss and the 20-minute window, the ways out of `needs_attention`, runner removal, at-most-once dispatch, submit retries, restart recovery, the 64-job limit and store failures |
| Desktop remote job registry | `ketplus_remote_job_store_tests`, `ketplus_remote_job_adapters_tests`, `ketplus_remote_job_specs_tests`, `ketplus_remote_job_ledger_tests`, `ketplus_remote_job_steps_tests`, `ketplus_remote_jobs_wiring_tests`, `ketplus_remote_jobs_restore_tests`, `ketplus_remote_jobs_submit_tests`, `ketplus_remote_jobs_relay_socket_tests`, `ketplus_remote_jobs_service_tests`, `ketplus_remote_jobs_end_to_end_tests`, `ketplus_remote_jobs_stop_tests`, `ketplus_remote_jobs_snapshot_tests` | One record store contract run on the in-memory store and on the store kept in the background task registry (SQLite in a temporary directory): revisions, the first record needing an approved queued reservation with the same `runs_on` and timeout, the stored form reading back and malformed forms listed as unreadable; the event-loop job clock and pin-backed runner trust; job specs held in memory only and never written to disk; a remote task reserved at creation, never claimed, parked, given an executor or child, and refused by the registry's own change once started; every allowed record step from queued to an ended state through the coordinator, with its outbox event in the local shape, and steps outside the table, a write failing midway, the acknowledgement held for leaving `needs_attention`, prune, upgrade of an existing database and restart for each kind of record; a local task still refused the steps it was refused before; the service on a real Control client: runner messages taken from the Relay socket and given back when it stops, Git run on the thread pool and the hourly sweep |
| Remote job entry | `ketplus_remote_job_entry_tests`, `ketplus_managed_task_remote_host_tests`, `ketplus_managed_task_remote_jobs_tests` | `runs_on` labels read and stored in one form, the remote spec rules, each refusal before a task exists (no task, reservation or approval left), submit once after approval, a failed submit ending the reserved task without running it, Stop while waiting for approval or before the first record, stop, cleanup, `get` and `list` of a recorded job through the remote jobs port, a restart ending a reserved task, the runtime letting go of a cleared, replaced or outlived port, and the runner guidance in `needs_attention`; the session host runs with a fake port, and again joined to the real entry and remote jobs with a Relay and a runner the test plays: the runner's output and one completion, `needs_attention` and its acknowledgement with the task revision, the Relay's refusals and 24-hour expiry, the deadline with and without the runner, the remote jobs restarting under a running host, a daemon restart before the Relay answered, a repeated start, the owner, generation, origin and session checks, the registry capacity, a stop crossing the result, a local start left unchanged and the remote jobs refusing while not open or not registered |
| Desktop job peer sessions | `ketplus_peer_output_tests`, `ketplus_peer_session_link_tests`, `ketplus_peer_session_job_tests`, `ketplus_peer_session_output_tests`, `ketplus_peer_session_runner_tests`, `ketplus_job_peer_clock_tests`, `ketplus_remote_output_spool_tests`, `ketplus_background_output_gap_tests` | The desktop's peer session of a remote job on fake ports with time driven by the test: offers, the 10-second `hello`, label and capability checks, connection replacement and epochs, spec, spec again before `job.accepted` and `job.resume`, the runner's answers, the snapshot port and desktop snapshot failures, output into the spool with acks, resumes, gaps and spool failures, durable results, cancel, untrusted frames and log contents; and the same session against the runner's real job session over in-memory channels |
| Scheme consumer adapters | `ketplus_scheme_consumer_tests` (Lite) | Lexilla role/font style mapping, editor state parity and buffer/selection/undo preservation, terminal ANSI replacement and screen/selection preservation |
| Theme selection store | `ketplus_theme_selection_tests` (Lite) | One-time migration, revisioned atomic commits, cross-process directory watcher updates, lock and config failure recovery |
| Color scheme contract | `ketplus_color_scheme_tests` (Lite) | RGBA parsing, sparse/unknown-data round trips, structured diagnostics, embedded palette parity, deterministic resolution and locked atomic catalog lifecycle/fallback |
| Editor features | `ketplus_editor_features_tests` | CM 0.2.0 commands in the Pro shell: recent and closed tabs, Save All, command palette, Spotlight files and back navigation, split view, search and replace in files, file format controls |
| CM document modes | `ketplus_desktop_document_modes_tests` | Markdown defaults to Preview, floating mode control/menu synchronization, CM terminal search/clear/restart controls |
| Claude API key | `ketplus_claude_credentials_tests`, `ketplus_claude_credentials_settings_tests`, `ketplus_claude_sidecar_tests`, `ketplus_desktop_daemon_client_tests` | local IPC authentication, memory-backed secret store, Remove clears an unreadable or invalid API or Admin entry but not a locked one, fixed endpoint and network failures, SDK credential scope, background rotation, disconnect without credential replay, native form states; no live API or real Keychain |
| Storage preferences | `ketplus_settings_storage_page_tests`, `ketplus_desktop_daemon_client_tests` | Immediate detail saves and explicit size-target saves, independent revisions, old-daemon capability fallback, read-only loading, conflict/error reload, malformed policy, advisory usage failure, late replies, keyboard/accessibility, narrow/wide native layout; capability negotiation and no reconnect replay; real local-daemon bridge read/apply |
| Settings → Runners | `ketplus_runner_settings_model_tests`, `ketplus_settings_runners_page_tests`, `ketplus_desktop_daemon_client_tests` | runner.list and pairing answers to rows and stages (missing or mistyped fields never trusted, daemon `trusted: false` disputes), every unavailable case from its code only, Relay strings as plain text, row actions with their IPC payloads and confirmations (Esc, Cancel and Enter send nothing), one create per click, masked token cleared on claim, expiry, cancel, close and page hide and never logged, only a click approves (events and Enter never do), a close before the end rejects once (also when the token arrives late), late replies, keyboard names, light/dark and zoom while open; `runner.pairing.changed` reaches `noticeReceived`; `KETPLUS_RUNNERS_CAPTURE=<dir>` saves `runners-{list,add,confirm,empty,error}-{light,dark}.png` |
| Settings composition | `ketplus_settings_integration_tests` | Shared appearance persistence across Pro shell surfaces; Providers page |
| Atlas process/WebView | `ketplus_atlas_tests` | Atlas process and tab behavior; complete selected-port readiness, malformed/truncated banners, stop-time suppression and repair/restart with a synthetic CLI |
| Atlas shell integration | `ketplus_atlas_workspace_tests` | Desktop and Atlas composition |
| Atlas renderer consent | `ketplus_managed_atlas_viewers_tests`, `ketplus_atlas_workspace_tests` | Per-launch consent from both desktop entry points, plain-text command details, static fallback, contract/path changes, pending Stop and reopen/recreated-registry isolation; fake CLI/URL handler, no real browser/provider |
| Agent message cards | `ketplus_agent_tests` (`incomingAgentCardsClampAndExpand`, `sentAgentMessageReadsAsTheMockNote`, team block tests) | Incoming agent/system/team and outgoing cards share a selectable four-line preview; Expand/Collapse anchors the reader, expanded team blocks keep task actions; only owner input remains a bubble |
| Agent Workspace | `ketplus_agent_tests` | transport, sessions, rendering, interaction, history, fork menus |
| Copilot desktop presentation | `ketplus_copilot_presentation_tests` | platform setup guides, daemon discovery/provider identity, transcript names, Teams availability, model permissions and missing/older-daemon states; native QML offscreen at wide and narrow zoomed layouts; `KETPLUS_COPILOT_CAPTURE` saves the wide picker screenshot; no provider invocation |
| Sidebar attention | `ketplus_conversation_attention_tests` | background blocking/async questions and confirmations, answer/expiry transitions and spinner lifecycle; fake provider only |
| Background attention metadata | `ketplus_conversation_attention_daemon_tests` | real local daemon and fake provider: async question updates the inactive row without hydrating its transcript |
| Desktop zoom | `ketplus_desktop_zoom_tests` | repeated 90/100/110/125% zoom transitions, QWidget and QML geometry, dynamic rows, elision, multi-window appearance, monospace changes and merging with another window's newer settings |
| Interface font stack | `ketplus_interface_font_tests` | embedded KÉT Sans weights, Vietnamese glyphs, tabular digits, shared QWidget/QML resolver and editor mono |
| Streaming presentation | `ketplus_agent_streaming_tests` | delta coalescing, final/stop flush, scope and prepend fencing, scroll/Latest, skill refresh isolation |
| Transcript viewport | `ketplus_agent_viewport_tests` | offscreen formatting deferral, partial source, source pin while reading, prefix anchors |
| Daemon activity groups | `ketplus_daemon_activity_groups_tests` | summary privacy, output watermark, evidence barriers, bounded snapshot pages, compaction, restart, fork/delete isolation |
| Open activity group loader | `ketplus_activity_group_loader_tests` | single in-flight request, coalescing, upserts, close/reopen fencing, snapshot restart, explicit retry |
| Desktop activity group IPC/UI | `ketplus_desktop_activity_groups_tests` | authenticated summary transport, native Qt expansion, live append while open, collapse, full export and explicit raw read |
| Local transcript storage window | `ketplus_daemon_transcript_window_tests` | indexed bounded seeks, giant turns, snapshot/compaction fencing, current approvals |
| Desktop transcript windows | `ketplus_desktop_transcript_window_tests` | selected-only hydration, older pages/export, cache eviction, pure event projection, provider notices and unknown item rows; large-event reassembly refuses a transfer split into more than 4,096 fragments while exact retries, conflicts and gaps keep their results |
| Conversation navigation | `ketplus_desktop_transcript_navigation_tests` | bounded initial read-ahead and cancellation before idle publication |
| Transcript catch-up | `ketplus_desktop_transcript_catchup_tests` | retained reader on return, revision refresh, cancellation, live buffering, overflow and retries |
| Transcript row window | `ketplus_transcript_window_model_tests` | delegate retention and reentrant range changes during history reset |
| Provider limits | `ketplus_rate_limits_tests` | Codex rate-limit parsing and state |
| Chat Markdown | `ketplus_chat_markdown_tests` | raw HTML escaping, bare HTTP(S) autolinks without changing code/existing links, whole answer visible, URL hover/open and source copy in light/dark transcript delegates |
| Transcript safety | `ketplus_transcript_safety_tests` | model rich text (inline/reference images, raw `<img>`, `<style>`, `<link>`) makes no network connection, every word stays visible, links re-colour on theme change, a 4000×3000 inline image is kept at thumbnail size, the layout trace records no message text |
| Chat Mermaid | `ketplus_chat_mermaid_tests` | Markdown and Mermaid integration |
| Long-lived provider stderr | `ketplus_provider_stderr_flood_tests` | Codex/Claude/Cursor fake processes each emit two 32-MiB floods with eight mandatory stdout round trips per flood; drain and 1-MiB newest-tail bounds are observed directly, then a normal call succeeds on the same process |
| Workspace boundary | `ketplus_workspace_tests` | Pro shell with CM explorer behavior |
| Async question lifetime | `ketplus_async_question_lifetime_tests` | turn expiry, session/generation isolation, late items and stale answers |
| Local turn acceptance | `ketplus_turn_acceptance_tests` | Raw/projection/state/configuration abort and ignored writes, deferred-FK commit failure, no rejected prompt publication, independent-reader visibility, repair/retry/reopen, duplicate result replay, counter/revision exhaustion, bounded oversized-payload refusal (ADR 0076); Stop pressed while a turn is starting does not reach the next turn after that start fails; a late or repeated Codex `turn/completed` or a late `turn/started` does not end or restart the active turn; a delta after its item completed is dropped until the item restarts or the provider stops; two clients on one session (Desktop and the relay's `cloud-control-v2`): one prompt accepted and the other `session_busy`, one approval answer and the other `already_resolved`, read markers only move forward; a prompt rejected after a provider switch, by the journal or by the provider, keeps the handoff note for the retry, and a switch that cannot be journaled stays with the previous provider and thread; approvals pending when a Codex or Claude turn ends expire before the completion is published and a late answer is refused, except while a Claude background task runs; a final Codex `error` for the running turn expires them too, before the error is published, and declines later requests for that turn, while a retried error or one naming an earlier turn does not and the next turn is not fenced; a Codex request naming an ended turn is declined as `stale_turn` while the running turn's own request is offered |
| Session/provider deletion | `ketplus_provider_deletion_tests` | Failed commit releases the transaction; ignored tombstone/work/history mutations roll back; retry/reopen preserves history, conversation number, cleanup work and unrelated sessions; shared provider threads and unsupported providers remain protected; query/mid-fetch ownership failures prevent fake-provider deletion, preserve pending work/history and recover after repair/restart; malformed/unknown ownership JSON blocks queued and local deletion for both source and retained sessions |
| Provider response identity/envelopes | `ketplus_provider_response_id_tests` | Fake-provider handshake barriers, typed id isolation, valid-response recovery, exactly-once callbacks and Codex oversized responses; numeric limits; malformed result/error shape, initialization refusal, outstanding-call isolation, duplicate replies and subsequent recovery; POSIX launcher cases skip on Windows |
| Provider version probing | `ketplus_provider_version_probe_tests` | Incremental stdout/stderr cap, excessive-output termination, exit status, timeout/EOF, retry/cache, distinct concurrent executables and same-path replacement; local helper only |
| Claude/Cursor framing | `ketplus_provider_framing_tests` | Coalesced real QProcess output, 16-MiB per-line boundary, aggregate valid frames, oversized complete/partial suffixes, failure drain and fresh-generation recovery; fixture process only |
| Malformed provider frames | `ketplus_provider_malformed_frame_tests` | Fake Codex/Claude/Cursor processes: initialize and pending calls, UTF-8/binary/control/JSON failures, suffix quarantine, once-only error callbacks, payload-free diagnostics and fresh-generation recovery; an oversized Claude or Cursor line, in the initialize response or a later call, publishes one stop and one crash backoff; POSIX process fixtures skip on Windows |
| Copilot adapter | `ketplus_daemon_copilot_adapter_tests`, `ketplus_copilot_cli_tests` | Discovery/overrides, Windows wrapper policy, fake ACP stream/cancel, explicit/nested/exit authentication failures, sanitized unrelated errors, fresh-process retry, startup deadline and host routing; no provider quota |
| Copilot permissions/browser | `ketplus_daemon_copilot_tools_tests` | Fake ACP permission option IDs, no promotion to always, absent options, expiry/cancel, stale turn/process answers, forged browser identity, host approval cards, MCP setup/resume/disable, routing, image content and completion/failure/shutdown cleanup; no provider quota |
| Copilot models | `ketplus_copilot_catalog_tests` | Config/grouped and advertised legacy catalogs, model/string/output bounds, fallback, cache/coalescing/expiry, timeout/stop cleanup, host model discovery, selected model before prompt, switches across turns, empty load state and explicit setter failures; native fixture executable only, no provider quota |
| Shared ACP engine | `ketplus_daemon_acp_adapter_tests` | Provider-independent fake stdio process: initialize, MCP session creation/load fallback, assistant/tool/usage events, permission option IDs and forged MCP identity refusal, cancellation, pending-call shutdown and malformed-output restart |
| ACP capability defaults | `ketplus_provider_compatibility_tests`, `ketplus_daemon_cursor_adapter_tests` | Missing/null/wrong-type capability fields, boolean controls/reset and actual session/load suppression; fake Cursor explicitly advertises implemented features |
| Provider line scanning | `ketplus_provider_line_reader_tests` | Deep array/object streams, chunk/depth bounds, JSON escapes/Unicode, invalid metadata, dotted-key isolation, next-line/reset recovery and a fresh-child POSIX peak-memory bound; no live provider |
| Daemon protocol/storage | `ketplus_daemon_protocol_tests` | framing, journal, deduplication, approval expiry, inherited macOS access-control entries removed from state, journal, media and refs and an exposed token replaced, a second daemon refused while the first holds hour-old locks |
| Daemon autorelease pools (macOS) | `ketplus_daemon_autorelease_pool_tests` | `DaemonApplication`, the daemon's and job executor's application object, drains an Objective-C autorelease pool around each event delivery (QCoreApplication's event loop on macOS never does): posted events, timer callbacks and events on a worker thread release what they autorelease when the delivery ends, and a nested event loop inside a delivery keeps the pools balanced |
| Schedule protocol (ADR 0126) | `ketplus_schedule_protocol_tests` | schedule documents, requests and events for all four targets and run statuses; unknown fields, empty prompt, missing target and bad enums; server-owned `created_by`; timestamps must carry `Z` or a numeric offset |
| Schedules window | `ketplus_schedule_client_tests`, `ketplus_schedule_window_tests` | one client applies schedule events without a reload; the manager is edit-only, validates before update, shows daemon errors, and View › Schedules… opens one window. `QT_QPA_PLATFORM=offscreen` and `KETPLUS_SCHEDULE_CAPTURE=<dir>` on the window test save light and dark grabs of the list, empty state, editor, validation, delete confirmation, 900×600 minimum and the View item |
| Daemon challenge v2 (#85) | `ketplus_daemon_machine_challenge_tests` | relay conformance bytes/signature, strict schema and machine/key binding, no legacy retry or credential request on rejection, and real HTTP serialization/signing with an in-memory credential store |
| Control v2 primitives | `ketplus_control_crypto_primitives_tests`, `ketplus_control_crypto_parity_tests` | `ketplus_control_crypto_primitives` alone (QtCore and OpenSSL): RFC 8032 Ed25519 signing and verification, refusal of changed messages, signatures and keys and of wrong lengths, generated key pairs, base64url, canonical arrays and key fingerprints; the parity suite compares those with `CloudCrypto`'s static API on Unicode, numbers, null and nested arrays, and `CloudCrypto::sign` with a stored key against the passed-key primitive |
| Journal usage retention (#128) | `ketplus_daemon_usage_retention_tests` | final usage samples, unchanged content/order, replay/page agreement, revision fencing, latest 20 actual turns including sequence gaps, raw-event acknowledgment, active/open/recent/question/approval/generation guards, bounded scan progress, transactional rollback and nonblocking writer contention with timeout restoration; legacy inline generated-image pixels removed one large payload per pass with prompt and path kept, only after acknowledgement and outside the latest 20 turns (ADR 0098), and with a session host, a 512 px JPEG preview of a real PNG kept as a media reference (ADR 0101); inline tool-result images replaced by `history_retention` placeholders in place, including rows trimmed by an earlier text pass and chunked payloads that contain an image, while text-only oversize payloads stay and probing a backlog of them is charged to the per-pass budget (ADR 0099) |
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
| Team context reset (ADR 0114 §11) | `ketplus_team_context_reset_tests` | coordinator with a fake runtime: task boundary for developers and reviewers, threshold for every role, `threshold` and `never`, a threshold of 0, a new thread adopted, settings validated and kept, briefing within 4 KiB and the leader's 20 open tasks; then the scripted fake providers: the leader's team turns start fresh past the threshold with the transcript note and briefing, user turns never do, and a transcript that cannot be written keeps the thread |
| Held-message Desktop release (#130) | `ketplus_agent_messaging_tests`, `ketplus_provider_transcript_tests`; Agent Workspace `heldMessageRequiresExplicitClickAndFencesDuplicates` | scoped authorization, all three guards, whole-batch validation, busy receiver, agent/remote refusal, expiry/deletion/generation fences, command replay, recovery before/after the durable input, explicit click/retry and typed notice projection; fake providers only |
| Agent messaging (ADR 0027) | `ketplus_agent_messaging_tests` | number allocation, uniqueness, restart stability and legacy-journal backfill; idle delivery and automatic reply from the final answer; busy receivers batched at turn end; `urgent` steering of a running turn; hop, chain and hourly holds released with the next user turn, the per-sender backlog and the per-turn delivery budget measured on the serialized message (escapes and sender title), with oversize messages refused at send, and the 24-hour request expiry; `#NNNN` references; token, number, size and reply validation; deletion quarantine and token revocation; per-provider `ketplus_agents` injection (Claude `mcpServers`, Codex dotted per-thread config, Cursor ACP); agent-token IPC scope without broadcasts; MCP tool mapping; `sent` and `replied` notices and the running automatic turn's sender in `session.list`; the number and that sender sealed into the phone's session metadata (`ketplus_daemon_cloud_privacy_tests`); Desktop notice text (`ketplus_provider_transcript_tests`) and the sidebar "from #N" label (`ketplus_workspace_tests`). Fake providers only; the one authorized live run is recorded in ADR 0027 |
| Conversation fork | `ketplus_daemon_session_fork_tests` | `threadFork` gating, Codex/Claude native fork, transcript copy at the same cursors, idempotent retry, active source isolation and unknown refusal, next turn resumes the forked thread; a fork of an archived conversation is unarchived and outlives its source's expiry (ADR 0100) |
| Daemon Cursor adapter | `ketplus_daemon_cursor_adapter_tests` | ACP turns, login/install errors, approvals, an approval from a restarted process that reuses an unanswered question's request id is answered as an approval, missing/false/mistyped `loadSession` skips `session/load`, `agent models` fixtures |
| Provider CLI compatibility | `ketplus_provider_compatibility_tests` | version parsing and statuses, manifest, Homebrew update command, version probe cache, outdated CLI blocked, untested CLI journaled once, `provider.runtime_status`, dialects and capabilities, `-32601` narrowing and steer gate, unknown Codex input (ignored notifications counted, protocol errors, declined requests, unknown item types) |
| Copilot CLI policy | `ketplus_copilot_cli_tests` | injected macOS/Linux/Windows discovery, override precedence/refusal, PATH and fallback order, runnable-file checks, native and Windows wrapper invocation, authentication versus network/model/policy errors, and daemon `--copilot` parsing through `--build-info`; never launches Copilot; a local fake `.cmd` runs only on Windows |
| Provider readiness | `ketplus_provider_readiness_tests` | version check before probes, Claude `runtime_missing`, probe `since` kept per state, reported failures outrank an account until a working turn, limits until reset, owner-only persistence across restarts, Codex `account/read` and Cursor `cli-config.json` probes, per-platform commands, `runtime.error` codes (structured first, the #170 messages), Check again keeps an unexpired limit; in `ketplus_provider_compatibility_tests`, a limited Codex turn is coded and keeps its reset, and a working turn clears a persisted failure |
| Provider setup guide | `ketplus_provider_setup_guide_tests`, `ketplus_settings_integration_tests`, `ketplus_agent_tests` (`providerSetup*`, `claudeSignOutGuidesWithoutHoldingTheMessage`, `aHeldMessageGoesWithItsDraft`, `modelPickerOpensTheProviderSetupGuide`) | in the Agent Workspace, the guide above the composer holds the message until a fresh read lists the same provider without a guide (not a stopped daemon, not a guide that stops blocking), and lets it go when the draft is edited or another conversation opens; Hide and the model button, Type in Terminal, Check again with `still`; a coded failed turn shows the guide with the provider's words and Retry, and turns back into a notice once the user writes again; Claude's sign-out never holds a send; Model & context keeps a guided provider's tab without models, shows its banner, keeps Apply off there, and Show setup steps opens the guide dialog, which closes once the provider works; `KETPLUS_SETUP_CAPTURE_DIR` saves the composer, transcript, model picker and dialog captures; `everySetupStateRendersInBothThemesAndZooms` renders all ten #169 states in light and dark at zoom 1 and 1.25, and `KETPLUS_SETUP_MATRIX_DIR` saves them as `<state>-<theme>-<zoom>.png` to set beside the mock's `--screen=workspace --state=setup-<state> --zoom=… --screenshot=…`; `anAccountChangeClearsAReportedFailure` (provider compatibility tests) checks that Codex `account/updated` drops a reported failure; guide ids follow the mock for every state and error code, the daemon's commands and the provider's own words, empty rows left out, the limit reset text, a turn error over readiness; in Settings → Providers, the guided badge and card, Copy and Type in Terminal (never run), Check again sends `recheck` and shows `still`, a fixed provider loses its guide; `KETPLUS_SETTINGS_CAPTURE_DIR` saves `providers-setup-{light,dark}.png` |
| App-wide toasts and update notice | `ketplus_settings_integration_tests` | status messages outside the editor show as the kit toast near the bottom, never the idle `Ready`; the sidebar reminder reads Review and install, then Restart to install; `KETPLUS_OVERLAYS_CAPTURE=<dir>` saves `toast.png` and `update-pill.png` to set beside `KetPlusMock --open=toast` and `--screen=workspace`. The app toast keeps the neutral icon on purpose: a status message may be a failure, while every mock toast is a confirmation |
| Waiting for you (#125) | `ketplus_awaiting_reply_tests`, `ketplus_conversation_attention_tests` | the fake Claude `ask-approval` and `needs-approval` prompts set `awaiting_reply`; a plain summary does not; it survives a daemon restart and clears on a new message and `session.mark_done`; the desktop row reads AwaitingReply after `turn/completed` and clears on the next turn; the Dock badge counts a worktree once across windows |
| Tag catalog (#429, ADR 0117) | `ketplus_tag_catalog_tests`, `ketplus_desktop_session_discovery_tests`, `ketplus_agent_tests`, `ketplus_workspace_tests`, `ketplus_daemon_cloud_privacy_tests` | daemon: defaults until saved, restart persistence, invalid catalogs refused, renames kept and removals dropped on every conversation; sealed phone metadata carries tags and awaitingReply. Desktop: lists no tags, takes another client's `tags.changed` without an error or a warning, and the header and sidebar draw no user tag, Add tag, Manage tags… or tag filter |
| Header openers and composer suggestions | `ketplus_agent_tests` (`changesPaneListsTheWorktreeChanges`, `composerPlaceholderTracksCaret`, reviewer picker), `ketplus_agent_shell_tests` (`headerChangesOpensTheDetailsPaneChangesTab`) | the header's Changes opens the details pane's Changes tab (the worktree's uncommitted files, each opening its diff, and Open Source Control); Changes, Terminal and Report stay in the header with the pane open, beside its Open content menu; Open tabs lists the tabs and Hide content pane keeps them; the Reviewer menu names each provider's default model and lists Codex as unable to review (ADR 0067); `KETPLUS_CHANGES_PANE_CAPTURE` and `KETPLUS_MENTION_CAPTURE` save the Changes tab and the file mention popover, to set beside `KetPlusMock --screen=workspace --open=mention|agent-mention|skill|slash` |
| Cursor catalog output | `ketplus_cursor_catalog_bounds_tests` | stdout/stderr draining before exit, shared 1-MiB boundary/excess, unsuccessful exit, coalesced callers/cache, timeout after output and failed launch; self-hosted CLI fixture only |
| Codex protocol contract | `ketplus_codex_contract_tests` (Node) | declared Codex usage exists in every tested schema snapshot; daemon code names only declared methods |
| Relay browser sign-in | `ketplus_daemon_browser_sign_in_tests` | relay paths appended to a base path, loopback redirect, PKCE S256 challenge, wrong state/path refused, one-time callback |
| Daemon lifecycle/IPC | `ketplus_daemon_lifecycle_tests` | authentication, protocol minor and features, reconnect replay, isolation, fencing; failed stream ordering, bounded recovery queues, deletion guards (active turn, live background task)/shutdown and SQLite writer-lock retry; `session.tags.set` validates, deduplicates and keeps tags across restarts and row rewrites (ADR 0107); `session.set_archived` keeps the first archive time across restarts and row rewrites, and archived conversations are deleted after 30 days one per pass, oldest first, skipping a busy one (ADR 0100) |
| Daemon upgrade | `ketplus_daemon_upgrade_tests` | version advertisement, idle replacement, active-turn deferral |
| Desktop daemon attach | `ketplus_desktop_daemon_client_tests` | detached UI lifetime, discovery, normalized transcript replay, negotiated features, provider capabilities, fork through the daemon; in-flight writes failed rather than sent again after a disconnect, oversized/zero/malformed daemon frames drop the connection, one replacement request to an older daemon and keeping a daemon that refuses |
| First-turn remote attachments (ADR 0122) | `ketplus_session_preparation_store_tests`, `ketplus_session_preparation_host_tests`, `ketplus_relay_preparation_host_tests`, `ketplus_machine_preparation_tests` | Private allocation/retry, atomic activation/cancel, restart/expiry and bounded receipts; authenticated Relay routing and creator-key barrier before ACK; encrypted staging through the first fake-provider turn; offscreen composer retry, visible/removable pending files during preparation/refusal, text-only recovery, late cancellation after view destruction, restart-safe durable cancellation and old-hosted/old-daemon fallback. Run the existing Machine attachment/conversation and SSH/Relay backend suites too. |
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

Managed Atlas consent regressions run in `ketplus_managed_atlas_viewers_tests`;
the fake-CLI cases use a Unix shell and skip on other platforms. The normal
ctest lane renders offscreen and intercepts browser URLs. For native permission
dialog acceptance on macOS, use the secondary work display:

```sh
QT_QPA_PLATFORM=cocoa KETPLUS_DISABLE_DAEMON_AUTOSTART=1 \
KETPLUS_ATLAS_PERMISSION_CAPTURE_DIR=build/dev/atlas-permission-captures \
  build/dev/ketplus_managed_atlas_viewers_tests liveThemeZoomAndLayout
```

The native lane skips without a secondary display. It checks live light/dark/
system theme, 90/100/110/125 percent zoom and narrow/wide dialog bounds, saving
light/dark captures when requested. These checks do not start a real browser,
project renderer, provider, or installed application bundle.

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

Review guard changes (ADR 0067 profiles) run `ketplus_claude_sidecar_tests`, which
includes `review-guard.test.mjs` and `review-profile.test.mjs`, and the daemon's
`ketplus_turn_review_tests`, `ketplus_team_review_tests` and
`ketplus_agent_messaging_tests`. The sidecar tests use a fake SDK that runs every
command and edit the guard allows, so a wrong allow changes the test checkout.

Bounded review policy changes additionally cover `ketplus_team_review_policy_tests`,
`ketplus_team_finding_resolution_tests`, `ketplus_team_finding_routing_tests`,
`ketplus_team_verdict_tests`,
`ketplus_team_review_place_tests` and `ketplus_team_language_tests` (ADR 0132).
Fake-host receipt tests establish consumer gates; the finding routing suite checks
actual three-revision Git proofs, authenticated host receipt closure and attacks
using ignored gitlinks/replacement trees. Required full Pro checks still apply.
Routing lifetime coverage in `ketplus_team_routing_tools_tests` uses an active
team, the real producer/store and authenticated decisions: unrelated notes remain
valid, task/settings races fail closed, concurrent replies are independent, and
an injected monotonic clock tests expiry before deterministic team-local eviction
and duplicate-ID nonrenewal (ADR 0133).

Team reply/worktree diagnostics use `ketplus_team_reply_tests`. Its existing
owner-worktree and resume-Git assertions retain their conditions and timeouts;
on failure they read up to eight `team.task.get` pages (`history_limit=6`) and
report bounded synthetic task-history/worktree-error metadata,
owner state/context/turn counters and pending approval kinds. Approval arguments,
prompts and auth fields are excluded; a failed approval-page read is distinguished
from an empty page. History totals come from the page API, with separate read
status/error metadata; a summary or failed read must not appear to be empty
history. `diagnosticsExcludeInputsAndBoundHistory` checks the projection and
read status; `collectorReadsRealWorktreeFailureHistory` creates a real fixture
worktree-switch refusal and verifies its paged history/error through the actual
collector. For order-sensitive failures, preserve the failed
log and run a bounded sequence of relevant predecessors (`team_flow`, `team_goal`,
`team_title`, then `team_reply`) sequentially, recording source/binary identity and
resource/concurrency metadata. A passing isolated run does not classify an earlier
failure. Pair a reproduced failure with the integration baseline under the same
conditions; do not relax assertions/timeouts or loop the full suite blindly.

A team reviewer's verify turn writes only in its scratch directory (ADR 0067). Build there:
`cmake -B <scratch>/build`, `xcodebuild -derivedDataPath <scratch>/DerivedData
-clonedSourcePackagesDirPath <scratch>/SourcePackages`, and run a script that writes into
its own tree (for example a font or asset generator) in a copy of the checkout in the
scratch directory. The turn points the usual cache variables into the scratch directory.

Atlas tunnel completion (`ketplus_atlas_live_tunnel_tests`, ADR 0084) exercises
per-frame, half-window and delayed full-window credits against a synthetic
loopback origin. It requires complete binary response bytes and exactly one EOF
without a final credit grant. Run the runtime, cloud-privacy and remote-control
client targets alongside it; no real CLI/provider or relay is required.

`ketplus_atlas_access_tests` checks that a live Atlas runtime stops when its device is revoked
or its conversation generation is deleted, with and without negotiated peer transport, and
that other devices, conversations and generations keep theirs. It also checks that a device or
machine revocation moves the access fence an in-flight `atlas.start` checks before it creates a
runtime.

Daemon Atlas readiness (`ketplus_atlas_live_runtime_tests`, ADR 0083) includes a
Unix synthetic Python CLI: wrong/longer port, embedded/partial/truncated banners,
LF/CRLF/slash/split compatibility, stop-time output and a one-shot start callback
when the process exits immediately after readiness. Invalid-output repair starts
a fresh owned runtime. These cases do not opt into a real CLI, renderer or
provider. Header changes require the full Pro matrix; keep the existing real
KetAtlas lane opt-in and report its skip separately.

## New team dialog layout

`ketplus_team_dialogs_tests` uses a fake daemon and renders the real Qt form. Its
geometry cases cover two columns at 1280 × 900 and 1024 × 420, a single column at
480 × 640, English and Vietnamese, visible footer buttons, compact wrapped help,
and keyboard access through both columns. Existing cases resize and zoom the
open dialog through 100%, 125% and 150%. `ketplus_team_goal_form_tests` covers
mentions, multiline entry and submit shortcuts; `ketplus_team_page_language_tests`
covers language behavior. No provider or real team is started.

For local PNGs and geometry logs, run `ketplus_team_dialogs_tests
columnsReflowAndKeepTheFooterReachable` with `QT_QPA_PLATFORM=offscreen`,
`KETPLUS_DISABLE_DAEMON_AUTOSTART=1` and `KETPLUS_T15_CAPTURE` set to a local output
directory. Keep generated captures out of the source tree.

`ketplus_team_provider_dialog_tests` (T-3) renders Change provider… against a fake
daemon: the provider select and its company-first labels, every member checked by
default, a member left out, a reviewer that stays without Full access or moves with the
team's Full access acknowledged, each member's
result from `team.provider.set` and a refusal, in light and dark at 100%, 125% and
150% in a 1024 × 640 window, with the theme switched while the dialog is open. An
unready provider's tooltip names it. No provider or real team is started.

The [layout verification record](evidence/new-team-dialog-layout.md) retains the
full Pro matrix results, initial failures, one isolated retry and platform skips.

## Minimum verification

Linux Pro packaging changes run `tests/packaging/test_linux_desktop_packages.py`,
the offscreen bundled-browser/Claude-adapter tests and production build-info
checks through `packaging/linux/build-desktop.sh`. Verify both actual package
formats in `Dockerfile.desktop-smoke` with `--network=none`, no SDK and fresh
home/config/data directories. Run each native architecture separately; an amd64
result is not ARM proof. See `linux-desktop-packaging.md` for the build commands
and outstanding publication gates.

Update-preparation changes run `python3 tests/packaging/test_update_archive_cli.py`
for invalid notes/version, output collisions, cleanup after a failed signing step,
the existing feed as generator input and its mode, no rollback after the feed is
replaced, and repair/retry using synthetic
Apple/archive/appcast tools. No signing key or network is used.

macOS Sparkle build configuration changes run
`python3 -I tests/packaging/test_macos_update_build_policy.py` (also registered as
`ketplus_macos_update_build_policy_tests`). The test uses the actual CMake
presets, a synthetic Sparkle framework layout and fixture Mach-O files for both
architectures. Changes to the compiled update feed also run the standalone
`ketplus_updater_feed_policy_tests` with the pinned Sparkle SDK as described in
[Desktop update QA isolation](#desktop-update-qa-isolation).

Update publication and rollback changes run
`python3 -I tests/packaging/test_update_r2_cli.py`. The fixture uses a local
S3 CLI stub for conditional writes, archive collisions, scope and QA rejection,
upload ordering and metadata, failure recovery, and appcast snapshot restore.
No Cloudflare account, credentials, or network is used.

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
results), `ketplus_remote_control_projection_tests` (sealed metadata projection
with tampered and unpinned rows, transcript paging across a large event, a tampered
restore response, a changed host key version and an unpinned host key, against the
synthetic loopback relay) and `ketplus_remote_ssh_tunnel_tests` (loopback `sshd` with throwaway
keys: host-key accept, reject and mismatch, auth failure, missing daemon socket
and reconnect), `ketplus_remote_ssh_backend_tests` (SSH machine backend states,
identity fail-closed and re-trust, machine store) and
`ketplus_remote_ssh_daemon_integration_tests` (a real `ketplus_daemon` with
FakeCodex behind the loopback `sshd`; see [`ssh-transport.md`](ssh-transport.md)). The normal full Desktop suite still covers the shared build/interface
changes. Also run the standalone Release cloud-privacy characterization, then
stage/install and check the runtime outside the Qt SDK; see
[`linux-headless-daemon.md`](linux-headless-daemon.md).

The machine picker geometry regression also mounts the real ActivityBar at 240 px
in light/dark and checks all four connection states remain readable before the
trailing route/name is elided in the closed label; the popup stays name-first. `ketplus_remote_workspace_tests` checks that the
signed-out Linux section shares the introduction's 24 px column at wide/narrow widths.

`ketplus_machine_end_to_end_tests` (macOS Desktop, offscreen) is the #419
acceptance: the real `MainWindow`, given the app's one `LinuxMachines` set
through `useLinuxMachines()` as `main()` does, with SSH and Relay backends over
the fixtures, selects a Linux machine in the picker through the window's own
Connections wiring, starts a conversation in a remote
worktree, answers an approval, sees the transcript update, survives a stopped
daemon or relay with Reconnecting/Offline shown and automatic recovery, and
returns to This Mac. The SSH case needs `sshd` (it skips without one) and uses a
real `ketplus_daemon` with FakeCodex; the Relay case uses
`tests/daemon/SyntheticRelay.h` and in-memory credentials. Set
`KETPLUS_E2E_SCREENSHOTS=<folder>` to save the Connections page, the window and
the open machine picker for each route (the PR evidence under
`mockups/qt/evidence/linux-e2e/`).

`ketplus_remote_relay_backend_tests` drives ADR 0119's `RelayMachineBackend`
against a loopback relay whose replies are shaped like the deployed relay's
session, worktree and command DTOs, with a synthetic host that decrypts each
command, runs it once and signs its result: a full turn with an approval and a
question, a session created in an empty host-verified worktree, per-worktree
verification (a mismatched scope, an ungranted worktree, the bound), an outbox
retry of the same ciphertext across a simulated restart, offline and
reconnect, and a revoked credential that fences an in-flight result. It also
covers lazy, cached and bounded worktree verification that never delays a
listing, a backend destroyed mid-request, live events resuming after a fence
during an events poll, requests held while reconnecting, a failure reported
only for a command that is never sent (one the relay holds is queued and
completes once), and a result applied only after its completion is recorded
and re-read while the link stays up. A `pendingApprovals` read never reaches
the outbox or a restart, a refusal without a code fails only its command while
a 503 without a code keeps it for the link, event polls stop for twenty closed
conversations (a read does not restart them) and stay bounded to the one that
is open, and a worktree granted after a `worktree_not_granted` verdict appears
once the verdict expires.
No network, account or provider is used.

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
- Journal startup (ADR 0096): `ketplus_daemon_journal_startup_tests` checks that
  legacy turn-number, title and turn-start backfills run only below format 1.
  It also checks that activity indexing runs after open in passes bounded by rows
  and payload bytes, that a failed, busy or marker-losing pass keeps its marker,
  that a new event waits for its session's backfill, which goes first, that a
  partly indexed group is not published and that drained rows match live indexing
  (ADR 0097). Opt in to the 300k-row timing over 20k sessions and over one session
  with `KETPLUS_RUN_STARTUP_BACKLOG_TESTS=1`; the first open, an append to a
  marked session, each activity backfill pass and the second open must each stay
  under 250 ms.
- Activity files backfill (ADR 0102): `ketplus_daemon_activity_groups_tests`
  checks that a journal indexed before `activity_files` keeps its groups hidden
  until their files are listed, that a failed pass keeps its marker, that edits
  completed during the listing are kept, and that forks and deletions carry or
  remove the marker.
- Journal migration recovery: `ketplus_daemon_journal_migration_recovery_tests`
  migrates a pre-format journal (60k rows in 2,000 sessions, without five later
  columns, the turn indexes and the activity tables) on a nearly full 256-MiB disk
  image (macOS `hdiutil`; skipped without it) and kills a child process at six
  times during the migration. The open must report the full disk, leave an intact
  journal without the format marker, and finish on the next open with the same
  schema definitions and table contents (worktree catalog without its clock) as
  an uninterrupted migration. The child opens only `LocalJournal`; startup
  compaction has its own interruption tests in `ketplus_daemon_journal_space_tests`.
- Media retirement: `ketplus_daemon_protocol_tests` leaves media behind an
  interrupted release and an unreferenced write, and expects startup retention to
  delete both while keeping media a live session shares, a foreign file and a FIFO
  with a canonical name; a media root that links to another directory is left alone.
- Media reads: `ketplus_daemon_lifecycle_tests` sends 24 deferred previews at
  once and expects 8 `busy` answers and 16 previews, then a free slot again;
  `ketplus_daemon_protocol_tests` gives a viewed-image path that is a FIFO and
  fails if reading it blocks for 2 s.
- Journal query audit: `ketplus_daemon_journal_query_audit_tests` traces every
  statement of 26 journal operations (append, transcript reads, activity pages,
  large-event fragments, fork including its snapshot reader, delete and purge,
  backfill, a late changed-files pass over 100k items in five groups, retention and
  garbage passes) on a conversation with 100k events of its
  own, next to 2,000 other sessions with 200k events and a conversation whose large
  tool results include three orphans. It fails a statement with more than 5,000
  full-scan steps, an automatic index or 200k VM steps. It reads SQLite's counters
  through the driver handle, so it is built only when CMake finds SQLite3 and skips
  when the driver uses a different SQLite build. Statement counts do not bound bytes,
  memory or the total work of an operation; media files are outside it.
- Journal format (ADR 0095): `ketplus_daemon_journal_space_tests` refuses a newer
  journal in both startup compaction and `LocalJournal` with byte-identical file
  contents, including a newer format only in an uncheckpointed WAL (written by
  `python3`; skipped without it), keeps a newer journal's `.compact` artifact,
  refuses a negative format, and records the format for new and pre-versioning
  journals.
- Staged fork copy (ADR 0094): `ketplus_daemon_session_fork_tests` covers one
  read snapshot while the source changes, byte-exact malformed TEXT, interrupted
  copies recovered as purge work at open, ignored purge/staging writes on abandon
  and at open, duplicate destinations and source
  deletion during a host fork. The 1k/2049-row fixtures assert the ADR 0094
  per-pass limits (2048 rows or 8 MiB), progress and bounded completion. Opt in
  to 100k/300k characterization with `KETPLUS_RUN_FORK_BACKLOG_TESTS=1`.
  Begin, slowest-pass and finish timings remain diagnostic; a shared runner's
  filesystem/scheduler latency is not a correctness threshold.
- Bounded history purge (ADR 0093): `ketplus_provider_deletion_tests` covers an
  ignored purge marker, tombstone retention while purge is pending, a stalled
  pass, an ignored marker delete, event-loop draining and reopen. The opt-in
  backlog probe asserts the delete command stays under 500 ms and each purge
  pass under 250 ms. `ketplus_daemon_cloud_privacy_tests` covers 413 retry, late
  202 and a fresh upload of a deleted generation's residual history.
- Paged live catalog (ADR 0131): run `ketplus_provider_deletion_tests` (bounded,
  complete, ordered pages over the frame limit; malformed requests) and
  `ketplus_desktop_daemon_client_tests` (refresh and discovery read every page).
  `ketplus_sidebar_scale_tests` covers building only the rows the sidebar shows.
  `benchmarks/run-large-workspace.sh` measures the real app with a large install.
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
  `agentQmlUsesBoundedAnimationsAndKitPrimitives` rejects infinite animation loops,
  unexpected frame animations, and the removed local metrics/icon/tooltip resources.
  `measurementTimerStopsAndRestartsForNewRows` exercises measurement exhaustion,
  layout changes and window visibility. The row-measurement suite checks that
  resource-backed bodies do not starve text and that a new layout re-enables work.
  `ketplus_interface_font_tests::agentThemeUsesKitAndEditorMono` changes the
  shared theme and zoom without a host update and checks the rendered header,
  page colour, profile typography and configured mono font. It runs first in its
  process so earlier font tests cannot preload KÉT Sans and hide a startup bug.
  It also checks code-frame/caret alignment, table header baselines and the brand
  image bounds after zoom changes. `markdownTableAfterCodeKeepsHeaderCells` covers
  Qt Markdown's leading empty cell paragraph and preserves intentionally empty
  headers across repeated formatting. Set
  `KETPLUS_THEME_AUTHORITY_CAPTURE=<prefix>` in its native Cocoa run to capture
  light/dark at 1/1.25 zoom. These captures cover the theme-authority regression;
  the complete app/mock state matrix remains a separate acceptance item.
  These checks do not replace the Apple Silicon foreground/background CPU
  measurements required by #126 or the app/mock comparison required by #136.
- Performance-sensitive behavior: Release build and the matching script under
  `benchmarks/`; record hardware and raw results.

### Native idle CPU (#126)

Run this manual check for native UI acceptance and before release. The dedicated
fixture mounts the production Agent Workspace with three synthetic conversations
and a short transcript. It uses temporary preferences and no provider or daemon.
It measures the UI surface, not the complete desktop/daemon process topology.

```sh
cmake --preset release -DKETPLUS_BUILD_BENCHMARKS=ON
cmake --build build/release --target ketplus_agent_idle_benchmark
python3 benchmarks/run-agent-idle.py build/release /tmp/idle-running-active --capture
python3 benchmarks/run-agent-idle.py build/release /tmp/idle-running-background --visibility inactive
python3 benchmarks/run-agent-idle.py build/release /tmp/idle-no-work --work idle
python3 benchmarks/run-agent-idle.py build/release /tmp/idle-hidden --visibility hidden
python3 benchmarks/run-agent-idle.py build/release /tmp/idle-minimized --visibility minimized
```

`--visibility minimized` minimizes the window a second after `ready` and expects no
held clock. For captures, the fixture binary also accepts `--open=mode`, `model` or
`limits` to open that composer panel first, like the mock's `--open`, `--open=reviewer`
for the header's Reviewer menu, `--open=composer` to focus the input, or
`--open=welcome` for an empty conversation like the mock's `workspace/welcome`. `--open=viewer` opens the
image viewer on the mock's sidebar capture, and `--open=tasks` shows the mock's three
background tasks (running, completed and failed) like its `workspace/running` strip. With `--open`, the
active conversation is the mock's Claude Opus 5.5 conversation with its fixed catalog and
usage (context, session cost and tokens, and three plan limits); runs without `--open`
keep the default CPU workload. `--transcript=claude`, `codex` or `cursor` replaces the
messages with the message rows of the mock's `workspace/<provider>` transcript, projected
from synthetic journal events through `daemonWindowMessages` (`--transcript=error` is the
mock's `workspace/error`: a prompt, a `runtime.error` and the failed turn end), and
`--scroll=0` to `1`
positions the transcript like the mock's `--scroll`; `--expand-groups` opens every
activity group first. `--display=secondary` (or a screen index) places the window like
the mock's `--display`, so review windows stay off the main screen, and `--hold` keeps
the window open for review without measuring. Under `QT_QPA_PLATFORM=offscreen` a
`--capture` run only saves the window and exits. It is not a CPU measurement, but it
lets a locked or headless Mac compare against the mock's offscreen `--screenshot`, both
at 1×.

Use an Apple Silicon Mac with a 2× display. The macOS app bundle lets computer use
identify the fixture separately from the installed app. After `ready`, focus the
fixture for `active`, or another native app for `inactive`. Keep that state until
the result; do not type, hover or scroll during measurement. The fixture waits for
three stable seconds, stops its readiness polling, then samples for 12 seconds.
Optional captures finish before a second warm-up period. Changing application
state during a visible-window sample invalidates it. A readiness timeout is not a
low-CPU result.

The runner saves JSON events, environment metadata, stderr and the requested
`top -l 5 -s 2 -pid <pid>` samples. Use a new, empty output directory for every
attempt, including invalid ones; the runner refuses a directory that already has
files. It stops the fixture and reports a collection failure when the run exceeds
`--timeout` (150 seconds by default) or `top` did not record all five samples for
the fixture process. The fixture also invalidates a sample when the window is
resized, minimized, moved to another screen or changes device pixel ratio while
it is measured, and checks the fixture conditions again at the end. Compare the four interval samples after top's
initial sample, alongside the fixture's process-CPU and rendered-frame counts.
Check `fixtureValid` and `valid`: Cocoa/Metal, 1440×900 at 2×, three realized rows,
stopped transcript measurement, expected activity-clock ownership and stable
window state are required. Working QML sidebar rows hold the animation clock
beside the running transcript, so a running sample reports three
`sidebarClockHolders`. The desktop shell draws its sidebar with Widgets
`ket::ConversationRow`, which this fixture does not measure.

The #126 targets are ≤7% running in front or behind another app, and
approximately zero with no running work or when hidden or minimized. They were
relaxed from 5%/1% on 2026-11-08 after the native measurement, because each tick
of the kit's shared clock redraws the whole 2× window. On 2026-11-09 the
background target moved from ≤2% to ≤7%: the kit (ad86196) now keeps 20 Hz behind
other apps instead of 4 Hz, which stuttered visibly on a watched secondary display.
On that DELL display at 1× (so `fixtureValid` false), with three working sidebar
rows and the transcript, running behind another app measured 3.4–6.6% over three
runs, up from about 1.6%. A qualified 2× sample at the new rate is still to be
recorded. Earlier, at 4 Hz behind, `feat/new-ui` 3a9117b8 with kit d476382 measured:
- running in front 2.1–5.3% over three runs;
- running behind another app 0.5–1.0%;
- no work 0.2%;
- hidden or minimized 0.04%.

All samples were valid, on an M1 Pro built-in display at 2×. Foreground samples
vary between runs, so record at least three. Also run `--work idle --visibility inactive` and
`KETPLUS_REDUCED_MOTION=1` when checking stop conditions. The sample must show no
new frames when its activity and measurement timers have stopped. Record invalid
attempts and unmeasured states; this fixture alone does not qualify Atlas Widgets
or every application state.

Automated stop checks cover the rest of #126:
- the QML scan in `agentQmlUsesBoundedAnimationsAndKitPrimitives` rejects
  `Animation.Infinite` and any `FrameAnimation` outside the self-stopping
  allowlist;
- the transcript measurement timer, row-window growth and wheel glide stop on
  their own;
- hidden, minimized (alone or inside a main window) and reduced-motion views hold
  no clock;
- in the daemon, the approval expiry timer runs only while a request can expire,
  and the remote-peer tick only while a peer is connected
  (`expiryTimerRunsOnlyWhileARequestCanExpire`,
  `helloEpochSupersedeAndRevocation`).

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

The codec/tunnel suite checks tampering, replay, direction/channel binding, credit-window crossing, binary EOF integrity and unregistered-origin refusal. The runtime suite checks synthetic CLI readiness and startup cancellation. Its optional real-CLI case serves static project files, verifies live reads after changing source, reattaches with fresh keys and a stable viewer port, and checks process cleanup. It does not authorize a project renderer. `ketplus_atlas_access_tests` exercises paired-device static starts with a fake CLI, including refusal/cancel assertions and fresh starts after Stop. These remote assertions are not a renderer-permission API.

Companion relay and mobile acceptance procedures are maintained privately. The public fixture executable remains available for native daemon tests.

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

### Shared background-task presentation

Run `ketplus_background_task_transport_tests` and `ketplus_agent_tests` after
changing task presentation. The transport suite uses a private local protocol peer,
not a daemon or provider: it covers registry pages beyond 16 rows, opaque cursors,
stale-cursor refetch, negotiated control/read permissions, cleanup revision payloads
and offline refusal. Agent Workspace tests `backgroundTaskBar`,
`managedTaskCleanupRequiresUserConfirmation`,
`managedTaskOutputFencesRequestsAndKeepsCursors`,
`managedTaskOutputRetriesAfterReadError` and `managedTaskStatesRender`
cover legacy compatibility, non-dismissable cleanup warnings despite a successful
root exit, explicit revision-fenced user confirmation, duplicate suppression,
read-only/offline/recovery, keyboard focus during registry loading, live theme/zoom
switching, output pagination/cache/request fencing, a failed output read followed
by a real Retry click and successful recovery (disabled offline, without read
permission or while pending), and all shared
states with 25 tasks. The render cases use light/dark at 900px and a 390px content
surface at 125% zoom, with the product sidebar hidden as in the embedded workspace.
`KETPLUS_BACKGROUND_CAPTURE_DIR` saves their Qt Quick offscreen captures. These
checks consume no provider quota and do not verify actual executor cleanup.

### Claude background tasks

The shared MCP/provider surface has a narrow offline Qt target:

```sh
cmake -S tests/daemon/agent-background-tools -B build/agent-background-tools -G Ninja
cmake --build build/agent-background-tools
ctest --test-dir build/agent-background-tools --output-on-failure
node --test tests/daemon/claude-sidecar/managed-background.test.mjs
```

It checks owner-free schemas, exact request-key/spec/cursor forwarding,
permission errors and refusal to expose local cleanup acknowledgement. The
sidecar fixture checks native Agent/Task/Workflow and explicit background denial
before permissive approvals, settings composition and review-hook preservation.
All providers use the existing injected conversation-bound MCP server. Its read
credential is insufficient for mutation: the host supplies a separate immutable
origin capability through MCP bootstrap, never through prompt/model arguments.
The narrow target also tests this capability state machine and credential capture.
Without a bootstrap mutation credential, MCP discovery omits start/stop while
retaining list/get/output. Concrete Cursor/Copilot Host fixtures verify that
capability opt-in still rejects start/stop until the exact epoch ACK, and that
review revocation removes authority. An unverified adapter remains a negative
control even after ACK; these tests never launch provider binaries.

For an explicit, no-inference check against an already installed SDK 0.3.322,
set `KETPLUS_CLAUDE_SDK_DIRECTORY` to that SDK package directory and run:

```sh
node --test tests/daemon/claude-sidecar/background-sdk-offline.test.mjs
```

The tests skip unless opted in. They yield no prompt and request only startup,
MCP refresh and background control, verifying rejection even when project settings try to
reenable background tasks. An earlier env-only configuration failed this test;
the host settings overlay is required. A synthetic MCP worker is removed and its
actual PID exit confirmed before its replacement starts; a removal ACK alone is
insufficient. This is not a live-model Bash/subagent
execution or timeout test. The normal sidecar fixture suite uses fake SDKs and
requires no installed SDK, provider account or quota.

The daemon build adds focused origin and Codex-isolation tests:

```sh
cmake --build build/daemon-dev --target ketplus_managed_task_origin_host_tests ketplus_codex_session_isolation_tests
ctest --test-dir build/daemon-dev -R 'ketplus_(managed_task_origin_host|codex_session_isolation)_tests' --output-on-failure
node --test tests/daemon/claude-sidecar/managed-transport.test.mjs apps/daemon/providers/claude-sidecar/test/process-barrier.test.mjs
```

These use trusted fake bootstrap acknowledgements, temporary journals and actual
local fixture processes, never a model. They cover review read/verify fences,
missing/spoofed/expired origins, idle/drain and queued requests, accepted child
policy snapshots, two-conversation process isolation, stale response IDs,
history/MCP setup failure and timeout, and SDK close failure before actual exit.
Concrete Cursor/Copilot host rows also verify start/stop rejection before ACK,
authorized dispatch after ACK, review revocation and retained read access; an
unverified adapter remains a negative capability control. Delayed/missing epoch
ACK rows run with concrete Codex, Cursor and Copilot adapters.
Synthetic history proves adapter behavior; it cannot prove installed-provider
history preservation. Installed-provider controls remain explicit opt-ins.

After the reviewed T-8 recipe, an explicit installed-Codex test uses the production
adapter and its real process/isolation/MCP lifecycle:

```sh
cmake --build build/daemon-dev --target ketplus_codex_offline_refresh_tests
KETPLUS_CODEX_OFFLINE_REFRESH=1 KETPLUS_CODEX_TEST_BINARY=/path/to/installed/codex \
  ctest --test-dir build/daemon-dev -R '^ketplus_codex_offline_refresh_tests$' --output-on-failure -V
```

Without the opt-in, its body skips before launching a CLI. It uses a fresh
child-only profile without credentials and a local model endpoint which always
rejects requests. An allowlist excludes model-turn methods; the sentinel must
record zero requests. The real adapter injects synthetic context through the
public API, restores the same thread ID, confirms prior MCP exit before new
provider launch, and discovers a new MCP epoch. Only the provider writes the
owned rollout; the test reads its marker without editing storage. This does not
test completed-turn history or model recall. See the
[provider gaps](decisions/0127-provider-origin-refresh-evidence.md).

The separate `ketplus_codex_bounded_refresh_tests` and
`ketplus_cursor_bounded_refresh_tests` targets default to skipping before any CLI
launch. They are not part of the quota-free acceptance matrix. A live run needs
an explicit provider opt-in (`KETPLUS_CODEX_BOUNDED_REFRESH=D-3` or
`KETPLUS_CURSOR_BOUNDED_REFRESH=D-3`), the installed executable in
`KETPLUS_CODEX_TEST_BINARY` / `KETPLUS_CURSOR_TEST_BINARY`, and the decision
owner's existing private ledger in `KETPLUS_BOUNDED_TEST_LEDGER`. Supply the
verified source head in `KETPLUS_BOUNDED_TEST_HEAD` and ledger owner in
`KETPLUS_BOUNDED_TEST_OWNER`. Never create a fresh ledger to repeat a consumed
allowance. The ledger requires `decision`, `maximum_per_provider`,
`owner_conversation`, `recall_allowed`, `retry_allowed`, and per-provider
`sent_or_uncertain` fields. Reservation occurs before delivery; errors and
timeouts do not refund it.

The separately authorized **D-7** additional Cursor turn uses
`KETPLUS_CURSOR_BOUNDED_REFRESH=D-7` and a distinct private ledger whose
`decision` is `D-7`. It retains the common owner/no-recall/no-retry/per-provider
fields above, plus `allowed_providers: ["cursor"]`, `maximum_total: 1`,
`previous_ledger` and `previous_ledger_sha256`. The previous ledger must still
be the unchanged D-3 ledger with all three provider allowances consumed and
the same owner. D-7 reserves its own Cursor count before forwarding and cannot
authorize Codex/Copilot or reset/refund D-3. Never recreate a consumed D-7
ledger. The quota-free proposal suite covers rejection of changed parents,
wrong scope/owner/decision, recall/retry, and reused allowances. The same
control/attestation, cheap named model, text-only, no-tools and history-refresh
barriers apply; no production mutation is enabled by this additional probe.

A later direct user instruction authorizes a separate **ten-prompt Cursor**
budget. Use `KETPLUS_CURSOR_BOUNDED_REFRESH=cursor-extra-10`, the existing private
ledger/owner/head variables, `KETPLUS_CURSOR_EXTRA_REFERENCE` matching its audit
reference, and an explicit `KETPLUS_CURSOR_TEST_PURPOSE` (initially
`canonical_failure_diagnostic`). This is a direct instruction, not a fabricated
decision answer. The ledger has `scope: "cursor-extra-10"`, Cursor-only scope,
total/per-provider maxima of ten, `authorization` metadata and two `parent_ledgers`
entries binding the unchanged consumed D-3 and D-7 files by SHA-256. Parent owners,
consumed counters and the D-7-to-D-3 hash chain must match. Private authorization
metadata stays out of source and prompt text.

The `invocations` journal records each reserved ordinal, purpose, source head and
model. Counter/journal mismatch, altered parents, broadened scope, reused ordinals
or an eleventh prompt are rejected. Atomic reservation remains before forwarding;
an error or ambiguous timeout consumes the ordinal. The owner chooses each next
verification step explicitly and stops once sufficient evidence is obtained;
there is no automatic retry loop. The current fixture still forwards only its
first prompt and intercepts the replacement prompt after real same-ID load.
Any later follow-up/recall that is explicitly added must also consume an ordinal.
Quota-free tests include `python3 -B tests/providers/cursor/extra-budget-test.py`
and the integrated ten-fake-prompt/eleventh-refused proposal case. These tests
cannot authorize a live provider run by themselves.

For the deliberate second additional probe, `KETPLUS_CURSOR_TEST_MODEL=composer-2.5`
and purpose `completed_history_refresh` select the catalog's non-fast named model.
Only that model and the original Haiku-low choice are accepted; Composer requires
the control or additional-budget mode. Its ACK must name the non-fast base ID or
explicit `[fast=false]` form and must not carry a conflicting fast option. Auto,
default, fast variants and mismatched ACKs fail the barrier. This opt-in changes
only the synthetic session, never the provider's default configuration.
Load diagnostics distinguish an optional response echo from the requested load
identity: `echo_present=false` has `echo_matches_requested=null`. A successful
ACP load may return an empty object; an absent echo is not an identity mismatch.

The separate inventory continuation uses purpose `injected_epoch_inventory`,
`KETPLUS_CURSOR_INVENTORY_ORIGIN=extra-ordinal-2` and
`KETPLUS_CURSOR_ORIGIN_SUMMARY` pointing to the private ordinal-two summary.
It accepts only the extra ledger with two consumed invocations, the matching
owner/cwd and the completed second invocation's original ID. Its summary hash,
completed-history outcome and confirmed cleanup must match. Composer non-fast,
attestation and no-tool gates remain required. This is separate from the old
D-3-only `READ_OWN_HISTORY` control; changing its purpose label is not a shortcut.
The wrapper rejects `session/new`, wrong IDs, failed loads, mismatched echoes,
absent/wrong-session history and duplicate prompts before reservation. Control
mode consumes nothing; live mode can reserve ordinal three exactly once. The
inventory path stops after that one prompt and requires inventory attributed to
its fresh injected worker before reporting success. Run the quota-free boundary
fixture with `python3 -B tests/providers/cursor/inventory-origin-test.py`.

These probes keep official inherited provider authentication, create a new
synthetic session and empty temporary cwd, and never scan existing history.
The native observer can record `assistant_refusal_rule` with only an allowlisted
P1–P6/U1–U2 rule ID and `plan`, `model` or `quota` category. These are stream
observations, not entitlement assertions; raw assistant text is never logged.
The Cursor adapter fixtures also cover ordinary answers quoting plan/usage
wording and split-chunk provider refusals. Run `ketplus_daemon_cursor_adapter_tests`
alongside the focused bounded-refresh classifier and Python attestation tests
when changing that refusal boundary, without enabling any native opt-in.
They pin cheap named models and check control/MCP before delivery. Cursor's
`control` opt-in intercepts the first prompt before the provider, without
reserving quota. Its live run passes only the initial authorized prompt;
the replacement transport's prompt is intercepted after the real load/model
response. History replay and MCP readiness come from the actual provider,
not that synthetic cancellation response. Both tests observe owned-process
cleanup; neither asks a model to recall history.

`ketplus_copilot_bounded_refresh_tests` follows the same opt-in pattern with
`KETPLUS_COPILOT_BOUNDED_REFRESH`, `KETPLUS_COPILOT_TEST_BINARY` and the existing
ledger/owner/head variables. Its default body also skips before CLI launch.
The separately approved D-6 Auto mode requires `copilot_auto_decision: "D-6"`
in that ledger; this changes the allowed model mode, not the allowance count.
The observer passes process-local `--model auto`, disables exposed model tools,
and checks the official session-scoped Auto configuration response and MCP
discovery before reserving/forwarding the one synthetic model prompt. The
replacement prompt is intercepted after real same-ID load. Underlying model
IDs are recorded only when the inspected ACP fields explicitly disclose them.
No default provider configuration is changed.

The Cursor discovery-approval proposal is separate from those model probes:

```sh
python3 -B tests/providers/cursor/discovery-proposal-test.py
python3 -B tests/providers/cursor/attestation-regression-test.py
python3 -B tests/providers/cursor/discovery-proposal.py --prepare --root /path/to/new-private-directory
```

The quota-free fixture tests use no provider. They verify the static
single-tool inventory, rejected mutation, matching project/ACP launch tuples,
private ledger writes before forwarding, refusal to reuse a mock allowance,
and refusal to credit one MCP instance's discovery to another instance. Private
attestation tests reject wrong nonce/epoch/tuple/OS peer PID and project-only or
incomplete handshakes. A real Node/Python fixture checks the actual preload and
Unix peer credentials without launching a provider.
The offline attestation regressions deliver socket events out of order and
verify immutable observation-time sequence/payload capture. `attested` and
`inventory` describe historical evidence; only `ready` can authorize a prompt
barrier. Exact-worker exit, close, EOF or error revokes readiness permanently
for that worker, including when older handshake records arrive later. Mock
wrapper tests revoke readiness during the wait, after the wait and at the final
barrier; the synthetic ledger stays at zero and no prompt reaches the fake
provider. A different instance's exit cannot revoke the injected instance.

The same suite drives RPC error, exit, timeout and assistant plan/usage-pattern
paths through a fake provider and wrapper. Diagnostics retain only an allowlisted
phase/reason and bounded integer RPC code/exit status, alongside event/PID/epoch
metadata. They do not retain raw error messages, configuration-command output or
assistant text. Assistant pattern enums are observations, not an attribution of
native RPC failure. Outstanding requests get a diagnostic after 30 seconds
(the Auto configuration wait also records its explicit 10-second timeout).
Timeout observation does not retry, refund a reservation or fabricate a response;
the adapter continues to own request completion/cancellation. These offline tests
launch only fixture Python/Node processes, never a provider CLI or model.
The observer also revokes readiness when its owned provider transport closes,
even if Node cannot deliver a child's terminal callback during shutdown. This
does not invent child lifecycle evidence or replace the independent PID closure
check. The bounded Cursor target has an always-offline canonical-diagnostic
classification test; only allowlisted adapter phase/reason enums escape it,
never the original message or unknown completion status. It runs with the live
opt-in unset and never launches a provider.
The full adapter's fake-ACP plan-refusal regression also verifies that successful
`end_turn` transport completion can still yield a failed canonical turn and no
completed assistant item. Its Auto plan-refusal and team-usage cases are likewise
quota-free. This is conditional adapter behavior, not proof of a live refusal's
cause when the original message was not retained.
Preparation starts no provider and executes no approval. Review its manifest,
checksum and `REVIEW.txt` before seeking the user's approval for the exact
native enable command. See the [proposal scope and revocation](decisions/0127-provider-origin-refresh-evidence.md#reviewable-cursor-discovery-approval-proposal).

After that exact approval, the Cursor production-adapter `control` run can set
`KETPLUS_CURSOR_APPROVED_PROPOSAL` to the existing proposal root, with its same
`CURSOR_CONFIG_DIR` and `CURSOR_DATA_DIR`. It uses the unchanged cwd and server
tuple from `session-new.json`, observing each newly spawned fixture instance.
By default this option is rejected in live mode: a static read-only approval does
not authorize a different mutation bootstrap. The explicit amended text-only
probe also requires `KETPLUS_CURSOR_ATTESTED_PROBE=1`, private per-transport
attestation, and the existing D-3 ledger. Cursor mutation remains disabled. A
project instance listing tools never establishes discovery for a separate
request-injected instance. Test-only `NODE_OPTIONS` instrumentation is restricted
to owned probe processes; no installed files, auth or default settings change.

`KETPLUS_CURSOR_READ_OWN_HISTORY=1` is a separate no-model diagnostic after the
allowance is consumed. It requires attested `control` mode and the existing
private ledger/owner. Only that ledger's synthetic Cursor ID and matching cwd
can be loaded; the target never scans history or delivers a native prompt.
It reports whether the known synthetic assistant marker replays, without
turning a failed or partial turn into completed-history evidence.

The earlier empty-session controls are retained for reference, not a loop to
repeat when their hypotheses and errors have not changed:

```sh
python3 tests/providers/codex/refresh-offline.py --executable codex
python3 tests/providers/cursor/refresh-offline.py --executable agent
```

The D-3 environment checkpoint adds a pinned Copilot control. Prepare an external,
test-only npm prefix, then invoke the already-installed CLI explicitly:

```sh
npm install --prefix "$TEST_PREFIX" --no-audit --no-fund @github/copilot@1.0.112
python3 tests/providers/copilot/refresh-control.py \
  --executable "$TEST_PREFIX/node_modules/.bin/copilot"
python3 tests/providers/copilot/refresh-control.py \
  --executable "$TEST_PREFIX/node_modules/.bin/copilot" --mcp-source launch
```

It creates and removes its own empty home/profile/cache/workspace, inherits no
credentials, and enables offline BYOK with a rejecting local endpoint. It sends
initialize/new and the informational `/mcp list` command, never a model question
or tool call. Exit 2 means MCP discovery was not proved; it must not trigger a
model fallback or automatic retry. The corrected 1.0.112 launch control exits 0:
the peer answers vendor `server/discover` with method-not-found before standard
MCP initialize/tools-list completes. Request injection still exits 2 without
worker startup within five seconds. Both record zero endpoint requests and zero
remaining workers. These are bounded observations, not an unsupported-provider
conclusion. This control is not in the default
suite and does not establish production-adapter history preservation. The bounded
live-turn budget and remaining authentication/discovery gates are recorded in the
[origin evidence](decisions/0127-provider-origin-refresh-evidence.md).

These controls do not send a model question, read existing user history or fabricate provider
storage. Empty-session load failure is recorded as a validation gap, not proof
that non-empty history cannot load.

The dedicated ACP transport has a separate small Qt target:

```sh
cmake -S tests/daemon/acp-isolation -B build/acp-isolation -G Ninja
cmake --build build/acp-isolation
ctest --test-dir build/acp-isolation --output-on-failure
```

It builds the production ACP engine and concrete Cursor/Copilot adapters, then
uses fake persisted sessions to check independent conversations, old-process
exit before replacement, history/epoch preservation, load errors/timeouts without
new-session fallback, and isolated worker failure. Provider runtime evidence is
separate; see [the refresh evidence record](decisions/0127-provider-origin-refresh-evidence.md).

The isolated storage/executor targets use temporary SQLite databases and no
provider accounts. They cover transaction-abort injection, byte paging across
UTF-8 boundaries, reopen, bounded 10 MiB retention, cursor gaps, short-write
coalescing, start retry, revision races, atomic terminal/outbox and wake acceptance,
quota admission/park/resume, pagination above 16 jobs, and native cleanup audit.
On macOS the executor suite starts a native supervisor and test-owned workloads;
it exercises a durable pre-launch handshake, reconnect without replay, timeout,
known detached descendants, supervisor crash, lease expiry, changed birth
identities, a pre-exec identity barrier, Stop before its commit and short-lived
commands that exit before Qt started notification. This is best-effort cleanup evidence, not proof that all possible
unobserved descendants are contained. The isolated target does not exercise SessionHost API or UI:

```sh
cmake -S tests/daemon/background-output -B build/background-output -G Ninja
cmake --build build/background-output
ctest --test-dir build/background-output --output-on-failure
```

The daemon build adds `ketplus_managed_task_host_tests` and
`ketplus_managed_task_ipc_tests` on macOS and Linux. Both use the scripted FakeCodex provider
and temporary journals; they consume no account quota. They cover independent
child sessions/output/approval, foreground Stop fencing without cancelling process
jobs, busy-owner durable enqueue without steering, duplicate completion/start
requests, actual IPC token authentication, owner forgery, retired-generation
local administration and complete registry pagination beyond 16 rows. The ledger
suite also injects prune failure and reopens tombstones to verify expired requests
cannot execute again after defaults change.

```sh
cmake --build build/daemon-dev --target ketplus_managed_task_host_tests ketplus_managed_task_ipc_tests
ctest --test-dir build/daemon-dev -R 'ketplus_managed_task_(host|ipc)_tests' --output-on-failure
```

The Pro `ketplus_managed_background_e2e_tests` fixture connects the real local IPC
server and registry to the Desktop client and `AgentSessionManager`. A test-owned
native process completes through FakeCodex, and the fixture checks the Desktop
task state/output, durable owner wake, retry/deduplication, conversation switching
and offline mutation refusal. It starts no installed daemon or live provider.
On Linux this fixture requires `KETPLUS_RUN_LINUX_JOB_TESTS=1` in the exclusively
owned delegated guest. The native QML task-bar states and controls remain covered
separately by `ketplus_agent_tests`; protocol-peer paging and stale-response tests
remain in `ketplus_background_task_transport_tests`.

Additional portable background-task builds and tests run over SSH in a task-owned
Linux workspace. Git and source edits stay on the Mac. Export tracked working-tree
source on the Mac (stage new source files there first), mirror that export with a
named one-way Mutagen session, and exclude Git metadata, builds, caches, local
authentication state and evidence. Keep remote build outputs and evidence outside
the source mirror. Flush synchronization and compare the complete file/mode/SHA256
manifest before testing; record the Mac commit and any uncommitted source hashes.
Do not edit source or perform Git writes on Linux, and do not rerun passed checks
merely to move them to the new host.

The native macOS identity/audit-token executor checks and macOS renderer-specific
Mermaid responsiveness classification require the Mac. Model-only Desktop,
Discovery and Attention checks are portable, as are runtime, storage, IPC and fake
provider checks. Select a Qt 6.11 toolchain explicitly on Linux; desktop/model
targets also require its WebView module. Missing toolchain components are a setup
blocker, not a reason to move portable work back to the Mac. Dependency preparation
must respect the same source-sync and no-remote-Git-write boundary.

The Host/IPC fixtures alone do not verify the shared UI, SDK native-background blocking or a
signed deployment bundle. On Linux, process tests require a non-root user with
a working systemd user manager, delegated cgroup v2/`nsdelegate`, `cgroup.kill`,
and permission to create isolated user/mount/PID/cgroup namespaces. Missing
capabilities reject start with `containment_unavailable`; do not lower host
policy to make the suite pass. A read-only cgroup mount alone, PGID cleanup or
same-UID delegation is not migration-containment evidence.

The owner-Stop and concurrent-fork timeout cases retain bounded state/timing
diagnostics on failure. Set `KETPLUS_MANAGED_TASK_DIAGNOSTICS=1` to include those
diagnostics in a passing control too. They report lifecycle milestones, the
terminal state and cleanup result without command arguments or credentials.
The fork case keeps the exact count bytes used by the assertion (at most 64
bytes as hex), parse status and an independent boundary snapshot before that
assertion. The owner-Stop case includes bounded executor metadata flags; the
executor currently records no actual launch, root-exit or timeout timestamps,
so registry milestone times must not be treated as those execution times.
Preserve the original failing run and binary identity before a
control; a passing control alone does not classify an earlier failure. Do not
relax the assertions or deadlines to obtain a passing result.

The native short-command fixture also reports its iteration and bootstrap phase
(`directory`, `process_start`, `connect`, `ready`), elapsed time, process/socket
state and error enums, valid exit information and bounded receipt readability
before destructor cleanup. The same diagnostic opt-in includes passing iterations.
It retains the 12 iterations and existing deadlines, and logs no protocol bodies,
lease values or command arguments.

Integration acceptance at `cc914a05` ([PR #763](https://github.com/ketvietlab/ketplus/pull/763))
ran the fresh serialized Pro matrix: 242 passed, one skipped and eight failed
out of 251 suites. Failures were in ChatMermaid, DaemonLifecycle, SessionFork,
AgentMessaging, DesktopDaemonClient, ConversationAttentionDaemon,
DesktopSessionDiscovery and NativeJobExecutor; the native failure refused
`Executor::start()` in an iteration before its short-command identity assertions. TeamReply
passed 11/11 in this run, which does not resolve its historical failure recorded
in the provider refresh evidence. The original Linux matrix passed five of seven
suites; owner-Stop expected `completed` but observed `timed_out`, and concurrent
forks failed the count predicate before the cleanup assertions. Isolated and
predecessor-order controls passed. After the Mac matrix ended, one final serial
run of the two Linux cases with exact-count/boundary diagnostics also passed;
the fork sample was 71 with successful parsing, a removed boundary and verified
cleanup. These controls do not classify the original failures. The team leader
accepted this Linux validation risk for integration after independent evidence
verification; this does not accept all T-5 checks, release or packaging. The Mac
failures remain pending classification and explicit disposition. No assertion or deadline
was relaxed and no full matrix was retried. CMake-installed daemon/helper staging
passed the runtime and capability checks; the whole-tree Runtime install stopped
at the unbuilt runner target, so this is not a full packaging pass.
One subsequent native short-command case control with bootstrap diagnostics passed
all 12 iterations; each reached `ready` in 45–74 ms with a readable receipt. This
does not identify or fix the original bootstrap failure, and native controls stop
at that single run. The team leader accepted the separate native validation risk
for integration, retaining the original failure as unclassified.

One authorized serial A→B→C control then ran only the 17 originally failed rows
from the other seven suites (15 methods, seven invocations). Every requested row
passed, with no skips or unintended rows; binaries matched the original full-run
hashes, and assertions, deadlines and fixed waits were unchanged. Mermaid measured
baseline/worst/threshold values of 20/47/150 ms and 26/32/150 ms. Controls stopped
after that single sequence; they do not replace the original full-run result or
classify its failures. Cleanup sampling observed one reparented SDK helper after
Attention, followed by empty inventories after Mermaid and on a later check.
Its operation and cleanup timing were not captured, so this is not a claim that
every intermediate inventory was empty. After independent verification of the
append-only summary correction, the team leader accepted the seven-suite validation
residual for integration, including these cleanup attribution limits. Original
failures remain unclassified; this is not all-T-5, release or full-packaging acceptance.

The seven-suite summary has an append-only evidence correction: version 2
supersedes only the original blanket cleanup sentence. The original summary,
its hash, the 30-file manifest and raw samples remain unchanged. The corrected
summary explicitly retains the Attention helper observation and the later empty
samples; neither establishes closure before that fixture ended or identifies the
helper's birth, launch, request or managed-job ownership. D1 owns correlation and
closure follow-up at a future assigned verification; no new run is authorized to
reconstruct missing historical data.

#### Integration review coverage and remaining gates

The following maps T-5's six criteria to the retained evidence. Coverage is not
formal acceptance; the original full-matrix failures and their separate residual
decisions remain part of the review.

| Criterion | Direct coverage | Remaining gate or limit |
| --- | --- | --- |
| 1. Fake-provider start, UI state, one owner wake and output | `ManagedBackgroundEndToEndTest::registryEventsReachDesktopAndWakeOnlyTheirOwner`; actual socket RED → GREEN and fresh full-matrix pass; native Agent UI suite and four-provider transport checks | Model/socket and synthetic native UI coverage; no live-provider inference claim |
| 2. Crash, restart, owner lifetime, races, descendants and held wake | `ManagedTaskRuntimeTest` restart/close/commit-failure cases; `ManagedTaskHostTest` provider exit, busy/permission/Stop and nested-child cases; Linux executor daemon/supervisor death and Stop/complete race | Original Linux owner-Stop and fork-count failures remain unclassified under the leader's integration-only residual decision; formal review pending |
| 3. Wake/auth fences, complete registry and platform assurance | Ledger atomic-outbox/dedup/generation/quota/pagination cases; IPC owner/grant checks; `ManagedTaskOriginHostTest` review/delayed-origin cases; Linux real-helper boundary tests; native birth/audit-token checks | Native bootstrap original remains unclassified under its separate integration-only residual decision; seven-suite integration-only residual retains the intermediate SDK-helper attribution limits; no absolute macOS containment claim |
| 4. Build/matrix, honest limits, no quota or app replacement | Fresh Pro build and 251-suite run; offline SDK/contract/public-tree checks; Linux acceptance and CMake-installed daemon/helper staging; source/binary/hash records | Full Mac remains 242 pass / 1 skip / 8 failed suites; whole-tree Runtime install stopped at an unbuilt runner; no full packaging, release or installed-app validation |
| 5. Reviewed PR into integration and cross-lane resolution | PR #763 contains the minimal deferred IPC routing fix and socket regressions; provider/UI dependencies are merged | Formal PR approval and integration acceptance remain outstanding; evidence-only residual reviews do not substitute for that approval |
| 6. Native faults, attention, slots, durable owner fences and user-only resolution | Original native detached-descendant, supervisor-death, reconnect and lease-loss cases; ledger/IPC/UI attention, slot and user-only CAS/audit cases; the targeted real-native attention follow-up below carries one incident through file-backed reopen and each distinct owner-close/generation-change path | Reviewer confirmed this criterion has sufficient evidence in the agreed scope at runtime `0eba074c` / review `ce4d31d0`. The unrelated SDK-helper sample remains unattributed; no absolute native containment or full packaging claim |

Portable follow-up work uses the verified SSH/Mutagen workflow above. Mac-only
identity and renderer checks stay on macOS. This mapping authorizes no additional
control, provider invocation, toolchain change or production fix.

#### Attention incident lifecycle follow-up (targeted runtime reviewed)

`ketplus_managed_attention_recovery_tests` adds the `owner-close` and
`generation-change` rows of `attentionSurvivesOwnerLifecycle`. It uses a file-backed
`LocalJournal`, real `SessionHost`/runtime and scripted FakeCodex. On macOS only,
the row launches a harmless marker/output/sleep process through the managed API,
captures its supervisor/root identities, faults the supervisor with the production
identity-bound signal, and requires `needs_attention`/`unverified` with the known
executor stopped. On Linux, it injects an executor result without an execution
identity: that branch checks portable persistence, Host lifecycle and CAS/audit
plumbing and makes no claim about native faults, birth identity or containment.

Each row closes and reopens both journal and Host/runtime, retains the incident,
output and root result, checks slot release without terminalizing the incident,
and tests two distinct invalidation paths. The owner-close row deletes through
the Host. The generation-change row closes the Host, verifies no tombstone or owner
fence exists, then replaces the generation directly through the journal and reopens
the Host without deleting first. A fresh owner-invalid fence must be observed;
close/recreate must not mask generation mismatch. Local administration must retain
access to the old owner's incident. Pruning, automatic
wake/relaunch/reassignment/resolution, agent acknowledgement and stale revision
acknowledgement are fenced. A current-revision local user acknowledgement must
create exactly one durable audit row and retain `unverified`/suppressed completion
after the final reopen. Before the native fault, the output API must already
return the exact bytes with a captured cursor and size; later reads check
preservation of this sample. Reconciliation waits for publication of this task and
generation, with a newer native revision; the portable no-identity path instead
requires publication of its unchanged incident. Native scope cleanup retains access
to the live Host/journal and validates the task's own receipt, including failures
before `running` or local identity capture. Stop/reconcile and birth-bound signals
must reach bounded closure before fixture deletion. Signal/Stop errors or unproven
closure fail the test and retain the private directory. Diagnostics whitelist
bounded fields, never lease values, protocol bodies or command arguments.

The Linux-only `acceptedIntentWithoutCallerIdIsRecoverable` regression persists an
intent in a file-backed journal, closes it without retaining a caller task ID, and
recovers the ID through the same read-only tuple lookup used by native fixture
cleanup. It distinguishes an absent generation from a database lookup error and
launches no executor. Cleanup never searches the API projection for request keys.
The macOS target still executes only the two native lifecycle rows.

Before executing this follow-up, submit the exact source/configuration and commands
for offline review. On the verified Linux source mirror, use a separate build tree
and a Qt prefix with the headless daemon dependencies; Desktop/WebView is not part
of this target:

```sh
cmake -S "$TASK_SOURCE" -B "$TASK_BUILD" -G Ninja \
  -DKETPLUS_EDITION=pro -DKETPLUS_BUILD_DESKTOP=OFF \
  -DKETPLUS_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$QT_ROOT" -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DFETCHCONTENT_SOURCE_DIR_KETPLUS_LIBDATACHANNEL="$TASK_DATACHANNEL_SOURCE"
cmake --build "$TASK_BUILD" --target ketplus_managed_attention_recovery_tests -j8
ctest --test-dir "$TASK_BUILD" -R '^ketplus_managed_attention_recovery_tests$' -j1 -V
```

`TASK_DATACHANNEL_SOURCE` must be a separately hashed/synced source-only export of
the libdatachannel commit and its dependency pins recorded in `cmake/WebRtc.cmake`,
including the patched libjuice pin. Prepare it on the Mac, without Git metadata,
build output, cache or credentials. The source override and disconnected mode
prevent CMake's normal Git clone/update/patch path from writing Git state on Linux.
Do not run configure until this dependency export is verified too.

Only the real macOS fault/recovery chain may use the existing Mac build, after
source/binary/config identity and test-owned process inventories are recorded:

```sh
cmake --preset dev
cmake --build --preset dev --target ketplus_managed_attention_recovery_tests -j1
ctest --test-dir build/dev -R '^ketplus_managed_attention_recovery_tests$' -j1 -V
```

The targeted verification ran once on source `0eba074c`: Linux matched exactly
three cases (the lookup regression and two injected lifecycle rows), all passed
without skips in 3.625 seconds. Only after Linux finished, the Mac target and its
required helper were built; exactly two real-native lifecycle rows passed without
skips in 41.637 seconds. These elapsed times include the run harness; the raw QtTest
logs separately record row timings. No old controls or full matrix were repeated.
The Linux build had already completed with `-j1` when the instruction to use `-j8`
arrived; it was not rebuilt. The command above records the new concurrency for
future necessary Linux builds; test concurrency remains serial.

Both native rows captured persisted API output (17 bytes and its cursor/size)
before supervisor fault, retained the recorded identities and root-result field
through reopen, observed fresh recovery/owner fences and retained attention without
pruning or slot ownership. User-only current-revision acknowledgement created one
durable audit, with final `interrupted`/`unverified` and suppressed completion;
the no-wake assertions passed. The native recorded exit-code field was `-1`, not
an observed successful root exit, and executor-event timestamps remain unavailable.
Both native cleanup guards reported closure, no signal/Stop errors and no retained
fixture; the scoped post-run process inventory was empty. These checks cover known
identities and preserve the native best-effort limit.

Append-only runtime evidence v11 retains source/binary/helper/config hashes,
platform/resource snapshots, exact selector listings, raw logs and JUnit outside
generated cleanup. Read-only review verified 43/43 manifest files, five exact source
snapshots and all 13 prior v10 artifacts, and found no remaining issue in this
runtime packet. Criterion 6 has sufficient evidence in the agreed scope; formal
whole-T-5 review remains outstanding. The original Mac 242/1/8 and Linux
5/2 failures remain unclassified under their integration-only residual decisions.
This follow-up does not complete formal T-5 acceptance, packaging or release.

Do not run unrelated suites, repeat passed controls, change deadlines or provision
shared toolchains. A source-only
Linux mirror has no Git metadata; retain the independently verified Mac source
manifest as provenance rather than claiming the generated `unknown` revision is a
commit identity. This disposition authorizes no additional verification run.

The opt-in `ketplus_linux_job_executor_tests` runs the real packaged helper and
registry. It covers missing-manager admission, durable retry/output, double-fork/
setsid descendants and migration attempts, timeout during forks, supervisor
SIGKILL and a separate daemon-runtime process killed before lease recovery and
registry reopen. Run it together with the fake-provider Host and socket IPC tests
only in an exclusively owned reference guest:

```sh
cmake --build build/daemon-dev --target ketplus_linux_job_executor_tests ketplus_managed_task_host_tests ketplus_managed_task_ipc_tests
bash tests/daemon/linux-containment/executor-acceptance.sh build/daemon-dev
```

The script does not install dependencies or modify sysctls, mount permissions or
AppArmor policy. A skipped opt-in suite is not Linux executor acceptance.

Review regressions cover a successful descendant waking its parked managed child
both with free slots and with its root's four slots occupied. The macOS executor
test captures an audit-token signal target, makes the process exec a new image
while retaining PID/birth, and verifies that sending the old target does not
signal the new incarnation. Linux's stalled-manager fixture uses an independent
POSIX Unix-socket listener that accepts DBus connections without answering auth:
admission times out after six seconds while a timer keeps advancing and another
real job remains running under its unchanged five-second executor lease.

#### Background job runtime verification record

On 2026-11-16, the T-2 runtime checkpoint passed the following checks without
provider accounts or changes to an installed application:

| Check | Environment | Result |
| --- | --- | --- |
| Host, IPC, native executor and runtime CTest suites after review fixes | macOS arm64, Qt 6.11.2 | 4/4, 61.51 s |
| Production-helper executor, Host, IPC, output, ledger and spec CTest suites after review fixes | Ubuntu 24.04 arm64, kernel 6.8, systemd 255, Qt 6.11.0 | 6/6, 114.37 s; no skipped executor cases |
| Runtime-component install into a separate prefix | Same Linux reference guest | Install succeeded; staged daemon `--check-runtime` succeeded with loader/plugin environment overrides removed |
| Staged executor capability probe in a delegated user unit | Same Linux reference guest | `available=true` |
| Contract fixtures and public-tree hygiene | macOS host | 9/9 Node tests and 4/4 Python tests |

The Linux helper suite verifies inherited environment plus explicit delta/removal,
durable start retry and output, migration attempts, detached descendants, timeout
during active forks, Stop/complete races, supervisor SIGKILL, daemon lease expiry
and journal reopen without command replay. Host/IPC fixtures use scripted providers
and exercise approval, busy/Stop fences, provider exit, descendant cancellation and
owner/grant checks. The separate T-7 primitive probes are not counted as these
runtime results. Only the disposable guest had the documented namespace-policy
bootstrap; the acceptance script and product installer do not apply it.

An earlier full Linux run failed the Host assertion that a managed permission
remained pending. The focused case subsequently passed (2.59 s), as did the full
matrix above. Its cause was not established. The test now checks and prints the
start reply before checking approval so an admission rejection can be distinguished
from an approval-lifetime failure. Earlier failure logs are retained locally for
review; the green rerun does not erase that observation.

During regression development the Qt-based fake bus listener aborted in Qt
Network's incoming-socket path. A GDB backtrace was retained; the fixture now
uses an independent POSIX listener instead. The focused stalled-manager test
passed (8.44 s), followed by the complete Linux matrix and staging checks above.
This change does not attribute or fix a general Qt Network defect.

This record does not verify SDK native-background blocking, shared desktop UI,
cross-provider live inference, Linux amd64, signed macOS distribution or an
installed service. Those remain separate provider/UI/release acceptance checks.

The accepted provider-neutral contract is [ADR 0127](decisions/0127-daemon-background-tasks.md).
Run its design-fixture check without a build or provider account:

```sh
node --test tests/daemon/background-task-contract.test.mjs
```

This validates the behavioral contract and required fault-matrix inventory in
`tests/daemon/fixtures/background-tasks-v1.json`; it does **not** test a running
registry, executor containment, SDK background blocking or completion delivery.
The ADR names those separate implementation gates. Existing tests below remain
legacy-behavior evidence during migration.

The opt-in Linux daemon-user environment and containment probes are documented
in [the Linux evidence record](decisions/0127-linux-containment-evidence.md).
They run synthetic workers in an exclusively owned disposable guest, including
expected escape controls. They are not part of the default suite and do not
establish production registry/executor acceptance.

`node --test tests/daemon/claude-sidecar/background-tasks.test.mjs` runs a legacy
fake SDK that deliberately ignores the native-disable options and reports an
existing background shell, solely to verify drain compatibility. The turn must complete while the
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

Build `ketplus_daemon_peer_transport_tests`, `ketplus_peer_link_tests`, `ketplus_daemon_relay_wakeups_tests`,
`ketplus_daemon_cloud_privacy_tests`, `ketplus_cloud_trust_lifecycle_tests`,
`ketplus_sender_key_cache_tests` and
`ketplus_daemon_context_relay_tests` in a
Release headless tree. The peer suite uses real libdatachannel loopback with signed
SDP, in-memory credentials, temporary SQLite and a fake provider that is never
started. It covers transfer caps/order/expiry, strict send priority, grants,
revocation, supersede/epochs, subscriptions, cloud-safe encryption and the same
command retried over both paths. No user daemon or provider account is accessed.
Physical devices/TURN require a separate explicitly configured local test stack.
`ketplus_peer_link_tests` links only `ketplus_peer_link`: a `WebRtcPeerLink` offerer against an
answerer over loopback carries frames on all three channels both ways, a transfer of exactly
8 MiB both ways and a refused 8 MiB + 1 transfer; a wrong expected fingerprint refuses the
answer, and a rewritten answer fingerprint fails DTLS, with no frame either way; closing or
destroying a link while negotiating or mid-transfer is safe. On macOS, also run it under
`MallocStackLogging=1 leaks --atExit --` after changing link lifetimes.

`ketplus_cloud_trust_lifecycle_tests` holds native fake HTTP replies across sign-out and
re-enrollment (including the same machine/workspace), and independently changes each scope
identifier. Device sign-in interleavings also hold enrollment, start approval, finish
enrollment with the same identity and release the old claim or approval reply; pending
sign-in must clear without restoring pins or issuing another approval request. The same
interleaving is checked when storage fails after resetting pins.
It verifies that stale replies cannot write pins or wrapped keys, issue follow-up
uploads, refresh a new enrollment's pending peer, or complete pairing/revocation. It also
checks that legitimate restored-enrollment migration works for distribution and sender-key
fallback, while initialized empty pins and missing enrollment do not permit adoption. The
standalone Release lane exposes the same suite as `r03_cloud_trust_lifecycle_tests`; it uses
in-memory credentials, generated synthetic keys, a temporary journal and no provider turns.

`ketplus_sender_key_cache_tests` (standalone: `r03_sender_key_cache_tests`) warms the
cache before sign-out and same-workspace enrollment, verifies denial until a new local
pairing approval, and checks authorized reuse without another HTTP request. It rechecks
removed/replaced pins and scope, sign-in/cancellation, enrollment, device/machine revocation,
and held key replies across those transitions, including revoke/re-pair with identical keys.
The command regression separately delivers a signed, encrypted `session.rename` through the
mailbox execution path: the journal stays unchanged before re-pairing and changes after it.
This is native command-path coverage with fake transport, not a physical-device or hosted
relay end-to-end test. Run it alongside the T-4 trust-lifecycle suite; all credentials are
synthetic and no provider is started.

## OS credential storage

Run `ketplus_daemon_credential_store_tests`, `ketplus_credential_keychain_tests`
(macOS), and `ketplus_credential_secret_service_tests` (Qt DBus and `dbus-daemon`
required). The focused standalone build is:

```sh
cmake -S tests/daemon/credential-store -B build/credential-store -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/credential-store
QT_QPA_PLATFORM=offscreen ctest --test-dir build/credential-store --output-on-failure
```

Keychain tests create and delete only a disposable keychain with synthetic values;
a second executable must be denied without prompting. Secret Service tests start
and stop their own private Unix bus and fake service; they never use the user's
session service. They verify missing/locked/denied stores, prompt dismissal and retry.
Migration tests preserve unsafe/corrupt sources, verify writes before cleanup, retry
after write/read-back failure, reject conflicting backups and preserve atomic removal.
Production storage refuses access in ordinary QStandardPaths test mode; fixtures
explicitly inject their backend. Also run the cloud trust-lifecycle, sender-cache,
remote-control client/projection/backend and WebRTC host phase 3 suites for account
and sign-out interactions. No provider quota or real user credentials are used.
Desktop session discovery links the same in-memory credential fixture and resets
it before each case so its synthetic daemon identity never accesses an OS store.
Tests that start a daemon subprocess use `ketplus_fixture_daemon`: the production
entry point linked with a test-only persistent credential backend. Its synthetic
records live beneath the explicitly supplied temporary `KETPLUS_DAEMON_DATA_DIR`,
so restart/fingerprint checks work without accessing a native OS store. The fixture
is neither installed nor bundled, and production has no environment switch to
enable this backend. A data-directory override alone does not isolate native
credential namespaces and must not be used as that isolation boundary.

`ketplus_credential_batch_tests` links the production record codec and lock with a
counting, synthetic backend. Its standalone equivalent is
`r03_credential_batch_tests` in `tests/daemon/cloud-privacy`. It checks bounded
fetches for heartbeat metadata, distribution, pairing and command-key lookup as
session counts increase; cold keys and wraps commit once. It also covers aborted
batches, unavailable reads/writes, metadata-cache rollback, current pins, competing
store instances, and a child process denied by the held lock. Subsequent operations
must observe committed mutations. Run it with the credential-store, cloud-privacy,
trust-lifecycle, sender-cache and remote-control suites. Shared headers and CMake
changes require the full Pro build and unfiltered suite as well.

For an optional synthetic performance run, set `KETPLUS_CREDENTIAL_MEASURE=1` only
on this target. It emits counts and raw timings for 2,000/8,000/20,000 accounts,
100 active sessions and a two-millisecond fake backend delay. Retained padding
values are 420 bytes; seeded wraps exercise the cached path, not cryptographic
throughput. A queued `session.interrupt` against a loaded idle session must return
`turn_not_running`; its dispatch time observes event-loop delay, not cancellation
of an active provider turn or IPC end-to-end latency. These are synthetic backend
fetches, not measured native OS calls. Record native-store measurements separately,
including dataset, sample count, environment and cleanup; label any extrapolation
explicitly. Keep raw security-audit evidence outside the public tree.

The normal test configuration warns if the optional Secret Service fixture tools
are unavailable on the build host; do not claim that coverage in that case. Fake
D-Bus tests on macOS do not verify a Linux keyring's ACLs, backup or desktop prompts.
Native signed upgrades and platform packaging remain separate acceptance checks.

## Conversation deletion and next-turn defaults

`ketplus_provider_deletion_tests` uses fake providers and temporary journals to
check historical-provider cleanup, shared references, restart retry and unsupported
provider rejection. `ketplus_desktop_daemon_client_tests` covers deletion reaching
another attached Desktop client. The standalone Release cloud-privacy lane checks
heartbeat deletion receipts and retry; the KetPlus deployment owns relay tombstone,
workspace/generation isolation and encrypted-history redaction tests.

The standalone `tests/daemon/cloud-privacy` and `tests/daemon/characterization`
projects take the daemon's sources from `apps/daemon/sources.cmake` (the journal's
`src/storage` sources for characterization), so a split of those sources reaches them
without a hand-kept list. New include paths, dependencies or platform libraries still
have to be added to each lane. They apply `ketplus_enable_warnings`, as do the
in-tree test targets, including `ketplus_atlas_tunnel_fixture`.
`r03_characterization` needs `R03_FIXTURES` to name a materialized v3 workload
directory. Without it, `initTestCase` fails.

`ketplus_agent_shell_tests` checks the Delete conversation card against the mock
(`deleteCardOffersArchiveInstead`): its title, the mock's copy, no worktree choices
when there is nothing to remove, and Archive instead, which archives and never
deletes. `KETPLUS_DIALOG_CAPTURE_DIR=<dir>` saves `<dialog>-{light,dark}.png` for
the Delete, Rename and worktree cards (#422), to compare with
`KetPlusMock --open=rename` and `--open=delete`.

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

### Storage options, Compact now, breakdown and export (#128, ADR 0108)

`ketplus_daemon_storage_jobs_tests` covers the revisioned storage options (kept apart
from the detail and size policies, local-owner only), the warning and its option,
Compact now clearing an over-limit journal, Compact now waiting for a running turn,
the breakdown measured off the event loop, and export writing one owner-only file per
conversation. Run it with `ketplus_daemon_usage_retention_tests` (generated-image
option), `ketplus_daemon_size_policy_tests`, `ketplus_daemon_lifecycle_tests`
(archive period), `ketplus_settings_storage_page_tests`, `ketplus_workspace_tests`
(sidebar banner) and `ketplus_desktop_daemon_client_tests`. No provider is started.

The real-size check is opt-in. Copy the installed journal with SQLite's backup API (never
copy the live WAL files, and never start the full daemon on the copy, because its cloud
client would connect as the same device), then run:

```sh
python3 -c 'import sqlite3,sys; s=sqlite3.connect(f"file:{sys.argv[1]}?mode=ro",uri=True); d=sqlite3.connect(sys.argv[2]); s.backup(d)' \
  "<journal.sqlite3>" "$HOME/KetPlus-journal-size-check/journal.sqlite3"
KETPLUS_JOURNAL_SIZE_CHECK="$HOME/KetPlus-journal-size-check/journal.sqlite3" \
  QTEST_FUNCTION_TIMEOUT=3600000 QT_QPA_PLATFORM=offscreen ./ketplus_daemon_storage_jobs_tests journalCopyShrinksOnFirstCompaction
```

It migrates the copy to incremental vacuum, runs Compact now twice and requires the file
under 4 GB and under its limit. Delete the copy afterwards; it holds real transcripts.

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

## Atlas result receipts

The Agent Workspace target covers explicit Markdown link extraction (including
code/raw-HTML exclusions), completed-only receipts, long-message chunk placement,
actual QML actions and conversation/worktree/deletion fences. `AtlasTest` checks
manifest limits, metadata and canonical-root/symlink isolation. `AtlasWorkspaceTest`
checks real manifest metadata and the existing builder action without invoking a
provider. Browser launch remains the managed viewer's existing integration path;
these tests do not start a browser or validate the Atlas screens themselves.

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

### Compacted image previews (#128, ADR 0101)

Run `ketplus_daemon_detail_compactor_tests` and `ketplus_daemon_usage_retention_tests`.
`generatedImageKeepsAPreview` covers bare base64 and `data:` URL pixels, the
`image-preview-v1` marker and idempotence. `generatedImageWithoutAPreviewKeepsItsCaption`
covers the caption-only fallback and that malformed base64 never reaches the encoder.
`toolResultImagesKeepUpToFourPreviews` covers four previews per call, later images and
input images as placeholders, the `previews` count and idempotence.
`toolResultImagesKeepAPreview` runs a real `SessionHost` over tool results with a
translucent 900×1200 PNG (one decoding item per pass, 512 px references after the sweep).
`generatedImagesKeepAPreview` runs a real `SessionHost` on a journal seeded with a
1024×768 PNG. It expects one preview after the first pass (one decode per pass) and a
512 px JPEG media reference on each eligible turn after the sweep. No
installed journal or live provider is used.

## Desktop update QA isolation

A desktop QA launch must isolate native settings before any application code reads
or writes them. On macOS, changing `HOME` and `CFFIXED_USER_HOME` alone is insufficient:
QSettings NativeFormat can still use the logged-in user's preferences via cfprefsd.
For a QA session that does not exercise Sparkle itself, launch a disposable app
bundle directly with `KETPLUS_QA_SETTINGS_DIR` pointing to an absolute scratch
directory:

```sh
qa_home="$(mktemp -d)"
HOME="$qa_home" CFFIXED_USER_HOME="$qa_home" \
  KETPLUS_QA_SETTINGS_DIR="$qa_home/settings" KETPLUS_DISABLE_DAEMON_AUTOSTART=1 \
  "$qa_bundle/Contents/MacOS/KetPlus"
```

Desktop routes default QSettings to INI files under the directory's `user` and
`system` subdirectories, including organization-level fallbacks. It exits with a
startup error on stderr if the requested location is unusable. The QA directory
takes precedence over `KETPLUS_PROFILE_DIR`. The updater is disabled for the
production bundle identifier and when the identifier is missing. To test Sparkle,
give the disposable bundles a unique QA bundle identifier shared by both versions
and keep `KETPLUS_QA_SETTINGS_DIR` set. Sparkle then uses the disposable bundle's
preferences. Use a dedicated daemon if the scenario needs one. Record only key
namespaces and counts in public evidence. Keep any comparison evidence outside
the repository.

The regression for audit finding M5 is `ketplus_qa_settings_isolation_tests`.
It checks that the default settings use INI before writing, verifies isolation
from ordinary INI fallbacks, and exercises startup rejection and the disabled
updater. On macOS, it intercepts Qt's native key lookup and verifies that the
isolated settings perform no native lookups. The test writes settings only to
scratch INI files. Run it with a Sparkle-enabled build to cover the macOS
updater selection:

```sh
cmake --build build/dev --target ketplus_qa_settings_isolation_tests
ctest --preset dev -R '^ketplus_qa_settings_isolation_tests$' --output-on-failure
```

Use a throwaway EdDSA key, loopback-only feed and fake providers. Verify the complete
bundle's destination before install/relaunch; it must be disposable and outside the
normal installed app. Check the QA settings file for expected key names after each
session, and stop if evidence indicates writes outside isolation. A running real app
can change its own preferences, so timestamps alone cannot attribute a write to QA.
Do not open real workspaces or consume provider quota for update tests.

The compiled-feed delegate harness in `tests/updates` uses a unique test bundle
identifier, starts no updater/network operation and restores only its own test domain.
It verifies that restored staging and HTTP defaults cannot override the compiled feed.
Set `SPARKLE_SDK_DIR` to the unpacked pinned SDK and `QT_ROOT` to the Qt prefix,
then build and run the harness.

```sh
cmake -S tests/updates -B build/updater-policy -G Ninja \
  -DCMAKE_PREFIX_PATH="$QT_ROOT" -DKETPLUS_SPARKLE_DIR="$SPARKLE_SDK_DIR"
cmake --build build/updater-policy --target ketplus_updater_feed_policy_tests
ctest --test-dir build/updater-policy -R '^ketplus_updater_(feed_policy|architecture)_tests$' --output-on-failure
```

The compiled-feed test sets `QT_QPA_PLATFORM=offscreen`. The architecture
test uses a disposable bundle and real Sparkle information checks over
loopback. It expects an offer for the matching architecture and no offer for
mismatched or missing metadata, with a diagnostic for each refusal. Production
bundle installation and owner-run manual UI acceptance remain separate checks.

The opt-in `tests/updates/sparkle_loopback_qa.py` runner exercises the real updater in
a staged Desktop bundle. Build the test-only trigger against the pinned SDK.

```sh
clang++ -std=c++20 -fobjc-arc -dynamiclib -F "$SPARKLE_SDK_DIR" \
  -framework Sparkle -framework Foundation -Wl,-rpath,@executable_path/../Frameworks \
  tests/updates/SparkleQaTrigger.mm -o "$qa_scratch/SparkleQaTrigger.dylib"
```

The trigger accepts only a disposable bundle with a `dev.ketplus.audit.*`
identifier and a loopback HTTP `SUFeedURL`. It requests a background check and
records Sparkle's cycle and error codes. Sparkle may install a downloaded
update when the app exits, so both the launch and candidate bundles must be
disposable. The trigger leaves relaunch to the operator.

Stage the full Runtime bundle with a loopback-only daemon control URL. Set the
QA bundle identifier and feed in a fresh scratch copy, then ad-hoc sign it.
Supply a ZIP and its signature, signed with a throwaway EdDSA key. For an
appcast that must be signed, pass the pinned `sign_update` tool and the scratch
private key using `--sign-update` and `--signing-key`. Use `--interrupt` to close
archive responses after 64 KiB. Add `--retry` to let the next request complete
and record whether it requested a byte range. Pass `--ctl` and `--fake-codex`
(the `hang` case from `tests/daemon/FakeCodex.h`) to verify that a separate daemon
reports an active fake turn before and after the update cycle. The runner
verifies the bundle's signature before and after each case. It waits for the
installer and progress jobs scoped to the QA bundle identifier, captures the
observed helpers' Sparkle logs, and deletes that identifier's preference domain
after the jobs stop.

By default the loopback runner tags the feed item with the thin host bundle's
architecture. For F1, supply the signed wrong-architecture archive and set
`--item-architecture` to that archive's actual slice. Use
`--item-architecture missing` for the untagged appcast case.

For a destination-volume case, mount the disk image inside the case root and
pass the mounted app to `--bundle`. For a read-only fixture, use a UDRO image
and attach it with `hdiutil attach -readonly -nobrowse`. For a full writable
fixture, copy N to an APFS image, then write filler on that volume until a
write returns `ENOSPC`. Keep the feed and daemon state outside the image. Detach
the image after the updater and daemon have stopped. Use `--inspect-volume` to
save the difference in paths on the destination before detaching, including the
test user's Cocoa replacement staging directory. If the runner reports live
helpers after cancellation, leave the image mounted until cleanup succeeds.

`ketplus_machine_toast_tests` renders the production Machine composer in temporary
INI settings, in light/dark. It checks the exact recipe gap after growth and window
resize, composer/view hide and restore, and preservation of another composer's
reservation during hide and destruction. It uses no backend or provider.

`ketplus_scheme_manager_tests` covers one-time legacy migration, independent
system preferences, built-in parity against live kit tokens, complete async
publication, exact notification counts, preview cancellation/commit, stale-result
suppression and external revision cancellation. `ketplus_git_diff_view_tests`
also verifies model/selection identity and alpha-composited canonical diff colors.
These offscreen checks do not replace native DELL or Pro chat acceptance.

## Consented SSH daemon installation (ADR 0124)

`ketplus_daemon_release_tests` verifies Ed25519 manifests and fail-closed build
configuration. `ketplus_ssh_install_transaction_tests` exercises the actual
Python transaction with disposable fixture runtimes: interrupted transfer,
checksum/archive/runtime failures, decline/EOF rollback, ownership guards,
busy stop refusal, downgrade refusal, upgrade rollback and data retention.
`ketplus_daemon_upgrade_tests` covers the authenticated idle-only upgrade and
`daemon.stop_if_idle` gates, including an active provider fixture.

The Connections target is part of normal desktop test builds/ctest and reuses
`KetQt::Widgets` from the desktop. Its consent/decline routing tests always run;
the full host proof additionally needs the explicit QA inputs below.
For the Linux Connections proof without the full editor's WebView/compat SDK,
configure `KETPLUS_BUILD_DESKTOP=OFF`, `KETPLUS_BUILD_SSH_INSTALL_UI_TESTS=ON` and
`KETPLUS_CONTROL_URL=http://127.0.0.1:9`. This optional test target compiles the
actual Connections/controller/installer sources with Két Widgets. It needs Qt
Widgets/Svg/Test, OpenSSL, Python 3.9+ and a non-root loopback sshd. Run ctest with
`QT_QPA_PLATFORM=offscreen`; never run a real provider for this proof.

The QA runtime must come from the task's complete `cmake --install` stage and
have a clean exact build revision. Archive its top-level children directly
(no `.` member/CPack wrapper; ADR 0124 layout v1). In a task-owned directory,
generate an Ed25519 QA keypair, restrict the private file to 0600, and set:

- `KETPLUS_T51_QA_KEY`: the QA private PEM file (outside the repository).
- `KETPLUS_T51_QA_ARCHIVE`: the full staged runtime TGZ.
- `KETPLUS_T51_QA_INFO`: JSON from that staged daemon's `--build-info`.
- `KETPLUS_T51_QA_EVIDENCE`: an optional task-owned capture directory.
- `TMPDIR`: a task-owned private temporary directory.

Then run `ctest -R '^ketplus_ssh_install_ui_tests$' --output-on-failure` in the
own build tree. Without the QA inputs this opt-in host proof reports a skip;
a skip is not completion evidence. The test signs a local manifest, serves it
on loopback, uses a fresh same-UID HOME/XDG profile through a loopback sshd,
clicks the actual Connections controls, checks missing-key/signature failures and
cancellation at runtime-ready, declines once, installs and verifies an authenticated
`daemon.hello`, restarts without downloading again, then uninstalls through the idle gate and
checks that user data survives. It creates no OS user or real relay registration.

## Color-scheme rendering and native comparison

`ketplus_scheme_rendering_tests` covers same-mode fenced-code color/font updates,
Mermaid configuration cache invalidation and Markdown document/selection/scroll
preservation. `ketplus_scheme_chat_tests` checks canonical syntax on an existing
Quick text document and retains the Quick root, transcript model and selection.
The related Pro streaming, shell and Mermaid targets exercise live updates with
fake/local fixtures; they do not invoke providers.

Native evidence is opt-in and refuses to show a window unless a DELL screen is
found. Ordinary offscreen runs skip these two manual cases:

```sh
QT_QPA_PLATFORM=cocoa KETPLUS_NATIVE_SCHEME_CAPTURE_DIR=/tmp/ketplus-native-colors \
  build/lite-dev/core/ketplus_scheme_consumer_tests nativeBuiltinComparisonOnDell
QT_QPA_PLATFORM=cocoa QSG_RHI_BACKEND=metal KETPLUS_DISABLE_DAEMON_AUTOSTART=1 \
  KETPLUS_NATIVE_SCHEME_CAPTURE_DIR=/tmp/ketplus-native-colors \
  build/dev/ketplus_scheme_chat_tests nativeBuiltinComparisonOnDell
```

Review the widget pair captures and the legacy/canonical chat captures in both
modes. The targeted macOS/Qt 6.11 check used DELL P2219H; both chat pairs had zero
changed pixels. The captures contain synthetic test messages only.

If Qt 6.11 stalls while loading a cached QML dynamic plugin, a diagnostic re-run
can use `QML_DISABLE_DISK_CACHE=1`; this changes test-run cache behavior, not the
application's scheme persistence or publication. See Qt's [disk-cache
configuration](https://doc.qt.io/qt-6/qmldiskcache.html#configuration).

## Evidence-gated team routing (ADR 0129)

`ketplus_team_risk_policy_tests` exercises the daemon coordinator with a temporary
journal and fake trusted receipts: low-risk acceptance, advisory calls without a
task/worktree, mandatory/unknown domains, actor and revision fences, AC/findings,
failed/missing checks, approval/rollback gaps and legacy serialization. No real
provider, account or worktree is used. Run it with the team store/task/review and
protocol targets after policy changes; shared-header/CMake changes retain the full
Pro build/suite requirement above. The synthetic v1 JSON fixture is checked by the
policy target; no live receipt adapter is enabled by these fixtures.

### Enrollment storage and callback regression matrix

Changes to enrollment storage or callback fences run the native trust-lifecycle,
sender-key-cache, cloud-privacy, remote-control-client/projection, credential-store and Atlas
access/runtime/tunnel suites. Shared daemon/remote headers also require a full Pro build and
unfiltered Pro CTest run. Do not enable live/provider opt-ins for this matrix.

The trust-lifecycle fixture injects read failures after earlier enrollment fields succeed,
write failures at each enrollment field and delayed challenge/credential POST responses across
same-identity and different-identity enrollment. It checks complete old-record retention and
restart, cancellation of old credential waiters, Atlas retirement, stale heartbeat batches and
sign-in completion semantics. The remote fixture restores a valid session then fails the host-pin
read, requiring an explicit error and byte-for-byte retention of both previous host pins.
These are native C++ tests with synthetic credentials and HTTP replies; they do not certify OS
ACLs, backup recovery, signed upgrades or deployment.

## Windows daemon tests

`cmake --preset daemon-dev`, `cmake --build --preset daemon-dev` and `ctest --preset daemon-dev`
run on Windows (MSVC) with the toolchain from `tools/windows` (see
[development](development.md)). Windows-specific coverage: `ketplus_windows_platform_tests`
(owner-only access list on a directory and its children; killing a provider ends its helper
processes through the Job Object), `ketplus_windows_script_launch_tests` (`cmd.exe` with
`nativeArguments`, npm shim to `node`), `ketplus_cursor_invocation_tests` (Cursor launch and model
id checks), a pipe-server user check in `ketplus_windows_platform_tests`, and `HeadlessDaemonTest::windowsPipe...` (daemon and
`ketplusctl` over the named pipe, a wrong credential refused, clean exit on `CTRL_BREAK`).

Provider fixtures are Python scripts written as `<name>.py`. On macOS and Linux the script
itself is the executable (`#!/usr/bin/env python3`). On Windows `tests/daemon/ScriptFixture.h`
copies `ketplus_script_launcher` (`ScriptLauncher.cpp`) to `<name>.exe`; the launcher starts the
configured `python.exe` on `<name>.exe.py` with the same arguments and stdio, sets
`PYTHONUTF8=1`, and ends the interpreter when the launcher is killed (Job Object). A fixture name
therefore goes through `fixtureName()`, and a test that needs an absolute path that does not exist
uses `absoluteTestPath()` (`"/repo"` is not absolute on Windows). Fixture logs written by Python
carry `\r\n` on Windows; a test that matches a whole line normalises it first.

The tests that use these fixtures carry the label `windows-script-fixture`, so
`ctest --preset daemon-dev -L windows-script-fixture` runs them. They were disabled on Windows
before; they now run. Last full Windows run (`ctest --preset daemon-dev -j4 --timeout 240`):
163 tests, 156 pass, 7 do not (see below); the 40 fixture tests plus `ketplus_claude_sidecar_tests`
were all disabled before. macOS and Linux are unchanged.

Not passing in that run:

- `ketplus_daemon_journal_migration_recovery_tests` migrates and reads back the same journal 13
  times. In a Debug build on Windows it takes about 230 s, because the SQLite driver of Qt's debug
  libraries is unoptimised (every step is about 3.4x slower than on a Linux machine with release
  Qt); a Release build takes about 38 s and the migration itself 1.8 s, so NTFS and Defender are not
  the cause. The test sets its own CTest limit (900 s) and QtTest function limit on Windows.
- `ketplus_peer_session_runner_tests` and the four `ketplus_snapshot_*` tests do not build on
  Windows yet (runner snapshot code).
- `ketplus_claude_sidecar_posix_tests` is disabled: it holds the Claude sidecar JavaScript tests
  that hang or fail under `node --test` on Windows (transcript, permissions, agent-tools, usage,
  compact-window, steer, fork, background-tasks, managed-background, model-catalog, review-guard
  and review-profile). The other sidecar files run in `ketplus_claude_sidecar_tests`.

Skipped inside a test on Windows with `KETPLUS_SKIP_ON_WINDOWS(reason)`; the reason prints in the
test output.

- The fixture is a POSIX shell script (a fake `git` or `gh`) that Windows cannot run:
  `TeamReplyTest` (three tests), `TeamTaskGitTest` (two) and `TeamDoneTest` (two).
- By design: `ComputerProviderIntegrationTest::frontFollowsSettingsForEachProvider` (macOS app
  bundle path), `ProviderCompatibilityTest::prefersHomebrewUpdateForHomebrewInstalls`
  (Homebrew), `SessionForkTest::forkRequiresMediaOwnershipBeforeJournalCommit` row
  `source-unreadable` (POSIX mode) and `JournalStartupTest::startupBacklogCharacterization`
  (a 250 ms startup budget; SQLite commits are several times slower on NTFS).
- `CodexSessionIsolationTest::twoSessionsKeepHistoryWhileOnlyOneProcessRefreshes` and
  `AcpSessionIsolationTest::twoSessionsKeepHistoryAndIndependentProcesses`: the fixture records its
  exit from a SIGTERM handler, which Windows' forced process termination never runs.
- Windows cannot rename or remove the directory a running process uses as its working directory:
  the `source unavailable` row of `TeamReviewTest::aCodexReviewerIsCheckedAroundEachReview` and
  the `source removed` row of `TurnReviewTest::aCodexReviewIsCheckedAroundTheTurn`.
- `TeamStorageTest::theMainCheckoutIsRefused` links the worktree to the main checkout with
  `QFile::link`, which makes a `.lnk` shortcut on Windows, not a symbolic link.

What the daemon tests learned about Windows, for the next fixture:

- A test that edits the Python source of a fixture edits `scriptSourcePath()`, not the launcher.
- A test that connects again after the daemon refused a client uses a new `QLocalSocket`, not the
  aborted one: on Windows, an aborted `QLocalSocket` that reconnects at once connects but never gets
  a reply, while a new object against the same server does (3 of 3 runs hang when reused, 0 of 4
  with a new object). The server is not the cause. `DaemonClient` does reuse its socket after
  `abort()` (`connectToDaemon()`); it reconnects after a timer delay, and whether that delay is
  always enough on Windows is not measured.
- Git for Windows converts line endings on checkout, so on Windows only, the evidence Git run in
  the daemon sets `core.autocrlf=input` to read such a checkout as clean (a worktree file that only
  swaps LF for CRLF still reads as modified there, as on the other platforms: a test checks it); the review snapshot copies the index
  to a plain file, because git could not replace the index of a `QTemporaryFile`.
- A fixture that holds a daemon, a host and adapters tears down the adapters first.

Skipped inside tests on Windows, each with its reason in the test source: fixtures that need POSIX
symbolic links (`QFile::link` makes a `.lnk` shortcut), file-mode assertions (access is an ACL, see
`ketplus_windows_platform_tests`), permission-denial fixtures, FIFOs and resource limits, the
Linux/macOS user-unit socket probe, the uncheckpointed-WAL fixture (the only `python3` on `PATH` is
the Microsoft Store alias), and producer-flood timing (QProcess drains the pipe in a thread).
`ketplus_daemon_usage_retention_tests` passes but takes several minutes in a Debug build on
Windows; its cause has not been measured. Fixtures that write exact byte counts switch stdout to
binary mode (`BinaryStdio.h`), because the C runtime would otherwise add `\r`.

## English chrome and multilingual team content (T30)

See [the inventory](team-content-language.md). Run `ketplus_team_page_language_tests`,
`ketplus_team_language_tests`, `ketplus_team_protocol_tests`, `ketplus_team_remote_tests`,
`ketplus_team_remote_cloud_tests`, `ketplus_team_dialogs_tests`, and
`ketplus_sidebar_filter_tests` with `QT_QPA_PLATFORM=offscreen`. The page fixture sets a
Vietnamese default locale; `KETPLUS_T56_CAPTURE` writes actual Qt page/task captures.
`ketplus_team_translation_catalog_tests` validates preserved catalog completeness, rather
than requiring Vietnamese translations of newly added English chrome.

Use the existing dialog geometry fixtures for desktop/narrow/short screens and English/
Vietnamese content. Inspect a native Qt capture of New Team/settings/team/task/decision
surfaces; retain original logs and render measurements. These tests exercise fake transports
and generated instructions, not model language compliance or an installed application.
Shared desktop/header changes still require a full build and full Pro suite. Record first
failures, bounded failed-only reruns and platform skips without changing thresholds.
