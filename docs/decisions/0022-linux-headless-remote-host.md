# 0022: Linux headless remote host

Status: accepted
Date: 2026-10-08

## Context

A server without a Desktop or display must own agent processes and repositories,
while a Desktop on another machine and a phone send remote commands. The existing
local daemon and encrypted Control v2 relay already own these responsibilities,
but enrollment assumes a local browser and workspace discovery assumes Desktop
has opened a repository.

## Decision

- Build the daemon independently with `KETPLUS_BUILD_DESKTOP=OFF`; keep it private
  in `ketplus-desktop`. Public CM remains immutable and has no remote-control code.
- Use the existing outbound HTTPS/WebSocket Control v2 relay. A Unix socket is
  strictly local administration; no daemon TCP listener is introduced.
- Add an installation- and PKCE-bound, ten-minute device authorization grant.
  Browser approval requires the authenticated viewer and same-origin CSRF consent.
  Control stores a digest of the polling secret and consumes the grant atomically.
- A dedicated systemd user service owns the state, provider login and owner-only
  credentials. SIGTERM/SIGINT shut down through the Qt event loop.
- Add local authenticated workspace registration that persists with zero sessions.
  Phone or future Desktop clients can create the first conversation using opaque
  remote catalog identity, without seeing a plaintext host path.
- Allow v2 device identities for iOS, Android, macOS, Windows and Linux. Grant
  eligibility remains account-, installation-, key- and host-approval-bound.
- Prepare a private Qt remote-client backend. Desktop UI/composition is deferred
  by user scope. Existing iOS Control v2 is the phone client in this implementation;
  Android's v1 preview requires a separate v2 application migration.
- Install/archive a complete versioned runtime, with a relative Qt library path,
  SQLite plugin, sidecar, skills, notices and a user service unit. Node/provider
  CLIs and the optional browser runtime remain explicit external dependencies.

## Consequences

The same relay and E2EE contracts serve local macOS and remote Linux hosts. There
is no direct network filesystem or process access from a client. Account login
alone does not grant session keys; pairing is still approved on the host. Release
builds bind the intended relay at configure time. Local tests cannot stand in for
production relay deployment or physical-device acceptance. See
[`linux-headless-daemon.md`](../linux-headless-daemon.md) for the operational flow
and current Desktop-backend coverage.
