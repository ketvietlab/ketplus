# 0124: Consented user-space daemon installation over SSH

Status: accepted; production installs remain disabled until the release owner supplies the public key
Date: 2026-11-13

Issue: [#666](https://github.com/ketvietlab/ketplus/issues/666), following #419.
Extends [ADR 0119](0119-remote-machine-composition.md).

## Decision

Connections may install a complete matching Linux daemon runtime after two explicit
steps: **Review signed install / upgrade** authorizes fetching release metadata;
**Confirm daemon changes** authorizes downloading the selected archive and changing
the named Linux account. Accepting an SSH host key, reconnecting or opening the
Connections dialog never authorizes installation. Decline downloads no archive
and changes no installation files.

The review names the SSH destination and fingerprint, version, architecture,
archive size, verification, user-space location, startup and rollback behavior.
Consent is bound to the tunnel generation, verified identity, signed artifact,
profile paths and existing installation receipt. Changed inputs require review
again. No password field or storage, sudo, OS-user creation, system package
installation, SSH configuration change or relay enrolment is included.

### Release channel and publisher contract

The release owner's decision is GitHub Releases in **ketvietlab/ketplus**, with a
dedicated ketPlus Ed25519 release key. The owner holds the private key; a future
Linux packaging/release job signs through repository secret
`KETPLUS_RELEASE_SIGNING_KEY`. This task documents that contract only: it does
not add a secret, publish a release or enable a signing workflow.

Desktop resolves its own product version, not `latest`, to tag `v<version>` and
build-configured `KETPLUS_DAEMON_RELEASE_MANIFEST_URL` (default):

```text
https://github.com/ketvietlab/ketplus/releases/download/v<version>/ketplus-daemon-linux-manifest.json
https://github.com/ketvietlab/ketplus/releases/download/v<version>/ketplus-daemon-linux-manifest.json.sig
```

`KETPLUS_DAEMON_RELEASE_PUBLIC_KEY` is one CMake cache constant containing exactly
64 lowercase hex digits (32 raw Ed25519 public-key bytes). **Its default is empty.**
An unset key or manifest URL yields **Install unavailable: no trusted release
channel**, with no download. There is no environment override or trust-on-first-
use for release keys. The artifact never supplies its own verification key.
The owner must ship key rotation/revocation in an updated desktop; there is no
network key-discovery mechanism.

The signature file is exactly **64 raw Ed25519 signature bytes**, over the exact
UTF-8 bytes of the manifest file. Do not reserialize JSON, append a newline or
base64-encode the signature after signing. A publisher can use:

```sh
openssl pkeyutl -sign -rawin -inkey "$RELEASE_KEY_FILE" \
  -in ketplus-daemon-linux-manifest.json \
  -out ketplus-daemon-linux-manifest.json.sig
```

`RELEASE_KEY_FILE` is a permission-restricted temporary file populated by the
release job from its secret, never a repository file or printed command value.
The future job must delete that temporary file. The manifest schema is:

```json
{
  "schema": 1,
  "product": "ketplus-daemon",
  "version": "1.2.3",
  "tag": "v1.2.3",
  "revision": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "control_url": "https://control.example",
  "artifacts": [{
    "os": "linux",
    "arch": "x86_64",
    "layout": "ketplus-runtime-v1",
    "url": "https://github.com/ketvietlab/ketplus/releases/download/v1.2.3/ketplus-daemon-linux-x86_64.tar.gz",
    "bytes": 123456,
    "sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
  }]
}
```

Versions and control URLs must match the desktop; revision is the daemon's exact
12–40 lowercase-hex `--build-info` revision (no dirty builds). Architectures are
`x86_64` and `aarch64`, selected from the verified host's Linux preflight. Exactly
one matching artifact is required. The manifest is limited to 64 KiB and eight
artifacts; compressed archives to 512 MiB and expanded content to 2 GiB.
Downloads use HTTPS, validate TLS normally, and allow at most five redirects,
only to the matching GitHub release path or GitHub's release asset hosts
`release-assets.githubusercontent.com` and `objects.githubusercontent.com`.
No downgrade to HTTP, credential-bearing URL or redirect outside these hosts is
accepted. QA-only constructor injection allows an HTTP server on `127.0.0.1`
and its exact port; production never reads this setting from the environment.

The package is the **entire `cmake --install` runtime**, including daemon, CLI,
Qt libraries/plugins, sidecar and notices. `ketplus-runtime-v1` puts `bin/`,
`lib/`, `plugins/`, etc. directly at the archive root, **without a CPack wrapper
or `.` root member**. `bin/ketplus_daemon` and `bin/ketplusctl` are required.
Reject traversal/absolute/duplicate names, hard links, special files, privileged
mode bits and symbolic links escaping the runtime or used as entry ancestors.
Safe internal runtime symlinks are preserved. The publisher must normalize CPack
output to this layout before hashing/signing; it cannot publish an unmodified
wrapper-directory CPack archive as layout v1.

### Verified transport, ownership and startup

Use the existing system-OpenSSH route and no-prompt environment. Every call
rechecks an immutable private snapshot of the verified host key (or CA/revocation
files for certificate authentication), with strict checking and no multiplexing,
forwarding, password, askpass, tty or user RemoteCommand/LocalCommand. Never
construct a second permissive SSH connection. Fixed Python source and base64 JSON
arguments travel as quoted data; archive bytes travel through stdin. Python 3.9+
on Linux is a prerequisite, not installed automatically.

Preflight is read-only. Reject custom `daemon.env`, an existing user service,
unmanaged installation or running daemon, foreign-owned/symlinked parent paths,
paths containing parent traversal, and modified or additional files in the managed
runtime inventory. State files outside the daemon data directory require manual
management. Normal custom/manual installations retain the existing manual-management path; this installer does not adopt or overwrite them.
An installed but stopped managed daemon offers **Start managed daemon** separately.

Versioned complete runtime:
`~/.local/opt/ketplus/<version>-<revision>-<arch>/`; `current` is an atomic relative
activation symlink and `ssh-install.json` is the owner-only receipt. Data, socket
and credential paths follow the existing SSH profile/environment defaults.

Startup is a detached **user process**, explicitly stated in consent. It does not
create/enable a systemd unit, login service, lingering or automatic startup. This
keeps the SSH operation independent of a user service manager and allows isolated
same-UID testing without altering the account's real services. Reopening a stopped
managed installation uses the explicit Start action. A future login-service
feature would require separate consent and a separate decision.

### Transaction, upgrade, rollback and uninstall

Hold an exclusive nonblocking lock on the installation directory. Recheck the
receipt/profile after acquiring it. Use a fresh owner-only staging directory on
the same filesystem. Desktop verifies the manifest signature before trusting any
field and checks the complete archive SHA-256/length before upload; the host checks
length/hash again before extraction. Run the authenticated daemon's `--build-info`
and `--check-runtime` before activation. Version, exact revision and control URL
must agree with the signed descriptor.

Upgrade accepts only a strictly newer version. It uses the existing authenticated
`daemon.prepare_upgrade` RPC, which atomically refuses active sessions/approvals
before closing the listener; it never kills an existing daemon. After it stops,
copy the existing data directory to a private rollback snapshot, activate the new
complete runtime and start it. Old version directories are retained for recovery.

A missing local IPC credential is initialised with the daemon's existing 32-byte
CSPRNG/base64url format in an exclusive owner-only file. Rollback removes it only
if this transaction created it; existing credentials are retained, including a
configured token path outside the data directory. No token enters the manifest,
process arguments, logs or the repository.

The SSH transaction remains open after startup. Only a real authenticated
`daemon.hello` through `SshMachineBackend` with the same verified SSH identity lets
Desktop send `commit`. Cancellation is honoured until this commit decision; once
sent, the transaction finishes rather than claiming it can undo an acknowledged
commit. EOF, decline, cancellation, failed hello, disconnect or a 90-second
confirmation timeout rolls back the runtime link, receipt and data
snapshot and stops only the newly started child. It releases staging before
restarting the previously running managed daemon and confirms that the restored
process owns the socket before reporting rollback complete. Interrupted upload or
runtime validation changes no activation.
The helper also handles SSH hangup/termination by running cleanup. Errors expose
fixed messages, not subprocess stderr, tokens, journals or server banners.
If SSH is lost during rollback or the host fails, Desktop reports that manual
recovery may be required; it does not claim distributed atomicity across a host
crash. The private staging directory retains recovery material if restoration
cannot complete, rather than deleting the only snapshot.

Uninstall requires its own review and confirmation. New authenticated
`daemon.stop_if_idle` uses the same active-session gate as upgrade, without
inventing a replacement version; older daemons without it refuse safely. Remove
only the receipt-owned active runtime and activation link after confirmation.
Conversations, credentials, provider configuration and previously retained runtime
versions remain. Removing a machine from Connections is still only a local trust/
connection operation and does not uninstall anything remotely.

### Verification and production enablement

Linux proofs use a task-owned QA keypair and local manifest/artifact server, plus
an isolated **same-UID HOME/XDG profile**, not a newly created OS user. The private
QA key stays outside the source tree. The full staged QA daemon is built with a
loopback control URL and an empty isolated credential store; it must never enrol
or connect as a real machine to any hosted relay. Stop only test-owned processes
and delete only test-owned files.

Offscreen coverage includes real Connections controls, verified SSH, the release
verifier/downloader, decline, complete runtime install and authenticated hello,
idle-only uninstall, filesystem/transfer/archive failures and rollback. Runtime
publisher signing/key provisioning remains the release owner's Linux packaging
work; until the public key is supplied, production install remains disabled.
