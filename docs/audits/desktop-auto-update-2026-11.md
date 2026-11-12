# Desktop auto-update audit — November 2026

Audit date: 2026-11-12. Scope: macOS Pro Sparkle, release preparation,
daemon continuity, failure handling, channels, and Linux/Lite boundaries.
This change contains findings only; no product fixes, feed publication or uploads.

The compiled public-code baseline is `d2c6c479`. QA artifacts were built from that public baseline with documentation edits
and external QA hooks; their local build revision is not a retrievable public
commit and is intentionally not used as a source reference. Compared with integration `b5621483033e`, the updater, feed
validation, release-preparation script, bundle verification and daemon upgrade
code inspected here are unchanged. Later branding changes fix the Settings card titles and bundle metadata,
but explanatory Settings copy still uses the old spelling (L1 below).

## Coverage and verification

| Area | Verification | Evidence and limits |
| --- | --- | --- |
| EdDSA before extraction | verified-by-test | Real Sparkle 2.10.0 downloaded unsigned, tampered and wrong-key archives; each failed before unarchiving, retained N. See case table. |
| Dependency pin | verified-by-test | Downloaded SDK SHA-256 matched cmake/Sparkle.cmake:14–16,56–61. |
| Private key custody | verified-by-reading | CMake accepts a public key; signing tools use an external key file or Keychain. Production key storage/history were not inspected. M2 shows the location guard is incomplete. |
| HTTPS feed | verified-by-test | Six CMake fixtures: HTTPS accepted; HTTP, query URL, missing key and bad key rejected; empty pair disables updater. Real QA uses an explicit HTTP loopback-only exception, not production TLS. |
| Downgrade/rollback | verified-by-reading | apps/daemon/src/ipc/LocalIpcServer.cpp:356–367 rejects non-newer daemon replacement. Application downgrade, rollback after failed launch and older signed appcasts were not exercised. |
| Signing/notarization | verified-by-test | Staged bundles and updated bundle passed strict deep codesign. QA is ad-hoc signed, not Developer ID/notarization acceptance. M1 remains. |
| Active daemon turn | verified-by-test | A real N daemon ran the fake provider's hanging turn; it remained alive and returned daemon_busy before and after actual bundle replacement. No provider quota consumed. |
| Idle daemon rollover | verified-by-test | Two offscreen upgrade/client-policy executables passed, including idle allowance and busy deferral. Full real idle process rollover was not separately exercised. |
| Interrupted/corrupt download | partly verified-by-test | Tampered ZIP was refused before extraction. Interrupted/resumed network download was not exercised. |
| Disk-full/read-only/translocation | not verified | Disk guards stayed above 15 GiB; that is not an actual ENOSPC test. Read-only and App Translocation cases remain untested. |
| Staging/production and architecture | verified-by-reading | Operator conventions only; M3/M4. No deployed feed or cross-architecture client was contacted. |
| UI/naming/release notes | verified-by-reading | Integration fixes card titles/bundle names, but explanatory Settings copy still uses KetPlus (L1). UI binding failed; API hook stands in for the click. Real generated release notes/appcast naming needs the owner's manual check. |
| Linux | verified-by-reading | apps/desktop/src/update/AppUpdater.cpp:20–37 supplies DisabledAppUpdater without Sparkle; ADR 0019 leaves Linux manual. AppImage/package-manager installation was not tested; no in-app Linux updater is established here. |
| Lite versus Pro | verified-by-reading | Root CMakeLists.txt:19–33 returns after core for Lite. The audited shell updater is macOS Pro; this does not certify a Lite update channel. |

## Findings

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
filename. End-user acceptance/filter behavior still needs QA. The compiled public key still controls archive trust; the feed itself is not
fully pinned at runtime because of the legacy-defaults behavior in M4.

### M4 — Medium: stale Sparkle preferences override the compiled feed

Reference: apps/desktop/src/update/SparkleAppUpdater.mm:24–28 (no delegate feed
pin or clear-defaults call); pinned Sparkle 2.10.0 SDK `SPUUpdater.h:267–270,
302–309`. Sparkle intentionally prefers an existing SUFeedURL user-default value
over Info.plist. A local Foundation/Sparkle API harness, using its own bundle ID
and isolated home, confirmed that a legacy HTTP loopback preference replaces the
compiled HTTPS localhost feed even with startingUpdater:NO. No network request
or application update was started by this harness.

