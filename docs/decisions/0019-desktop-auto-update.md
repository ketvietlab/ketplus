# 0019: Desktop in-app updates through Sparkle

Status: accepted
Date: 2026-10-04

## Context

KetPlus Desktop is distributed as one `KetPlus.app` that embeds the per-user
daemon (ADR 0003). Users update by downloading a DMG and replacing the app by
hand, so many keep running old builds and the daemon rollover only happens when
they remember to. Agent turns run in the daemon for minutes or hours, so an
update must never interrupt them, and an update channel is a code-execution
path that must not trust the download server or the network.

## Decision

- macOS Desktop uses [Sparkle 2](https://sparkle-project.org) (2.10.0, MIT),
  downloaded at configure time from the pinned GitHub release archive and
  checked against its SHA-256. `KETPLUS_SPARKLE_ARCHIVE` points at a local copy
  for offline builds. No Sparkle source is vendored.
- The appcast URL and the EdDSA public key are build settings:
  `KETPLUS_UPDATE_FEED_URL` (HTTPS, no query) and
  `KETPLUS_UPDATE_PUBLIC_ED_KEY`. They are written to `Info.plist` as
  `SUFeedURL` and `SUPublicEDKey`, together with
  `SUVerifyUpdateBeforeExtraction`. Both must be set together; when both are
  empty (the default) Sparkle is neither downloaded nor bundled and the app
  never contacts an update server.
- Every archive is verified against the compiled-in EdDSA key before it is
  extracted. The private key is never stored in this repository and never
  passed to CMake. `packaging/macos/publish-update.sh` signs archives with the
  Keychain key from Sparkle's `generate_keys`, or a key file or standard input
  supplied from outside the repository, and refuses a key inside the checkout.
  It prepares the appcast but does not upload it.
- `AppUpdater` is the process-wide interface (`apps/desktop/src/update`). The
  Sparkle implementation wraps `SPUStandardUpdaterController`. Unconfigured
  builds and other platforms use a disabled implementation, and tests install a
  fake.
- Sparkle's standard windows show release notes, download progress and the
  install prompt. KetPlus adds Settings → Updates (version, last check, Check
  for Updates, automatic checks, background downloads), a "Check for Updates…"
  item in the application menu and the sidebar user menu, and a sidebar
  reminder.
- Automatic checks are on by default (`SUEnableAutomaticChecks`), so Sparkle
  never asks for permission on the second launch. Background downloads are off
  until the user turns them on. KetPlus always asks before relaunching.
- Scheduled checks use Sparkle's gentle reminders: if KetPlus is not in focus,
  Sparkle does not raise a modal alert. The sidebar shows "Update available"
  until the user opens it or Sparkle ends the update session.
- Installing an update relaunches only the Desktop. The running daemon keeps
  serving its turns. The new Desktop then performs the ADR 0003
  `prepare_upgrade` handshake: an idle daemon is replaced, and a busy daemon
  keeps running until a later launch.

## Consequences

- Release builds that should update must be configured with the feed and public
  key. Rotating the key requires an update that ships the new public key, signed
  with the old one.
- Production updates still require Developer ID signing and notarization of the
  app and the embedded Sparkle helpers. EdDSA verification is required either
  way. `publish-update.sh` warns when the app is not Developer ID signed.
- Every published version needs a higher `PROJECT_VERSION`, which is both
  `CFBundleVersion` and `CFBundleShortVersionString`.
- Sparkle replaces the bundle while the old daemon may still be running from
  it. A daemon that later loads a plugin or sidecar file from the bundle path
  gets the new version's file. Versioned daemon runtime directories are the
  follow-up if this causes failures. Until then, a busy daemon is only replaced
  at the next Desktop launch, not on a timer.
- Windows and Linux have no in-app updater. They keep the manual install path.

## Alternatives considered

- A KetPlus-specific updater (download a DMG, verify it, swap the bundle with a
  helper). It would duplicate Sparkle's signed-update verification, safe
  bundle replacement and relaunch, which are the security-sensitive parts.
- Updating through the relay or the daemon. That would tie desktop updates to
  account state and give the relay a code-delivery role. The appcast is a
  separate static feed.
- A feed URL chosen at run time (settings or environment). It would let a local
  setting redirect updates. The build-time URL and key keep a shipped app bound
  to one channel.
