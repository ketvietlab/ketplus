# Release process

Linux Pro engineering packages use the native Docker recipe in
[linux-desktop-packaging.md](linux-desktop-packaging.md). Build AppImage and deb
on each native architecture, verify installed `--build-info`, run the SDK-free
smoke for both formats, and retain SHA-256 checksums and metadata. These unsigned
build-only artifacts must not be published until the Qt/AppImage source and
notice audit and release approval are complete. Debian repository signing is a
separate follow-up; no production signing material belongs in the build image.

## Branch policy

Three long-lived branches carry every change (ADR 0030):

```text
feature/fix -> test what you changed -> pull request into integration -> merge
                                                                          |
                                         pull request integration -> develop
                                         (promotion provenance + full build,
                                          tests on several OS versions)
                                                                          |
                                             release: main fast-forwards to develop
```

- `integration` receives every feature and fix pull request. Only its lightweight
  public-tree guard runs on PRs; native Linux packaging runs through the normal
  promotion/release build workflow, not integration PRs (ADR 0030).
  Before pushing, the author runs the tests for the part they changed (see
  `docs/testing.md`) and records the commands and results in the pull request.
- `develop` is verified. It receives only promotion pull requests whose head is
  `integration` in this repository and points at the integration tip; the
  `promotion provenance` job refuses anything else. Merge a promotion with a
  merge commit, never squash or rebase: the next promotion recognizes the
  previous one by its second parent. A failure on a promotion is fixed with a
  new pull request into `integration`.
- `main` is production and moves only through a release.

Each feature or fix has one pull request, into `integration`. Both editions
release from the same commit history; a change to `core/` and to Pro lands in
one pull request.

A hotfix also goes through `integration`. If an incident forces a direct change
to `develop`, get explicit approval, run the affected tests and merge the same
commit back into `integration` before the next promotion, or the promotion
check refuses it.

Branch protection: `integration` has no required checks but allows no
force-push or deletion; `develop` requires `promotion provenance` and the
`build` workflow's jobs (including every `os-suite` build and test entry listed
in `docs/testing.md`), including for administrators; `main` keeps its release rules.
The `develop` rule is the intended configuration and must be set by a repository
owner; until then (not configured as of 2026-11-09) a failing promotion check
does not block the merge.

Workflow actions use full upstream commit SHAs (ADR 0080), retaining the previous
version/ref as a comment. Updating one requires verifying the replacement commit
in its owning upstream repository. Build, promotion and spike jobs use explicit
read-only content permissions and do not persist checkout credentials. These
settings do not replace hosted promotion/release checks or the dependency audit.

## Versions and tags

Each edition keeps its own version:

| Edition | Version source | Tag |
| --- | --- | --- |
| KétPlus (Lite) | `project(KetPlusCM VERSION ...)` in `core/CMakeLists.txt` | `lite-vMAJOR.MINOR.PATCH` |
| KétPlus Pro | `project(KetPlus VERSION ...)` in `CMakeLists.txt` | `pro-vMAJOR.MINOR.PATCH` |

Tags up to `cm-v0.3.6` are KetPlus CM releases from before the merge (ADR 0029)
and stay as they are. To release:

1. Promote the change into `develop` and require every CI job to pass.
2. Raise the edition's version. A `core/` change shipped in Pro also raises the
   Pro version.
3. Fast-forward `main` to the verified commit.
4. Create and push an annotated tag for each edition released.
5. Verify CI on the production `main` commit.

```sh
git switch main
git merge --ff-only develop
git tag -a pro-v0.1.52 -m "KétPlus Pro 0.1.52"
git push origin main pro-v0.1.52
git switch develop
```

Do not retag, force-push a release branch, or release an unverified `develop`
commit.

## Pro release

Production Pro desktop DMGs and Linux daemon packages use
`https://api.ketplus.app`. The `release` and `daemon-release` presets enable
`KETPLUS_PRODUCTION_RELEASE=ON`; a manual configure must pass that flag explicitly.
It selects the production URL and refuses a conflicting `KETPLUS_CONTROL_URL`.
Bundle verification checks the daemon's `--build-info`, rejects another URL and
rejects a runtime-overridable production daemon. For example:

```sh
cmake --preset release
# Headless Linux package:
cmake --preset daemon-release
```