A user migrating from a testing build or restoring old preferences can remain on
the wrong feed instead of the new release channel. A stale feed/key pairing can
also make updates fail signature validation indefinitely. This does not bypass
the embedded EdDSA key, establish arbitrary remote code execution, or mean that
remote HTTP is accepted by the application's transport policy.

### M5 — Medium: isolated HOME does not isolate native macOS settings

References: apps/desktop/src/main.cpp:35–38 keeps the settings identifiers;
apps/desktop/src/app/RepositoryRouter.cpp:91–97 uses default QSettings.

With HOME and CFFIXED_USER_HOME pointing at a scratch home, a Qt probe reported
a filename under that home but still found an existing workspace/lastRoot key.
The scratch Preferences directory contained no plist. Native CFPreferences/
cfprefsd behavior therefore made a filename-only isolation check insufficient.
Failure scenario: a QA app restores/writes the normal user's recent repositories
or conversation preferences despite following an isolated-home recipe.

The initial launch began at 09:41:18 UTC. Read-only inspection found the real
plist modified at 09:46:39 UTC, with scratch references in these keys:

- `workspace.recentFolders`
- `spotlight.repositories`
- `agent.sessions (one key; suffix withheld)`

No before-launch snapshot existed, so other changes cannot be attributed. The
QA app/daemon were stopped before an updater action. The owner elected to leave
the entries and clear stale recent repositories manually inside the app. This
audit did not edit or clean the real preference domain.

### L1 — Low: explanatory update copy still uses the old product spelling

Reference: apps/desktop/src/app/SettingsUpdatesPage.cpp:103,105,108,120,137
at integration `b5621483033e`. The card titles and bundle metadata use ketPlus Pro,
but descriptions still say KetPlus. A user can see inconsistent product names
on the same Updates page. Verification: source reading; no code fix in this
change. Include these descriptions in the existing branding follow-up and the
release owner's manual naming check.

## QA fixture and instrumentation

N=0.1.52 and N+1=0.1.53 were complete Release Pro Runtime installations,
including the daemon, CLI, computer helper and browser runtime. The daemon's
compiled control URL was loopback-only and non-overridable. A throwaway Ed25519
key was generated outside the checkout; neither production keys nor Keychain
signing entries were used. Every archive case started from a fresh complete N
copy. The QA bundle identifier was `dev.ketplus.audit.t32`; the installed user
application was never replaced, relaunched or targeted by the updater.

Two external, uncommitted hooks distinguish QA binaries/processes from shipping:

1. A bundle-ID-gated startup hook selects QSettings IniFormat under the scratch
   home. A probe exits before UI construction and requires a scratch filename,
   inside=1 and contains_workspace_lastRoot=0.
2. An additional case-path- and bundle-ID-gated hook calls the real SPUUpdater
   background-check API. It writes feed/automatic-check/download preferences only
   to the QA persistent domain, logs callbacks, and invokes Sparkle's official
   immediate-install handler only for the valid fixture. QT_QPA_PLATFORM was
   explicitly cocoa, not offscreen.

These hooks alter settings storage and substitute for the UI trigger/install
choice. They do not replace appcast parsing, EdDSA verification, archive
extraction or bundle replacement. The feed was exclusively HTTP loopback on
127.0.0.1:4443; this is a disclosed QA transport exception to production HTTPS
configuration. No system trust settings or real preference-domain values were
changed by the hooks. No real workspace or agent UI was opened in final QA.
The fake turn was created directly through the daemon's authenticated local IPC.

## Actual update results

Each case has an app stderr log, PID-filtered log-stream capture, loopback
request log, before/after preference digests, and a machine-readable result in
scratch. Public evidence below omits user paths, preference values and signing material.

