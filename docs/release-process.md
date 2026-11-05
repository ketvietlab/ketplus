# Release process

## Branch policy

Three long-lived branches carry every change (ADR 0030):

```text
feature/fix -> test what you changed -> pull request into integration -> merge
                                                                          |
                                         pull request integration -> develop
                                         (promotion provenance + full build)
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
`build` jobs, including for administrators; `main` keeps its release rules.

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
git tag -a pro-v0.1.51 -m "KétPlus Pro 0.1.51"
git push origin main pro-v0.1.51
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
repository, the script's arguments, CMake arguments or logs. Hosting the update
feed and rollback remain production release work. Each archive is published under a new
`PROJECT_VERSION`; the EdDSA private key stays in the release Keychain or secret
store and never enters this repository, CI logs, or CMake arguments.
