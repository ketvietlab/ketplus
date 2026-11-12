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
   credential and runs nothing on the host. OpenSSH 8.5 or later is required.
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

## States

| State | Meaning | Next |
| --- | --- | --- |
| `Connecting` | Resolving, probing, locating or forwarding. | |
| `Connected` | `device()` is the daemon connection. | |
| `Offline` | The host, the forward or the daemon connection failed or closed. | Reconnects. |
| `DaemonSocketMissing` | The host is reachable but no daemon listens at the resolved socket, or it has no local token. | Reconnects. |
| `DaemonTokenInsecure` | The daemon token is readable by other users. | Waits for `open()`. |
| `AuthFailed` | ssh reported `<user>@<host>: Permission denied (...)` on a line of its own. | Waits for `open()`. |
| `HostKeyMismatch` | The host key differs from a known key. | Waits for `open()`. |
| `HostKeyRejected` | The key was unknown and not accepted. | Waits for `open()`. |

A reconnect repeats the whole route, including host-key verification. Its delay
doubles from `initialBackoffMilliseconds` (1 s) up to
`maximumBackoffMilliseconds` (30 s) and resets after each `Connected`.

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
a permissive or missing token, and reconnection after the daemon drops the
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
  supplied by the link and kept in memory only.
- `DaemonMachineBackend` maps the operations to the local request types.

| Tunnel state | Machine state | Reason |
| --- | --- | --- |
| `Connecting` | Connecting, or Reconnecting after a session | |
| `Connected` | Connecting until the hello, then Connected | |
| `Offline` | Reconnecting after a session; Offline for a host never reached | "The host cannot be reached over SSH." |
| `DaemonSocketMissing` | Offline (the tunnel keeps retrying) | "ketPlus daemon not installed or not running on this host." |
| `AuthFailed` | Offline | "SSH refused the sign-in…" |
| `HostKeyMismatch` | Offline | "The host's SSH key changed…" |
| `HostKeyRejected` | Offline | "The host's SSH key was not trusted." |
| `Idle` after `disconnectFromHost()` | Offline | "Disconnected." |

A request made while Offline fails at once with the reason, and so do the
calls waiting for a hello when the machine becomes Offline.
`connectionDetails()` gives Connections the route (`SSH`), the target, the
machine id, the verified SSH host key fingerprint, the state and the reason.

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

### Backend tests

`ketplus_remote_ssh_backend_tests` drives the backend with a fake link and a
fake daemon. It covers the state mapping, an Offline request, the first hello,
restart persistence without secrets, a dropped session that replays a read,
rotation fail-closed and re-trust with binding moves, a daemon that names no
identity, durable add and remove, an unreadable store, and destination
parsing. `ketplus_remote_ssh_daemon_integration_tests` runs a real
`ketplus_daemon` with FakeCodex (no provider quota) behind the loopback `sshd`
fixture (`tests/daemon/SshFixture.h`). It lists repositories and
conversations, completes an approval round-trip, pages a transcript,
reconnects after a Desktop restart as the same machine, and reports a host
without the daemon. Like the tunnel tests, it skips where `sshd` cannot run as
the user.
