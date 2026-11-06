# Linux headless daemon

KetPlus 0.1.29 can build and install the private daemon independently of Desktop.
The Linux server owns provider processes, repositories, the durable SQLite journal,
session keys and approval decisions. iOS and a remote Desktop client communicate
through Control v2. They do not connect to the local Unix socket over the network.
The daemon opens outbound HTTPS and WebSocket connections to the relay; this
topology needs no inbound TCP port or display server.

The Qt remote-client backend is implemented in `ketplus_remote_control`. Desktop
and daemon reuse the private `ketplus_control_crypto` target for keys and credentials;
the client target does not depend on provider/session-host runtime code. Desktop
UI/composition is deliberately deferred: the current Desktop still attaches to its
local daemon. Android remains the existing v1 preview; accepting `android` in the
v2 pairing protocol does not migrate that application.

## Build and stage

Use CMake 3.24+, Ninja, a C++20 compiler, OpenSSL 3 and Qt 6.11+ Core, Concurrent,
Gui, Network, Sql and WebSockets. Gui supplies image decoding; Widgets, QML,
WebView, `core/` and an X server are not needed for the headless
target. Tests additionally require Qt Test. Node 24+ and npm stage the Claude
sidecar. An already staged tree can be supplied with
`KETPLUS_CLAUDE_SIDECAR_DIR` for an offline build.

Set the intended relay URL at configure time. Release builds cannot replace it
using an environment variable. A loopback URL is suitable only for isolated tests.

```sh
cmake -S . -B build/daemon -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DKETPLUS_EDITION=pro -DKETPLUS_BUILD_DESKTOP=OFF \
  -DKETPLUS_BUILD_TESTS=ON \
  -DKETPLUS_CONTROL_URL=https://CONTROL_ORIGIN/native/TENANT
cmake --build build/daemon --parallel 4
ctest --test-dir build/daemon --output-on-failure --parallel 2
cmake --install build/daemon --prefix "$PWD/build/daemon-stage"
env -u LD_LIBRARY_PATH build/daemon-stage/bin/ketplus_daemon --check-runtime
build/daemon-stage/bin/ketplus_daemon --build-info
cpack --config build/daemon/CPackConfig.cmake -B build/packages
```

`packaging/linux/Dockerfile.build` provides an Ubuntu 24.04 Qt 6.11.1
builder for x86_64 or ARM64 (`--platform` selects the architecture). Use a native
Linux runner for releases; emulated builds are slower and
do not establish native performance. The package contains both executables,
Qt libraries/plugins, the Claude sidecar, agent skills, notices and a systemd
user unit. Test the installed tree in a clean runtime without the Qt SDK.
Qt 6.11's default QPA plugins are explicitly excluded; the acceptance helper
also rejects a staged `plugins/platforms` directory.
`--check-runtime` verifies the SQLite plugin; `ketplusctl doctor` separately
checks provider availability. Node and provider CLIs are external runtime
dependencies. Optional agent browser tools require a separately staged canonical
browser runtime; it is not included in this daemon package.

On a clean Ubuntu 24.04 runtime, install the system libraries used by Qt image
decoding/networking, plus Git and trusted root certificates:

```sh
sudo apt-get install ca-certificates git libgl1 libopengl0 libegl1 libfontconfig1 \
  libfreetype6 libglib2.0-0 libdbus-1-3 libxkbcommon0
```

These are shared libraries, not a display server. Node 24+ and selected provider
CLIs must also be installed when those providers are enabled.

`packaging/linux/Dockerfile.runtime-test` supplies a clean Ubuntu acceptance
image with those libraries and test utilities, but no Qt SDK or provider login.
Mount only the installed tree read-only at `/runtime`; its entrypoint validates
library/plugin discovery, CLI authorization, persistent catalog restart,
owner-only local token, graceful signals and systemd unit syntax. It does not
install or start a systemd service.

## Run as a dedicated user

Extract the entire staged package into a versioned directory under
`~/.local/opt/ketplus/`, and point `current` at that directory. Verify the
extracted daemon's `--build-info` URL and version before starting it. Install the
user unit from `share/systemd/user/ketplus-daemon.service` into
`~/.config/systemd/user/`, then reload the user's systemd manager and start
`ketplus-daemon.service`. A system administrator can enable lingering for that
dedicated user when the server must run after SSH logout.

The supplied unit uses:

| Setting | Value |
| --- | --- |
| Executable | `%h/.local/opt/ketplus/current/bin/ketplus_daemon` |
| Data | `%h/.local/share/Ketsuite/KetPlus` |
| Socket | `%t/ketplus-daemon-v0` |
| Optional environment | `%h/.config/ketplus/daemon.env` |
| Secrets/files | owner-only, `UMask=0077` |
| Restart | on failure, after five seconds |

