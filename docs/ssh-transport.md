# SSH transport

Status: transport primitive implemented (`ketplus_remote_ssh`). It is not yet
composed into Desktop: no UI adds a host, and `DaemonClient` does not use it yet.

Desktop can reach the KetPlus daemon of a Linux host that it can already reach
with OpenSSH. The relay ([`remote-transport.md`](remote-transport.md)) needs
pairing and an account; this route needs only the user's own SSH access, and
the relay sees nothing.

## Route

`ketplus::remote::ssh::SshTunnel` drives the system `ssh` client. It never
implements SSH itself, so `~/.ssh/config` (Host aliases, ProxyJump, identity
files) and the user's agent (`SSH_AUTH_SOCK`) apply as in a terminal. Every call
adds `-n -T`, `BatchMode=yes`, `PasswordAuthentication=no`,
`KbdInteractiveAuthentication=no`, `StrictHostKeyChecking=yes`,
`UpdateHostKeys=no`, `CheckHostIP=no` and a connect timeout, so ssh never
prompts and never edits a known_hosts file.

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
   as ssh does (`@revoked` and `@cert-authority` lines never verify a key).
   - **Verified**: a known key of the offered type matches.
   - **Mismatch**: a known key of the offered type differs, or only other types
     are known and a second probe restricted to those types (`HostKeyAlgorithms`)
     does not match. The tunnel stops in `HostKeyMismatch` and exposes the
     offending fingerprint. It never retries and never asks.
   - **Unknown**: nothing is known for the name. The host-key prompt callback
     receives the key; accepting appends it to the KetPlus known_hosts file
     (`<GenericDataLocation>/Ketsuite/KetPlus/ssh/known_hosts`, file 0600,
     directory 0700). Refusing, or having no callback, ends in
     `HostKeyRejected`. The user's own known_hosts files are never written.
4. **Locate the daemon socket.** `ssh <alias> "exec /bin/sh -c '...'"` runs a
   POSIX sh program that resolves the socket as the user unit does
   ([`linux-headless-daemon.md`](linux-headless-daemon.md)):
   `$KETPLUS_DAEMON_SOCKET`, then the last `KETPLUS_DAEMON_SOCKET=` line of
   `~/.config/ketplus/daemon.env`, then `$XDG_RUNTIME_DIR` or `/run/user/$(id -u)`,
   then `$KETPLUS_DAEMON_DATA_DIR` or `$XDG_DATA_HOME/Ketsuite/KetPlus`, each
   followed by `/ketplus-daemon-v0`. It prints one line,
   `ketplus-socket <present|missing> <path>`. A path with whitespace or `:` is
   refused.
5. **Forward.** `ssh -N -o ExitOnForwardFailure=yes -o StreamLocalBindUnlink=yes
   -o StreamLocalBindMask=0177 -L <local>:<remote>` forwards a socket in the
   private temporary directory to the daemon socket. `device()` returns the
   connected `QLocalSocket`.

Steps 4 and 5 pass the user files and the KetPlus file as `UserKnownHostsFile`
and restrict `HostKeyAlgorithms` to the verified key type, so ssh checks the key
again; a key that changes after the probe fails closed in ssh itself.

## States

| State | Meaning | Next |
| --- | --- | --- |
| `Connecting` | Resolving, probing, locating or forwarding. | |
| `Connected` | `device()` is the daemon connection. | |
| `Offline` | The host, the forward or the daemon connection failed or closed. | Reconnects. |
| `DaemonSocketMissing` | The host is reachable but no daemon listens at the resolved socket. | Reconnects. |
| `AuthFailed` | ssh reported `Permission denied`. | Waits for `open()`. |
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
  fingerprint. ssh's stderr is only classified, never logged or shown.
- The local forward socket lives in a 0700 temporary directory and is bound
  owner-only.

## Tests

`ketplus_remote_ssh_tunnel_tests` starts a non-root loopback `sshd` on
`127.0.0.1` with throwaway host and user keys and a client config passed with
`-F` (`IdentityAgent none`, `IdentitiesOnly yes`, fixture known_hosts, no
`~/.ssh` file). `sshd` sets `KETPLUS_DAEMON_SOCKET` for the session and a
`QLocalServer` plays the daemon. The cases cover acceptance of an unknown key
(recorded and reused without asking), rejection, a changed key (no prompt, no
reconnect, no daemon connection), a refused account, a stopped daemon that
later starts, and reconnection after the daemon drops the connection. Parser
and socket-resolution cases run without sshd. Where `/usr/sbin/sshd` is missing
or cannot run as the user, the sshd cases skip.