| Fresh N case | Server evidence (UTC) | Sparkle/result | Isolation |
| --- | --- | --- | --- |
| Tampered ZIP | 13:32:35 appcast and ZIP GET 200 | Outer 4005, underlying EdDSA 3002; explicit validation failure before unarchiving; installed version stayed 0.1.52. | Passed re-evaluation against final QA identifiers. |
| Unsigned ZIP | 13:36:13 appcast and ZIP GET 200 | Outer 4005, underlying 3002; signature missing/refused before unarchiving; stayed 0.1.52. | Passed. |
| Wrong signing key | 13:36:52 appcast and ZIP GET 200 | Outer 4005, underlying EdDSA 3002 before unarchiving; stayed 0.1.52. | Passed. |
| Correct QA key | 13:37:38 appcast and ZIP GET 200 | Downloaded 0.1.53; install-ready/immediate-install callbacks at 13:37:54; full bundle changed to 0.1.53 and strict deep codesign passed. | Passed. |

After the valid update, CFBundleShortVersionString and the installed daemon's
build-info both reported 0.1.53. The original daemon was still alive and its
prepare-upgrade response was still daemon_busy, as before replacement: the fake
running turn survived. An explicit controlled launch of the updated full bundle
subsequently stayed alive through startup and passed the isolation comparison.
Automatic Sparkle relaunch was not independently observed as a new PID; the
verified result is installation after quit plus the controlled N+1 startup.
Real automatic relaunch/UI acceptance remains a manual release-owner check.

## Isolation attribution

The normal app remained in use. An app-free five-minute control changed plist
mtime and one existing session digest, with no key-name changes. Final
comparisons therefore require identical strict workspace/update/Sparkle/SU-key
digests and prohibit churn outside agent.sessions and agent.composerDrafts.
Allowed agent churn must not hash to a QA identifier. Hashing follows UTF-8
SHA-256 in modules/ai/src/agent/AgentMessageFormatter.cpp:167–170 and
modules/ai/src/agent/AgentWorkspaceView.cpp:66–70,
including path:conversation composer scopes.

The final comparison set contains 440 QA identifiers: scratch/workspace/home/
bundle paths and canonical forms, daemon session/thread/turn identifiers from
QA-only journals/results, and the corresponding composer scopes. Every case
passed strict comparisons and had no matching QA hash in the real domain.
The isolated INIs contain workspace/recentFiles and workspace/recentFolders;
only their key names were recorded as positive write-location evidence.

Key churn, attributed to the concurrently active normal app after the control:

| Key | Change and case | QA exclusion |
| --- | --- | --- |
| `agent.composerDrafts (one key; suffix withheld)` | Added during tampered case. | Does not match any of 440 QA identifiers. |
| `agent.sessions (one key; suffix withheld)` | Changed in control and all four cases, plus controlled N+1 startup. | Does not match any of 440 QA identifiers. |

Private local snapshots retain key names, digests and metadata only, without
preference values. Path-derived key suffixes and digest values are deliberately
withheld from this public report: unsalted path hashes are guessable and do not
anonymize a workspace. Public descriptions identify namespaces and counts only. The initial 345-key baseline temporarily reached 346 during the
normal-app draft churn; subsequent cases began with 345. No forbidden namespace
change, strict digest change or QA-identifier match was accepted. Early attempts
with an ineffective defaults-suite setting were excluded from archive-test
acceptance because they did not download the fixture.

## Checks and remaining release work

- Archive-preflight fixtures: 15/15 passed; signing-CLI fixtures: 8/8 passed.
- Native offscreen daemon upgrade/client-policy tests: 2/2 passed (2.68 seconds).
- CMake feed/key policy fixtures: 6/6 passed.
- Public-tree hygiene after rebase: 4/4 passed; git diff --check passed.
- No interrupted-download, actual disk-full, read-only, translocation, production
  TLS, Developer ID/notarization, downgrade or cross-architecture acceptance is
  claimed. Shipping native-settings behavior is not certified by the INI hook.
- Before release, the owner must click Check for Updates once in the real Pro
  build and verify its UI, release notes, channel, naming and automatic relaunch.
- All task-owned app, fake-provider daemon, log-stream and feed-server processes
  were stopped. No feed was published and no artifact uploaded.

## Proposed follow-up tasks (leader to plan)

- M1: require Developer ID and notarized distribution trust in release mode,
  retaining an explicit separate local QA mode.
- M2: resolve the final signing-key target and add a symlink-to-checkout fixture.
- M3: bind channel/architecture identity, output directories and appcast checks.
- M4: pin or migrate effective feed defaults, testing restored staging preferences.
- M5: document mandatory isolated-settings QA and attribution checks; provide an
  explicit isolated backend or require a separate OS user for native-settings QA.
