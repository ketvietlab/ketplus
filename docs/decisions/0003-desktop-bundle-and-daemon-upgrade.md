# ADR 0003: Desktop bundle and daemon upgrade ownership

Status: accepted

Date: 2026-09-11

## Context

KetPlus is downloaded directly from KetSuite. Requiring a second daemon
installer creates a second update channel and makes it possible for Desktop and
daemon versions to drift. Replacing a running daemon during an application
upgrade can also terminate a live provider turn.

Codex CLI is currently an external runtime integration. Bundling it would add a
separate redistribution, update, notice, and provider-authentication decision.

## Decision

- Distribute one KetPlus product. On macOS, `ketplus_daemon` is a nested helper
  in `KetPlus.app/Contents/Helpers`; other platforms install it beside the
  Desktop executable.
- Run the helper as the signed-in user. This slice does not install a privileged
  system daemon or register a login-time service.
- Keep journals and local credentials in the user's application-data directory,
  outside the replaceable application bundle.
- Build Desktop and daemon from one product version. `daemon.hello` advertises
  that version in addition to the independently versioned local protocol.
- When a newer Desktop finds an older protocol-compatible daemon, it requests
  `daemon.prepare_upgrade`. The daemon exits only if it has no starting,
  running, waiting-for-input, or stopping session. Desktop reconnect then starts
  the helper embedded in the newly opened application.
- If a turn is active, replacement fails closed with `daemon_busy`; the new
  Desktop keeps using the compatible old daemon. A later application launch
  retries the rollover. An older Desktop never requests a downgrade.
- Do not copy, install, or download Codex CLI as part of KetPlus packaging.
- Do not copy, install, or download Cursor CLI (`agent`) as part of KetPlus
  packaging. ACP clients spawn a user-installed binary.
- For the direct KetSuite distribution, the user opens the DMG and replaces the
  single application in `/Applications`. Installation itself does not stop or
  replace the running per-user daemon; opening the new app activates the safe
  rollover handshake.

## Consequences

- Installation is one drag/copy operation and updates replace one app bundle.
- Opening the new version is the activation point for an idle helper upgrade;
  merely downloading a DMG does not affect a running session.
- Product releases must bump `PROJECT_VERSION`; otherwise Desktop cannot detect
  a changed helper build.
- Protocol compatibility remains a separate release gate. A future incompatible
  protocol needs an explicit migration rather than forced process termination.
- Remote availability immediately after login, before Desktop is opened, needs
  a later user-visible Service Management decision.
- Production distribution still requires nested-code signing, notarization,
  update metadata, rollback policy, and complete notices.
- ADR 0019 adds in-app updates through Sparkle. Sparkle relaunches only the
  Desktop; this handshake is still what replaces the daemon.