The CLI and daemon share `KETPLUS_DAEMON_DATA_DIR`, `KETPLUS_DAEMON_SOCKET`,
`KETPLUS_DAEMON_STATE` and `KETPLUS_DAEMON_TOKEN_FILE`. Export the same settings
in an administrative shell, especially the unit's socket path
`$XDG_RUNTIME_DIR/ketplus-daemon-v0`. Defaults are derived from the user data
directory and runtime directory, with a shared data-directory fallback.
Linux credentials are kept in service-specific owner-only files. Protect this
OS account and its backups; it is the host trust boundary.

Install Git and the providers that the server will run. Authenticate each provider
under the service user using that provider's supported login method. The service
PATH includes `~/.local/bin`; custom paths can be supplied in `daemon.env`.
Provider login is distinct from KetPlus account login.

## Enroll, register and pair

```sh
export KETPLUS_DAEMON_SOCKET="$XDG_RUNTIME_DIR/ketplus-daemon-v0"
~/.local/opt/ketplus/current/bin/ketplusctl doctor
~/.local/opt/ketplus/current/bin/ketplusctl login
~/.local/opt/ketplus/current/bin/ketplusctl workspace add /srv/repositories/project
~/.local/opt/ketplus/current/bin/ketplusctl workspace list
~/.local/opt/ketplus/current/bin/ketplusctl pairing create
```

`login` prints an approval URL and an eight-character confirmation code. Open the
URL on your own computer/phone, sign in through the tenant gateway, compare the
code, then approve. The daemon polls with a private device code plus a PKCE S256
verifier for at most ten minutes. No browser or loopback listener runs on the
server. The page never receives the polling secret. Replays, wrong installation,
wrong verifier, expired grants and revoked users cannot issue credentials.
To cancel a pending login, use `ketplusctl call remote.sign_in.cancel`.

Workspace registration accepts an existing absolute Git worktree root, and is
local-administration only. It persists even when there are no conversations, so
iOS can create the first conversation remotely. A retained session prevents
unregistration. Paths are encrypted in remote metadata; the relay does not expose
the filesystem path as a routing identifier.

Enter the pairing ticket/code in the iOS app (or a future Desktop pairing UI).
Inspect the pending claim with `ketplusctl pairing status TICKET_ID`. For a claim
whose keys reproduce its digest it prints `claim_verified: true`, the
`confirmation_code` to compare with the code the phone shows, and the
`claim_digest`; an unverified claim has no code and cannot be approved. Approve
that exact claim with `ketplusctl pairing approve TICKET_ID CLAIM_DIGEST`. The daemon refuses the
approval when the ticket's digest changed or does not match the claimed keys,
and pins the device only on this machine (ADR 0034).
Enrollment is account authentication; pairing separately grants the device
access to host-wrapped session keys. Revoke a ticket or device with
`ketplusctl pairing revoke TICKET_ID` / `ketplusctl device revoke DEVICE_ID`.

`ketplusctl sessions` lists local sessions. `ketplusctl call OPERATION -` reads
an authenticated local command payload from stdin. This CLI never starts a
daemon automatically and must run as the daemon's OS user.

## Upgrade and verification limits

SIGTERM/SIGINT wake the Qt event loop and shut the daemon down normally; the next
start reconciles interrupted turns from the journal. Restarting the service still
interrupts provider work. Check active turns/approvals and wait for idle before
switching `current` to a complete new version and restarting. Never replace a
single executable in an existing deployment.

Local deterministic tests use fake providers and isolated data directories. They
do not log in to a real provider, consume model quota, install to a user's service,
or prove a physical phone/network deployment. Production acceptance still needs
the intended relay deployment, a native server, provider authentication, a real
pairing round and restart/offline recovery on that server. The CI Linux job builds,
tests, stages and archives the headless target independently of Desktop.

The scoped results and remaining deployment checks for 0.1.29 are recorded in
[`linux-headless-verification.md`](linux-headless-verification.md).

## Qt backend prepared for Desktop

`modules/remote/src/control/ControlV2Client` provides origin-scoped credentials,
refresh recovery, key-bound pairing/cancellation, session/worktree indexes,
authenticated event decryption, encrypted command preparation/submission and
host-signed command-result decryption. Persist the prepared envelope and retry
exactly that ciphertext. It never resolves server paths or starts a local daemon.
Revoked credentials fence in-flight responses. Sign-out also reports a local
credential-file removal failure instead of claiming a durable successful logout.

Desktop now uses a loopback PKCE browser handoff to obtain a UserSession for the
backend's installation ID, and Connections can claim and approve a Linux pairing
link after an explicit identity comparison. The sidebar lists the local machine
and any remote machine IDs visible in the worktree index. Session/worktree index
metadata remains sealed; presentation,
sealed metadata projection, large-event assembly, session creation/restore,
attachments and a durable Desktop command outbox are future integrations. The
current backend supports ordinary session controls and workspace-file requests;
it is not a complete remote Desktop product. A changed host signing key version
in an event page fails closed; historical-key selection is also a future extension.
