# Release process

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

- `integration` receives every feature and fix pull request and has no CI.
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

### Two-architecture Pro macOS release

Produce separate arm64 and x86_64 DMGs, never combine a single-architecture Node
runtime into a universal app. Each architecture has its own worktree-owned build
and staging directory. Use a Qt SDK and OpenSSL with the requested architecture;
Apple Silicon Homebrew libraries alone cannot satisfy an Intel link.

```sh
cmake -S . -B "$BUILD_ROOT/arm64" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DKETPLUS_EDITION=pro -DKETPLUS_BUILD_TESTS=OFF \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DKETPLUS_BUNDLE_BROWSER_RUNTIME=ON \
  -DKETPLUS_CONTROL_URL=https://ketplus.ketviet.vn
nice -n 10 cmake --build "$BUILD_ROOT/arm64" --target package --parallel 3
```

Repeat with a separate `x86_64` build directory and architecture-compatible SDKs.
Before each local build, count exact `ninja` and `xcodebuild` processes; start only
below three and with at least 15 GiB free. Recheck disk between stages and remove
only this task's intermediate artifacts. Verify `lipo -archs` on the app, daemon
and bundled browser Node, and run each daemon's `--build-info`: `control_url` must
be the intended production URL above. Do not launch an app until native settings
isolation is proved as described in testing.md.

The `pro-macos-release` CI matrix builds unsigned artifacts on native `macos-15`
(arm64) and `macos-15-intel` (x86_64). Native Intel avoids cross-linking arm64
Homebrew OpenSSL; macOS 13 is retired. Both use Qt 6.11.1 and explicit CMake
architectures, with the browser runtime enabled. CI produces per-arch unsigned
handoff artifacts only, never signed distributions or published feeds. The runner
labels are documented in the [official runner image table](https://github.com/actions/runner-images/blob/main/README.md).

Signing is an owner-controlled local stage. All input/output paths must be absolute;
the signing script refuses relative paths before creating output. Select an existing
Developer ID Application identity for team `J9MRAQR8W3`. If the notary profile or
credentials are absent, stop and ask the release owner; never create or enter Apple
credentials. After explicit owner authorization, the absolute-path command is:

```sh
packaging/macos/sign-notarize-dmg.sh --app "$ABSOLUTE_STAGED_APP" \
  --output "$ABSOLUTE_SIGNED_APP" \
  --identity "Developer ID Application: <owner> (J9MRAQR8W3)" \
  --notary-profile "$EXISTING_NOTARY_PROFILE"
```

For each final app/DMG, verify `codesign -dv` reports the intended TeamIdentifier,
`codesign --verify --deep --strict`, `spctl -a -vv` and `xcrun stapler validate`.
Launch only disposable QA copies with an IniFormat hook or a separate OS user;
verify Intel on Rosetta or Intel hardware, or explicitly record the limitation.
Never replace the installed app or write its real preference domain.

Updates use separate `<channel>/arm64/appcast.xml` and
`<channel>/x86_64/appcast.xml` directories and matching download prefixes. Test both
with a throwaway QA EdDSA key and localhost feeds, including wrong-architecture
refusal, before any production-key operation. Keep QA keys outside the checkout,
never publish QA artifacts, and never upload a release or feed without the owner's
separate go-ahead. A successful local build is not signing, notarization or release
approval; record those gates independently.

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
Existing feeds must carry matching `urn:ketplus:release` channel and architecture
metadata and scoped archive URLs. Legacy unscoped feeds require a clean scoped
output directory and an explicit release migration; do not mix their archives.
Identity metadata is created only for a new feed before Sparkle generation.
Existing appcasts are validated without rewriting any bytes, preserving namespace
prefixes, declarations and signatures. Legacy feeds without identity are refused. Exported signing-key paths are resolved through
all symlinks before a filesystem-identity ancestry check (including case aliases) and before being passed to Sparkle.
The app pins the compiled feed through its delegate: restoring an old `SUFeedURL`
preference cannot switch the build's channel or redirect it to HTTP.

For local update QA, follow the native-settings isolation procedure in
[testing](testing.md#desktop-update-qa-isolation). Use only a throwaway key,
a loopback feed and a disposable complete bundle. Never replace or relaunch the
normal installed application. One manual update check on the real release build
remains an owner-operated pre-release check.

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
packaging/macos/sign-notarize-dmg.sh --dmg "$ABSOLUTE_UNSIGNED_DMG" \
    --output "$ABSOLUTE_SIGNED_DMG" \
    --identity "Developer ID Application: <team name> (J9MRAQR8W3)" \
    --notary-profile "$EXISTING_NOTARY_PROFILE"
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
repository, the script's arguments, CMake arguments or logs. Hosting the update
feed and rollback remain production release work. Each archive is published under a new
`PROJECT_VERSION`; the EdDSA private key stays in the release Keychain or secret
store and never enters this repository, CI logs, or CMake arguments.
