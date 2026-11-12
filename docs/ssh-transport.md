# SSH transport

Status: transport primitive (`ketplus_remote_ssh`) and the ADR 0119 machine
backend over it (`ketplus_remote_ssh_backend`, see "Machine backend") are
implemented. They are not yet composed into Desktop: no UI adds a host yet.

Desktop can reach the KetPlus daemon of a Linux host that it can already reach
with OpenSSH. The relay ([`remote-transport.md`](remote-transport.md)) needs
pairing and an account; this route needs only the user's own SSH access, and
the relay sees nothing.

## Route

`ketplus::remote::ssh::SshTunnel` drives the system `ssh` client. It never
implements SSH itself, so `~/.ssh/config` (Host aliases, ProxyJump, identity
files) and the user's agent (`SSH_AUTH_SOCK`) apply as in a terminal. Every call
adds `-T` (and `-n` unless stdin carries the daemon connection), `BatchMode=yes`,
`PasswordAuthentication=no`, `KbdInteractiveAuthentication=no`,
`StrictHostKeyChecking=yes`, `UpdateHostKeys=no`, `CheckHostIP=no` and a connect
timeout, so ssh never prompts and never edits a known_hosts file. Every call
also overrides the parts of a user's config that would change what runs or
where: `ControlMaster=no` and `ControlPath=none` (a live master would skip the
key exchange and run commands through the user's session),
`ClearAllForwardings=yes` (a `LocalForward` already bound by the user's own
session would otherwise kill the call), `RemoteCommand=none`,
`PermitLocalCommand=no`, `ForwardAgent=no` and `ForwardX11=no`. Calls that can
authenticate run with `LogLevel=ERROR`, so a server banner never reaches the
stderr KetPlus classifies; the host-key probe runs with `LogLevel=INFO` because
it never authenticates and needs ssh's "no matching host key type" line. The
child environment sets `SSH_ASKPASS_REQUIRE=never`, which also covers
`ProxyJump` hops: they read the user's config, not these options.

1. **Resolve.** `ssh -G <alias>` gives the effective config; its
   `UserKnownHostsFile` and `GlobalKnownHostsFile` entries are the user's
   known_hosts files.
2. **Probe the host key.** `ssh` runs with both known_hosts options set to
   `/dev/null` and `KnownHostsCommand` set to a private script that records
   `%H %t %K` (name, type, key) in a 0700 temporary directory. With no known key
   ssh refuses the host before authentication, so the probe offers no
   credential and runs nothing on the host.
3. **Verify.** `ssh-keygen -F <name>` looks the name up in each user file and
   in the KetPlus-owned file, matching plain, `[host]:port` and hashed entries
   as ssh does. `ssh -G` prints known_hosts paths unquoted, so a path with a
   space is rejoined when the joined path exists.
   - **Verified**: a known key of the offered type matches.
   - **Revoked**: an `@revoked` line names the offered key. The tunnel stops in
     `HostKeyMismatch` ("The host key is revoked."); the key is never offered
     for acceptance or recorded.
   - **Mismatch**: a known key of the offered type differs, or only other types
     are known and a second probe restricted to those types (`HostKeyAlgorithms`)
     returns a different key or finds that the host offers none of them ("no
     matching host key type"). The tunnel stops in `HostKeyMismatch`, and
     `fingerprint()` is the offending key (from the second probe, or the first
     one when the host offers no known type). It never retries and never asks.
     A second probe that cannot reach the host or times out is `Offline`, not
     a mismatch.
   - **Trusted by a CA**: no plain key is known but an `@cert-authority` line
     names the host. ssh checks the certificate itself: the later calls keep
     ssh's algorithm order instead of the verified type. A host that presents
     no valid certificate fails closed in ssh (`HostKeyMismatch`).
   - **Unknown**: nothing is known for the name. The host-key prompt callback
     receives the key; accepting appends it to the KetPlus known_hosts file
     (`<GenericDataLocation>/Ketsuite/KetPlus/ssh/known_hosts`, file 0600,
     directory 0700). Refusing, or having no callback, ends in
     `HostKeyRejected`. The user's own known_hosts files are never written.
4. **Locate the daemon socket and token.** `ssh <alias> "exec /bin/sh -c '...'"`
   runs a POSIX sh program that resolves them as the user unit does
   ([`linux-headless-daemon.md`](linux-headless-daemon.md)). Each setting comes
   from the environment, then the last matching line of
   `~/.config/ketplus/daemon.env`, then the unit's default:
   - socket: `KETPLUS_DAEMON_SOCKET`, else `$XDG_RUNTIME_DIR` or
     `/run/user/$(id -u)`, else the data directory, followed by
     `/ketplus-daemon-v0`;
   - data directory: `KETPLUS_DAEMON_DATA_DIR`, else
     `${XDG_DATA_HOME:-~/.local/share}/Ketsuite/KetPlus`;
   - token: `KETPLUS_DAEMON_TOKEN_FILE`, else `<data dir>/daemon/local.token`.

   It prints `ketplus-socket <present|missing> <path>` (a path with whitespace
   or `:` is refused) and, for a present socket, one token line:
   `ketplus-token <base64>` only when the file is a regular file of at most
   4 KiB owned by the user with no group or other permission bits,
   `ketplus-token-permissive` when it is readable by others (its bytes are
   never printed), or `ketplus-token-missing`. A present socket without a token
   is `DaemonSocketMissing` ("KetPlus daemon not installed or not
   initialised"); a permissive token is `DaemonTokenInsecure` and fails closed.
5. **Bridge.** `ssh -W <remote socket> <alias>` carries the daemon socket on the
   forwarder's stdin and stdout, so ssh binds no listener and
   `ClearAllForwardings` stays on. A `QLocalServer` in the private 0700
   temporary directory (owner-only, one connection, then closed) bridges that
   stdio to the `QLocalSocket` that `device()` returns.

`device()` is valid only while `Connected`. The caller must send `daemon.hello`
on it promptly: the daemon closes a connection without a hello after 10 s,
which the tunnel reports as a loss and reconnects. `daemonCredential()` is the
token file's bytes while `Connected` and empty otherwise. It is held in memory
only, cleared (and overwritten) on `close()`, every teardown and every failure,
and never logged or persisted.

Steps 4 and 5 pass the user files and the KetPlus file as `UserKnownHostsFile`
and restrict `HostKeyAlgorithms` to the verified key type, so ssh checks the key
again; a key that changes after the probe fails closed in ssh itself.

## OpenSSH versions

- Client: OpenSSH 8.5 or later, for `KnownHostsCommand` (step 2). It also
  covers `ssh -W <socket path>`, which needs Unix-domain socket forwarding
  (`direct-streamlocal@openssh.com`, OpenSSH 6.7).
- Host: OpenSSH 6.7 or later with `AllowStreamLocalForwarding` left at its
  default (`yes`, or `local`). With it disabled, or with a stale socket file
  that nothing accepts on, locate succeeds but every forward fails: the tunnel
  stays `Offline` with growing backoff and never reports `Connected`.
- Tested with OpenSSH 9.6p1 (Linux) and 10.0p2 (macOS).

## States

| State | Meaning | Next |
| --- | --- | --- |
| `Connecting` | Resolving, probing, locating or forwarding. | |
| `Connected` | The forwarder stayed up for `settleMilliseconds` (0.5 s) after the bridge connected, or carried daemon bytes sooner; `device()` is the daemon connection. | |
| `Offline` | The host, the forward or the daemon connection failed or closed. | Reconnects. |
| `DaemonSocketMissing` | The host is reachable but no daemon listens at the resolved socket, or it has no local token. | Reconnects. |
| `DaemonTokenInsecure` | The daemon token is readable by other users. | Waits for `open()`. |
| `AuthFailed` | ssh reported `<user>@<host>: Permission denied (...)` on a line of its own. | Waits for `open()`. |
| `HostKeyMismatch` | The host key differs from a known key. | Waits for `open()`. |
| `HostKeyRejected` | The key was unknown and not accepted. | Waits for `open()`. |

A reconnect repeats the whole route, including host-key verification. Its delay
doubles from `initialBackoffMilliseconds` (1 s) up to
`maximumBackoffMilliseconds` (30 s). It resets only when daemon bytes come back
through the channel, not when the settle timer fires: the timer shows only that
the forwarder is still running, and a slow link or a ProxyJump hop can fail
later. A bridge that fails within the settle period never reports `Connected`;
one that fails later reports it at most once per growing backoff interval.

`hostKeyStatus()` follows the same steps: `Verified` once the key matched (on
the CA path once ssh accepted the certificate in the locate call, even when
the daemon socket or token is then missing),
`Mismatch` whenever the tunnel stops in `HostKeyMismatch`, including a refusal
by ssh in the locate or forward call. Teardown (`close()`, a failed attempt or
the destructor) kills any running ssh child without blocking: the child is
detached from the tunnel and deletes itself once it is reaped, so no `QProcess`
is destroyed while it runs.

## Security properties

- No byte reaches the daemon before the host key is verified or explicitly
  accepted: the probe stops before authentication, and later calls require
  ssh's own strict check against the same files.
- KetPlus stores no secret. `SshTarget` persists only the alias, user and port;
  the KetPlus known_hosts file holds only accepted public keys. Passwords,
  passphrases and keys stay with OpenSSH and the agent.
- The target alias and user are restricted to ssh_config tokens (no leading
  `-`, no whitespace or shell metacharacters), so they cannot become options.
- Logs (`ketplus.remote.ssh`) contain at most the alias, the state and the
  fingerprint. ssh's stderr is only classified, never logged or shown. Each
  pattern is anchored to a whole line in ssh's own wording.
- The daemon token is read only over a verified, authenticated connection,
  only when it is owner-only, and lives only in memory while `Connected`.
- The bridge socket lives in a 0700 temporary directory, is owner-only and
  accepts one connection.
- `open()` resets the host-key status, fingerprint, socket path and last error,
  so nothing from an earlier attempt describes the current one.

## Tests

`ketplus_remote_ssh_tunnel_tests` starts a non-root loopback `sshd` on
`127.0.0.1` with throwaway host and user keys and a client config passed with
`-F` (`IdentityAgent none`, `IdentitiesOnly yes`, fixture known_hosts, no
`~/.ssh` file). That config also sets `ControlMaster auto` with a
`ControlPath` and `ControlPersist`, a `RemoteCommand`, a `LocalForward` and a
`LocalCommand`; the cases check that none of them ran. The server has Ed25519
and ECDSA host keys and a banner that imitates ssh's own refusals. `sshd` sets
`KETPLUS_DAEMON_SOCKET` and `KETPLUS_DAEMON_TOKEN_FILE` for the session and a
`QLocalServer` plays the daemon. The cases cover acceptance of an unknown key
(recorded and reused without asking; the token is exposed while connected,
cleared on close and absent from every log message), rejection, a changed key
(no prompt, no reconnect, no daemon connection), a second probe that verifies
a key of another known type, finds a different one, or finds no known type,
a second probe that cannot reach the host (`Offline`), `@revoked` and
`@cert-authority` lines, a refused account, a stopped daemon that later starts,
a permissive or missing token, a key that changes between the lookup and
locate (`Mismatch`), a stale daemon socket that fails every forward (never
`Connected`, backoff grows), a forward that fails after the settle timer
reported `Connected` (the backoff still grows), a CA-signed host certificate
accepted with no daemon running (`Verified`), destroying a connected tunnel without a
`QProcess` warning, and reconnection after the daemon drops the
connection (the token is empty in every state but `Connected`). Parser,
error-classification and socket/token-resolution cases run without sshd. Where `/usr/sbin/sshd` is missing
or cannot run as the user, the sshd cases skip.

## Machine backend

`SshMachineBackend` is the ADR 0119 `MachineBackend` for a Linux host
([ADR 0119](decisions/0119-remote-machine-composition.md)). It composes three
parts and adds no protocol of its own:

- `SshTunnelLink` wraps `SshTunnel` (`SshLink` is the seam tests replace).
- A `DaemonClient` in its remote mode (`DaemonClient(CredentialSource)`)
  adopts the tunnel's connection with `adoptConnection()` on every
  `Connected`, sends `daemon.hello` with the host daemon's local token, and
  queues and replays requests exactly as for This Mac. It never starts,
  replaces or reconnects a daemon: the tunnel owns reconnects. The token is
  `daemonCredential()`, read when the hello is sent and never stored.
- `DaemonMachineBackend` maps the operations to the local request types.

| Tunnel state | Machine state | Reason |
| --- | --- | --- |
| `Connecting` | Connecting, or Reconnecting after a session | |
| `Connected` | Connecting until the hello, then Connected | |
| `Offline` | Reconnecting after a session; Offline for a host never reached | "The host cannot be reached over SSH." |
| `DaemonSocketMissing` | Offline (the tunnel keeps retrying) | "ketPlus daemon not installed or not running on this host." |
| `DaemonTokenInsecure` | Offline | "The ketPlus daemon token on this host is readable by other users…" |
| `AuthFailed` | Offline | "SSH refused the sign-in…" |
| `HostKeyMismatch` | Offline | "The host's SSH key changed…" |
| `HostKeyRejected` | Offline | "The host's SSH key was not trusted." |
| `Idle` after `disconnectFromHost()` | Offline | "Disconnected." |

A request made while Offline fails at once with the reason, and so do the
calls waiting for a hello when the machine becomes Offline.
`connectionDetails()` gives Connections the route (`SSH`), the target, the
machine id, the SSH host key fingerprint only once it is verified (a key that
did not verify is `presented_ssh_host_key_fingerprint`), the state and the
reason. `connectToHost()` is a first attempt: Connecting, not Reconnecting. It
stays closed after a changed daemon identity, but retries after a daemon that
named none, so updating ketPlus on the host is enough.

### Machine identity and the machine store

The machine id is the daemon's host key fingerprint from `daemon.hello`
(`host_key_fingerprint`, feature `daemon.host_identity/1`), which the daemon
has without a cloud sign-in. The remote client requires it
(`setHostIdentityRequired`): a hello that names no fingerprint, or a different
one from the trusted id, fails closed before any queued request is sent.

`SshMachineStore` keeps, in
`<GenericDataLocation>/Ketsuite/KetPlus/ssh/machines.json` (file 0600,
directory 0700), each target (alias, user, port), the machine id last trusted
for it and the conversation bindings (`conversation id -> machine id`). It
holds no token, key or password, and it never overwrites a file it cannot
read. `parseSshDestination()` splits Connections' `user@host[:port]` (or
`[v6]:port`) input into an `SshTarget`.

- The first hello of a new target records its machine id
  (`trustFirstIdentity`).
- A later hello with another fingerprint makes the machine Offline with
  "This machine's identity changed", exposes the presented id, closes the
  tunnel and ignores Connect. `trustPresentedIdentity()` (Connections' "Trust
  the new identity") replaces the id and moves every binding of the old id to
  the new one in one write, then connects. Removing the machine forgets the
  target, its id and the bindings no other saved target shares.

### Accepted host keys

A first-use host key that the user accepts in Connections goes into the
KetPlus-owned known_hosts file (`SshTunnel::defaultKnownHostsFile()`), and
its known_hosts name (`[host]:port` or the `HostKeyAlias`) is recorded with the
target in `machines.json` (`accepted_host_keys`). `KnownHostsFile.h` edits only
that file: `removeKnownHost()` removes a name from plain and hashed (`|1|`)
lines as `ssh-keygen -R` does, and keeps comments, other hosts and `@revoked` /
`@cert-authority` lines. The user's own known_hosts files are never edited.

- Removing a machine also removes the keys accepted for it that no other saved
  target shares.
- After a host key mismatch, `forgetAcceptedHostKeys()` removes the old key
  when ketPlus accepted it. Connections then runs the first-use step again, so
  the new key is compared before it is accepted. If the old key is in the
  user's own known_hosts, Connections shows the `ssh-keygen -R` command
  instead.
- Only the connect card answers a host key prompt. A background reconnect that
  meets an unknown key rejects it.

## Connections

`LinuxSshMachines` (apps/desktop) owns one `SshMachineBackend` per saved
target and writes each machine into `LinuxMachineModel`. Each machine is keyed
by its ADR 0119 machine id (a saved target whose daemon has not named itself
yet is `ssh:<target>`). The model shows its live state, the verified SSH host
key, a presented key that did not verify (as a mismatch, never as verified),
and an error the user can act on. A host that the Relay lists under the same
machine id is one row reached over SSH, and the Relay's id is a route
attribute. While SSH cannot reach that host (Offline, with no error or a
connection failure) and the Relay is connected, the row falls back to the
Relay: its state, Connect / Disconnect and `machineBackend(id)` are the
Relay's. A key mismatch or a changed identity is never hidden behind the
Relay. After an identity change, Details shows the trusted and the presented
daemon identity before Trust new identity, which is disabled when the daemon
named none. The `ssh-keygen -R` hint names the entry as ssh files it
(`'[host]:port'` for a port other than 22). The connect card adds the target when Connect is pressed. An
attempt that fails or is cancelled before the first connection removes the
target and any key accepted for it. Connect, Disconnect, Remove, Forget old
key and Trust new identity act on the backend. Saved machines with a trusted
identity connect when Connections opens.

`LinuxRelayMachines` (apps/desktop) is the Relay half. It owns one
`RelayMachineBackend` for each host that the Relay's worktree index lists,
with one owner-only outbox file per host
(`<GenericDataLocation>/Ketsuite/KetPlus/relay/outbox-<hash>.json`). The links
run only while the signed-in account holds the Relay entitlement (ADR 0119
"Relay entitlement"). Without it, each Relay row shows "Needs Pro" with an
upgrade hint and offers no Connect; SSH stays free. A row named by the pinned
host identity shows it as verified in Details. An unpinned host is
`relay:<relay id>` and its Details reads "Not verified yet". The listing's
`machineDisplayName` names the row, and the newest worktree `updatedAt` (the
host heartbeat, every 15 s) is when the host was last seen. Connected over the
Relay means only that the Relay answers. When the host has not been seen for
more than two minutes, the row, Details and picker say "Host offline, last
seen …", and Connect and Details say that requests wait for the host for up to
15 minutes (as they do while Connecting or Reconnecting). Commands still in the
outbox are shown as "outcome pending" until `commandCompleted`. Connections
refreshes the listing every minute while it is shown. Relay pairings are not
removed from Desktop; Remove is offered only for SSH machines.

The activity bar's machine picker labels each machine with the same route and
state as its row (`build-node-02 · SSH · Connected`). The window's
`MachineSelection` registers each listed id's backend from
`LinuxMachinesSection::machineBackend(id)` on `machinesChanged`, so choosing a
machine binds the Desktop composition to it.

### Backend tests

`ketplus_remote_ssh_backend_tests` drives the backend with a fake link and a
fake daemon. It covers the state mapping, an Offline request, the first hello,
restart persistence without secrets, a dropped session that replays a read,
rotation fail-closed and re-trust with binding moves, a daemon that names no
identity, durable add and remove, accepted host keys removed with their machine
and forgotten for re-trust, an unreadable store, and destination parsing.
`ketplus_remote_known_hosts_tests` covers `removeKnownHost()` and
`hasKnownHost()`. `ketplus_linux_ssh_machines_tests` (offscreen) drives
Connections' SSH route through the same fake link and daemon
(`tests/daemon/FakeSshLink.h`). `ketplus_linux_relay_machines_tests` (offscreen) drives the Relay route
with fake links: live state, the entitlement gate, fingerprint keying with SSH
preferred, host presence, pending outcomes and the Pro hint.
`ketplus_remote_ssh_daemon_integration_tests` runs a real
`ketplus_daemon` with FakeCodex (no provider quota) behind the loopback `sshd`
fixture (`tests/daemon/SshFixture.h`). It lists repositories and
conversations, completes an approval round-trip, pages a transcript,
reconnects after a Desktop restart as the same machine, and reports a host
without the daemon. Like the tunnel tests, it skips where `sshd` cannot run as
the user.