Developer/QA configurations retain the staging default until the staging domain
cutover is approved. A generic `CMAKE_BUILD_TYPE=Release` without the production
flag remains available for explicitly configured staging and isolated local QA.
Use a fresh build directory and explicitly pass both
`-DKETPLUS_PRODUCTION_RELEASE=OFF` and the intended `-DKETPLUS_CONTROL_URL=...`;
see the [staging/QA DMG commands](development.md#developer-packages).
Turning production mode off in an existing production build does not reset its
cached control URL. Never repurpose that directory for staging/QA; verify the
resulting daemon's `--build-info` against the intended deployment before use.

Windows packages are a ZIP built by `cmake --build --preset daemon-release --target package`
(`release` for the desktop), checked with `packaging/windows/VerifyStagedTree.cmake` after
unzipping; see [development](development.md#windows-package). They are unsigned: until
Authenticode signing exists, sign every `.exe` and `.dll` under `bin\` before the ZIP is
published, and do not publish an unsigned Windows build as a production release. Windows
artifacts are audited for the license notices in `share\licenses\ketplus` like the others.
The Qt notice file lists each bundled third-party component with the license, copyright and
source recorded in Qt's SBOM, and includes the texts of the licenses that SBOM defines itself
(`LicenseRef-*`). Texts of standard SPDX identifiers (MIT, Zlib, BSD, ...) are not in the Qt
installation and are not copied here: supplying them is left to the legal review before a public
Windows release.

A production desktop release additionally requires:

- a clean commit on `main` reached through the approved release flow;
- a successful supported-platform build and test matrix;
- packaging, signing, and notarization;
- for builds that update in place, the configured feed and public key, and an
  appcast prepared with `packaging/macos/publish-update.sh` (ADR 0019);
- the license files and exact third-party notices required by `licensing.md`;
- an About surface that identifies the source revision;
- no tokens, transcripts, or signing material in the repository or packages.

Update preparation validates a filename-safe bundle version and readable HTML/Markdown
notes before creating an archive. Existing archive or notes destinations, including
dangling symlinks, are refused rather than overwritten (ADR 0082). This preflight
does not make artifact generation transactional or safe for concurrent writers.

### Preparing a scoped macOS update

`publish-update.sh` defaults to `--mode release`: strict code-signature verification,
a Developer ID Application authority and a valid stapled notarization ticket on the
app are required before archiving. `--mode local-qa` explicitly permits an ad-hoc
signed local fixture, but still requires a valid signature. Never publish QA output.
Apple account use and notarization require the release owner's approval.

Pass `--channel staging|production` and `--arch arm64|x86_64` explicitly. Use a
separate output directory ending in `<channel>/<arch>` and an HTTPS download prefix
ending in `/<channel>/<arch>/`. Local QA also accepts HTTP on a literal loopback
address (for example `127.0.0.1` or `[::1]`); HTTP elsewhere and HTTP release output
are refused. Channel/architecture scoping still applies to QA. The app's compiled feed must equal that prefix plus
`appcast.xml`, and its main executable must have exactly the selected architecture.
For example, with operator-supplied paths:

```sh
packaging/macos/publish-update.sh --app "$STAGED_APP" \
  --updates-dir "$UPDATE_ROOT/production/arm64" \
  --download-url-prefix https://updates.example.invalid/production/arm64/ \
  --channel production --arch arm64 --sparkle-bin "$SPARKLE_BIN"
```

Substitute the approved distribution origin when configuring and preparing the app.
The command only prepares files; it never uploads or authorizes publishing.
Existing feeds must carry matching `urn:ketplus:release` channel, architecture
and release mode metadata and scoped archive URLs. Legacy feeds without mode or
scope require a clean output directory and an explicit release migration. Keep
QA and release artifacts in separate directories, even when both use HTTPS.
Identity metadata is created only for a new feed before Sparkle generation.
Existing appcasts are validated without rewriting any bytes, preserving namespace
prefixes, declarations and signatures. Exported signing-key paths are resolved through
all symlinks before a filesystem-identity ancestry check (including case aliases) and before being passed to Sparkle.
The app pins the compiled feed through its delegate: restoring an old `SUFeedURL`
preference cannot switch the build's channel or redirect it to HTTP.

For local update QA, follow the native-settings isolation procedure in
[testing](testing.md#desktop-update-qa-isolation). Use only a throwaway key,
a loopback feed and a disposable complete bundle. Never replace or relaunch the
normal installed application. One manual update check on the real release build
remains an owner-operated pre-release check.

### Publishing the macOS update to R2

Cloudflare R2 serves the static feed and update assets. The release owner maps
the approved public HTTPS prefix to the R2 keys `<channel>/<arch>/appcast.xml`,
`<channel>/<arch>/KetPlus-<version>.zip`, and sibling delta and release-note
files. Snapshots are stored at
`<channel>/<arch>/snapshots/<version>/<sha256>.xml`. Keep snapshots and every
object they reference for the rollback window. The public route only needs to
serve the appcast and its assets. Keep the snapshot path out of the public route
when access is controlled by a proxy or Worker.

Use Python 3 and AWS CLI v2 with `s3api put-object` support for `--if-match` and
`--if-none-match`. The owner issues a bucket-scoped R2 S3 token with object read
and write access and provides the expected bucket and S3 endpoint through the
release environment. Set `KETPLUS_R2_BUCKET` and `AWS_ENDPOINT_URL_S3` there. Use
an owner-provisioned `AWS_PROFILE` in the AWS files outside the checkout or
inject `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` from the release secret
store, with `AWS_SESSION_TOKEN` for temporary credentials when applicable.
Keep these values outside the checkout, command arguments and release logs.

Run this plan first with the prepared release directory and the same public
prefix used in the app bundle. The dry run checks local inputs and lists the
keys it would write. It works without AWS configuration and does not contact R2.

```sh
python3 -I packaging/macos/upload-update-r2.py publish \
  --updates-dir "$UPDATE_ROOT/production/arm64" \
  --download-url-prefix https://updates.example.invalid/production/arm64/ \
  --channel production --arch arm64
```

Keep the prepared directory stable during publication and retain the files
referenced by its appcast. The owner reviews the plan, confirms the signing and
notarization checks, destination scope and staging verification, then reruns
the same command with `--execute`. Check the public feed and its archive URLs
after publication. Configure the public cache to honor the appcast's 60-second
TTL (or bypass edge caching for `appcast.xml`), and verify that behavior during
staging. Versioned assets use a one-year immutable cache policy.

Execution checks R2 before uploading. Identical objects are reused and any
content collision under an existing asset key stops the release. The tool uses
conditional writes so a concurrent publisher cannot overwrite an asset or
replace an appcast that changed during the run. It uploads the archives, deltas
and notes first, saves the exact previous live appcast as a snapshot if it is
changing, then writes `appcast.xml` last. Record the printed snapshot key in the
release record. First publication has no previous appcast to snapshot. If an
upload or connection fails, review the live appcast and rerun the plan before
retrying. Unreferenced artifacts left by a failed attempt can be reused if
their contents match.

### Rolling back the macOS update feed

Use a snapshot ID printed during an earlier publish or rollback. Omit the scope
and `snapshots/` from `--snapshot` and pass only `<version>/<sha256>.xml`.

```sh
python3 -I packaging/macos/upload-update-r2.py rollback \
  --snapshot "$SNAPSHOT_ID" \
  --download-url-prefix https://updates.example.invalid/production/arm64/ \
  --channel production --arch arm64
```

After reviewing the scope and snapshot, the owner adds `--execute`. The tool
checks the snapshot's digest, metadata and referenced objects, snapshots the
displaced live feed, then restores the prior bytes. Confirm the public appcast
afterward and purge its edge cache if it still serves the withdrawn feed.
Rollback withdraws the newer version from future feed checks. It does not
reverse updates already downloaded or installed, and the app never downgrades.
For clients already updated, publish a fixed release with a higher version.

### Packaging the macOS app

Packaging is not complete yet. Passing CI is necessary but does not by itself
authorize distribution of an official binary.

The unsigned package layout is available through the CMake `package` target as
a branded drag-to-Applications DMG with a deterministic Finder layout that does
not require Automation permission during packaging. It installs the per-user
daemon inside `KetPlus.app` and deliberately excludes Codex CLI. Replacing the
app preserves journal/token state outside the bundle; on first open, version
negotiation replaces an older daemon only when it has no active turn. Package
creation fails if the helper, Qt/QML runtime, SQLite driver, or nested code
signature is incomplete.

`packaging/macos/sign-notarize-dmg.sh` turns that DMG into one Gatekeeper opens
on other Macs. It signs the app inside-out with a Developer ID Application
identity and the hardened runtime: bundled `node` gets
`ketplus-browser-node.entitlements`, and nested code already signed by another
team's Developer ID keeps that signature. It notarizes and staples the app
itself, then signs, notarizes and staples the DMG. The app needs its own ticket:
Gatekeeper assesses the copy in `/Applications`, and without a stapled ticket
the first launch waits on an online lookup and a scan of every bundled file
(about 6,800), which on macOS 27 left the process at `_dyld_start` long
enough to be reported as not responding.

```bash
packaging/macos/sign-notarize-dmg.sh --dmg build/release/KetPlus-<version>-arm64.dmg \
    --identity "Developer ID Application: <team name> (<team id>)" \
    --notary-profile ketplus-notary
```

The same tool accepts `--app <staged>/KetPlus.app --output <signed>/KetPlus.app`
for a complete app-only replacement. It copies the source, signs nested code in
the same order, requires a Developer ID authority and matching Team ID, and checks
that the app's designated requirement is not a cdhash. `--skip-notarize` does not
weaken signing; it only skips the Apple submission/stapling steps and is limited
to local use. Keep the bundle identifier and signing Team stable across updates
so TCC can recognize the app. Ad-hoc engineering builds must not be offered as
replacements merely because strict signature verification succeeds.

The identity's private key stays in the login Keychain. The person holding the
Apple ID creates the notary profile once per machine with
`xcrun notarytool store-credentials ketplus-notary --apple-id <id> --team-id <team>`
and types an app-specific password at its prompt. Neither secret enters this
repository, the script's arguments, CMake arguments or logs. Each archive is published under a new
`PROJECT_VERSION`; the EdDSA private key stays in the release Keychain or secret
store and never enters this repository, CI logs, or CMake arguments.
