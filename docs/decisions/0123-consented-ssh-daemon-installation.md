# 0121: Consented user-space daemon installation over SSH

Status: proposed — release trust/distribution decision required before installation
Date: 2026-11-13

Issue: [#666](https://github.com/ketvietlab/ketplus/issues/666), following #419.
Extends [ADR 0119](0119-remote-machine-composition.md).

## Context

Connections verifies an SSH host and probes for the daemon socket and token.
`SshTunnelBridge.cpp:13–35` reports a missing socket or token; the desktop maps
that to `LinuxMachine::Error::DaemonNotInstalled` in
`apps/desktop/src/remote/LinuxSshMachines.cpp:46–47`. This also includes an
installed daemon that is stopped, so that result alone cannot authorize or
select installation.

The Linux CPack branch in `packaging/CMakeLists.txt:1–8` stages a relocatable
TGZ with the daemon, CLI and runtime. The Linux job in `.github/workflows/build.yml`
builds/tests/stages it and uploads a CI artifact. At integration `305ac716` it
publishes no signed Linux install manifest or release-signature trust key.
The repository's GitHub release list was empty during this audit. CI artifacts
and transport-level TLS are not an application release-signature contract.

## Decision proposed

### Consent and scope

Before any remote installation write, perform read-only preflight over the
already verified SSH route: Linux/architecture/ABI, account ownership, required
runtime dependencies, existing installation and process/service state. An
installed-but-stopped daemon offers Start separately; it is not overwritten.
Unknown existing binaries/paths require manual resolution. A changed SSH host
key fails closed through the existing host-key verification flow.

Show an explicit confirmation naming the verified host/fingerprint, exact
version/build/architecture, artifact size, user-space location, files/service
changes, startup behavior and rollback. Install and Cancel are separate actions.
A reconnect, background retry or initial host-key acceptance is not install
consent. Consent is bound to the target, verified identity, selected artifact
and connection generation; changed inputs require a new confirmation.

No password field/storage, sudo, system package installation, system unit,
firewall rule, SSH configuration edit or lingering change is included. Missing
prerequisites are explained without modifying the host. Installing Node/provider
CLIs and provider authentication remain separate user actions.

### Trusted artifact and verification

Use the complete matching Linux runtime, never a copied build-tree executable.
A release descriptor must bind product, version, immutable source/build id,
Linux architecture and supported ABI, control-environment identity, HTTPS
artifact URL, byte length and SHA-256. Desktop rejects mismatches, unknown
architectures, oversized input, redirects outside the approved release origins,
downgrades and absent descriptors. It never substitutes the latest unrelated
build or silently switches a staging client to a production daemon.

Proposal: a detached Ed25519 signature covers a versioned, domain-separated
manifest byte sequence. Verify it with a release-owner-approved public key
pinned in the shipped client, then verify the entire archive's signed length and
SHA-256 before extraction or execution. The artifact does not supply its own
trust key. Pin the metadata format, allowed signing keys/key ids, exact signature
encoding and rotation/revocation policy in the release contract and verifier
fixtures before enabling Install. Private keys stay with the release owner and
never enter this repository, an SSH command, logs or the client.

Do not treat an adjacent unsigned checksum file, SSH host authentication, the
relay's runtime identity key or the macOS Sparkle configuration as a substitute
for this Linux release trust contract. No key is generated or selected by the
installer. Test signatures use ephemeral test keys and cannot enable production
installations.

### Location, transport and transaction

Versioned complete runtime: `~/.local/opt/ketplus/<version>-<build>-<arch>/`;
`current` is the activation link used by the existing user unit. User unit:
`~/.config/systemd/user/ketplus-daemon.service`. Runtime/data/token locations
remain those documented in `linux-headless-daemon.md`. Installation files and
receipts are owner-only; data and authentication material are never copied into
Desktop logs or saved SSH-machine records.

Use the existing strict verified SSH options and no-prompt environment for
preflight, upload, activation and rollback. Every SSH call checks the pinned
host identity; it must not use an independent permissive SSH invocation. Send
binary/script input through stdin with bounded I/O and timeouts, not commands
interpolated from host responses, paths or archive names. Revalidate the target
and generation after each asynchronous step and before mutation.

Stage in a new random owner-only directory on the same filesystem. Extract only
validated package entries into that empty directory: reject absolute/traversing
paths, escaping symbolic/hard links, special devices and unexpected top-level
layout; preserve only safe runtime links and executable permissions. Validate
runtime dependencies and the verified daemon's build information before
activation, without a provider turn or access to an existing user journal.

Serialize installation per account/location. Refuse to overwrite a different
installation or race an active daemon. Record the pre-install state of every
file/link/service setting that this transaction can change. Activate atomically
only after verification; start the user service and verify the expected daemon
hello/identity before reporting Connected. No user systemd manager means an
actionable prerequisite failure, not an automatic privileged workaround.

Declining consent performs no remote writes. On verification, transfer,
extraction, validation, activation or initial connection failure, stop only a
process this transaction started, restore its previous link/unit/service state,
and remove only its own staged files/new bootstrap data and newly-created empty
directories. Never delete existing user data, credentials or another install.
Test this invariant with before/after filesystem and service snapshots, including
cancellation at each mutation boundary. Cleanup failures are reported precisely;
they must not be presented as an unchanged machine or a successful install.

### Upgrade and uninstall

Upgrades require the same explicit artifact-bound consent and signature checks.
Do not interrupt active turns, pending approvals or background work; defer until
the host is idle and re-check immediately before replacement. Keep the previous
complete runtime until the new version connects successfully; restore it if
health/connection checks fail. No arbitrary downgrade or automatic journal-format
rollback is allowed. An incompatible migration requires a separately approved
backup/recovery policy before an upgrade can be offered.

Uninstall is a separate confirmation. Require idle state, stop only the managed
user service, and remove only links, the unchanged managed unit and versioned
files named in its installation receipt. Modified/unrecognized files require
manual resolution. Keep journals, repositories, credentials and provider state
by default. Erasing user data is a separate explicit destructive action, not
part of this installer.

### State machine and validation

`Disconnected -> Verified -> Preflight -> AwaitingConsent -> Fetching ->
Verifying -> Staging -> Activating -> Connecting -> Connected`.

Cancel/failure before mutation returns without host changes; after mutation it
must finish transaction rollback and report the exact result. No continuation
from a stale connection generation can execute a later installation step.

Offscreen tests cover decline, stale consent, host-key change, wrong version,
architecture/environment, bad/missing signature, wrong digest/size, unsafe
archive entry, interrupted transfer, cancellation/rollback, existing stopped or
busy daemon, startup failure, upgrade rollback and uninstall ownership guards.
Use the existing real-daemon/loopback-SSH fixture with FakeCodex, plus a clean
Linux account/profile to prove consent-to-Connected and failure/decline snapshots.
Never consume provider quota or modify an existing deployment for this proof.

## Decision needed before implementation

The release owner/leader must select and supply the authoritative Linux artifact
channel plus the pinned release public key/provenance policy and matching signed
manifest/encoding. Approve the exact descriptor/signature contract before its
publisher and installer are implemented. Until then the production Install path
remains disabled; no binary is downloaded, executed or installed by this task.
A locally generated test signature is not evidence that a real release is trusted.

Also identify an isolated Linux test account or approve a clearly labelled fresh
user-space profile in the task-owned test directory. A profile under the existing
UID is not reported as a separate fresh OS user. No sudo/account creation is
assumed by the test harness.

## Consequences

Installation is explicit, account-scoped and auditable, preserves the verified
SSH boundary, and shares no provider credentials. The extra release-trust
contract is required to offer a safe one-click path; source/runtime packaging
alone is insufficient. Connections keeps its existing missing-daemon explanation
until the required release inputs and implementation/tests exist.

## Alternatives considered

- Manual installation: remains available while the signed release contract is
  pending; does not meet the requested Connections flow on its own.
- `curl | sh`, unsigned CI artifacts or checksums fetched beside a binary:
  rejected because they do not establish release authenticity.
- Copying Desktop's bundled daemon: rejected across OS/architecture and because
  a raw executable omits the Linux runtime and sidecar.
- Root/system-wide installation or automatic OS/provider setup: outside this
  user-space consent and ownership boundary.
