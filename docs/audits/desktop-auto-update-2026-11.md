# Desktop auto-update audit — November 2026

Status: work in progress; local end-to-end acceptance is not yet established.
Source baseline: public integration commit `d2c6c479`; findings refer to that
compiled/read tree. Later integration changes must be reconciled before review.

Scope: public integration tree, macOS Pro updater, preparation tooling, daemon
handoff, and Lite/Linux boundaries. Audit only: no product fixes or publication.

## Evidence and coverage

| Area | Current verification | Evidence / remaining work |
| --- | --- | --- |
| Archive EdDSA before extraction | verified-by-reading | apps/desktop/CMakeLists.txt:263–268 sets SUVerifyUpdateBeforeExtraction; real tampered/wrong-key refusal pending |
| Pinned Sparkle dependency | verified-by-reading | cmake/Sparkle.cmake:14–16,56–61 pins 2.10.0 and archive SHA-256 |
| Signing key custody | verified-by-reading | CMake accepts public key only; publish-update.sh:65–70 attempts to exclude checkout private-key files; final symlink handling needs attention |
| HTTPS feed configuration | verified-by-test | Six CMake policy fixtures passed: HTTPS accepted; HTTP/query/missing key/bad key refused; empty pair disables updates. Actual network/TLS QA pending |
| Downgrade / rollback | not verified | Daemon rejects non-newer replacement (LocalIpcServer.cpp:356–367); Sparkle application downgrade and rollback tests pending |
| Code signing / notarization | verified-by-reading | publish-update.sh:73–77 verifies signatures but only warns for non-Developer-ID apps; notarization not enforced here |
| Daemon upgrade handshake | verified-by-test | Offscreen daemon upgrade and client-upgrade-policy executables passed 2/2, including active-turn deferral and idle replacement; simultaneous Sparkle replacement with a live turn remains pending |
| Interrupted download / corrupt archive | not verified | Real Sparkle failure-path QA pending |
| Disk full / read-only / translocated app | not verified | Real Sparkle failure-path QA pending |
| Channel separation | verified-by-reading | Build-time immutable feed/key; no channel picker (ADR 0019); release preparation channel binding not enforced |
| Settings / names / notes | verified-by-reading | SettingsUpdatesPage.cpp:102–137 retains old names; Sparkle standard dialogs and generated appcast title need runtime inspection |
| Linux | verified-by-reading | AppUpdater.cpp:20–32,35–37 disables updater; ADR 0019 documents manual installation; no AppImage/package auto-update demonstrated |
| Lite vs Pro | verified-by-reading | CMakeLists.txt:19–31 returns after core for Lite; Sparkle is included later for Pro |
| Archive preparation failure handling | verified-by-test | python3 tests/packaging/test_update_archive_cli.py: 15 passed; synthetic tools, not actual Sparkle |
| Signing CLI failure handling | verified-by-test | python3 tests/packaging/test_signing_cli.py: 8 passed; synthetic tools, not Developer ID acceptance |

## Preliminary findings

### M1 — Medium: release preparation does not require distribution trust

Reference: packaging/macos/publish-update.sh:73–77. A valid ad-hoc-signed app
passes strict signature verification and only emits a warning about Developer ID.
The script does not require a notarization ticket. An operator can prepare a
signed feed/archive for an app that users cannot launch under Gatekeeper.
This is a release-tool gate gap; no claim that EdDSA verification is bypassed.
A synthetic fixture with a successful codesign stub but no Developer ID authority
completed preparation (exit 0) and emitted the warning. This tests the script
gate, not Gatekeeper acceptance.

### M2 — Medium: final key-file symlinks bypass checkout exclusion

Reference: packaging/macos/publish-update.sh:67–70. The parent directory is
canonicalized, but the final filename is not resolved. A key-file symlink outside
the checkout that targets a file inside it passes the location check. The guard
therefore does not establish its stated guarantee that signing material stays
outside the checkout. A synthetic fixture confirmed direct checkout file
rejection (exit 1), but accepted an outside symlink to it (exit 0). The payload
was inert text and the signing tool was stubbed; no real key was read.

### M3 — Medium: channel and architecture are operator conventions

Reference: packaging/macos/publish-update.sh:61–62,79,135–139; ADR 0019.
The embedded feed is read only for output. The updates directory, download prefix
and signing identity are not bound to an explicit staging/production channel or
architecture. A release operator can generate the wrong channel's metadata or
mix separate architecture builds in a directory using the same versioned archive
filename. End-user acceptance/filter behavior still needs QA. Build-time feed/key
immutability does prevent ordinary runtime settings from redirecting the client.

### L1 — Low: update-facing copy has the old product spelling

Reference: apps/desktop/src/app/SettingsUpdatesPage.cpp:102–137 and
apps/desktop/CMakeLists.txt:250. Dialog/card names still use KetPlus/KétPlus Pro
instead of the approved ketPlus/ketPlus Pro. Generated appcast title needs QA.
Identifiers and bundle paths are intentionally outside the rename scope.

## Local QA record

Archive-preflight fixtures: 15/15 passed. Signing CLI fixtures: 8/8 passed.
CMake feed/key policy fixtures: 6/6 passed, with APPLE disabled in a temporary
script to test validation without fetching/building a framework.
The pinned Sparkle 2.10.0 delegate header describes default-channel behavior
(`SPUUpdaterDelegate.h:90–113`); the application delegate adds no channel
selector. This documents behavior, not separation of deployed feeds.
One initial command used a nonexistent test filename; it did not run any tests
and was corrected to test_signing_cli.py above.

No N → N+1 attempt yet. Throwaway QA EdDSA key files were created outside
the checkout with restrictive permissions; no signing material is in this report.
The pinned Sparkle release archive checksum matched the CMake pin. Release
configuration with a localhost HTTPS feed and the QA public key succeeded.
The initial Release app/upgrade-test build passed with four low-priority jobs.
The two offscreen upgrade test executables passed (2/2, 2.68 seconds).
Full Runtime staging stopped before app installation because the separate
`ketplusctl` prerequisite was not built; that target and the incremental N+1
build wait for the next team build slot. No raw build-tree executable was
copied into an installed bundle. No installed application was
launched, replaced or relaunched; no staging/production feed was contacted for
publication, and no artifacts were uploaded. Disk checks remained above the 15 GiB floor.

## Proposed follow-up tasks (leader to plan)

- M1: add an explicit distribution mode that requires Developer ID and notarized
  app trust, retaining a clearly separated local QA mode.
- M2: resolve and validate the final signing-key target before invoking Sparkle;
  add an outside-symlink-to-checkout fixture.
- M3: require explicit channel/architecture release identity, isolated output
  directories and matching appcast generation/acceptance checks.
