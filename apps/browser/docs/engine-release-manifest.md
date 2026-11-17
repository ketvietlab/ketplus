# Route A: immutable managed-engine release inventory

Status: preparation only. D-8 reaffirmed independent publisher identity before
managed downloads/cache reuse. Project enrollment authority is not an approved
substitute; platforms without independent evidence remain not ready. No production
pin has been enrolled. The production installer/launcher is unchanged.
No signing key, online metadata service or TUF implementation is introduced.

## Trust and ownership gates

The proposed inventory ships inside an authenticated application release and is
replaced only with that application. It is not loaded from a browser cache,
environment URL, downloaded sidecar or a tool request. The release maintainer
must first authenticate every initial artifact using the approved authority and
preserve its exact bytes/platform/version binding. An independent security
reviewer checks that evidence before any expected hash is published. Under the
current policy, a local hash from an HTTPS download, matching mirrors, or a project
signature over unexplained bytes does not satisfy initial authentication.

Named artifact enrollers, independent reviewers and release/signing custodians
still need to verify and retain publisher-authentication evidence and establish
the app-release trust chain. A project-signed app or manifest does not itself
authenticate an upstream artifact. Private keys stay outside the repository and
agent workspace. No owner, signer or key is appointed here.

| App-release link inspected | Source-level evidence | Still required before trusting an inventory |
| --- | --- | --- |
| macOS app distribution | `packaging/macos/sign-notarize-dmg.sh` checks strict/deep code validity, Developer ID, the requested Team ID and an Apple-anchored designated requirement | Identify the approved publisher and verify a real release artifact, including its embedded runtime/inventory resource seals. None was inspected in this task |
| macOS update | `packaging/macos/publish-update.sh` release mode requires strict/deep verification, Developer ID and a stapled notarization ticket; ADR 0019 binds Sparkle feed/key settings | Verify the intended release's compiled trust anchors, archive signature, publisher, channel and update path. No published update or production key was verified |
| Linux app distribution | `docs/linux-desktop-packaging.md` explicitly describes unsigned engineering artifacts and an unset production release signing key | Establish an authenticated release channel/package binding before using an embedded inventory as a trust root; unsigned engineering packages do not suffice |
| Windows app distribution | ADR 0019 describes manual installation; no Windows release artifact was examined | Establish the actual publisher-authenticated app/package chain, expected signer and protected installed layout. Manual installation alone is not authentication |
| Browser runtime discovery | `modules/browser/src/BundledBrowserRuntime.cpp` validates manifest fields and contained entry paths | These metadata/path checks do not verify a publisher signature or prove that the chosen runtime comes from the authenticated app |
| Runtime staging | `scripts/package-runtime.mjs` copies `src/`; `scripts/source-digest.mjs` and CMake bind staged source bytes | A matching source digest is consistency evidence, not app-release authentication. An inventory would need to reside in the covered source/resource tree; no production inventory has been added |

These are read-only source observations, not verified release outcomes. See
[`docs/release-process.md`](../../../docs/release-process.md) and
[ADR 0019](../../../docs/decisions/0019-desktop-auto-update.md). Neither a
cache-local `runtime.json` nor a receipt with a release ID can bootstrap trust.
An attacker replacing both an unsigned runtime and its inventory could satisfy
internal hash comparisons; the authenticated app binding is indispensable.

## Prepared inventory and gate

`src/engine-release.mjs` parses bounded immutable JSON with an application release
ID and exact Playwright package version. Expected bindings must come from the
authenticated application; native platform/architecture must come from the host,
not `PLAYWRIGHT_HOST_PLATFORM_OVERRIDE`. Entries bind an identity ID, component,
platform, registry revision, browser version (null for helper artifacts), enrolled
status, archive URL/SHA-256, installed-tree SHA-256, executable path and evidence.
Duplicate scopes/IDs, unknown components, wrong release/host/version, missing pins
and malformed revocation inventories fail closed. A status string or evidence URL
is a record, not proof of authentication.

The integration seam selects the entire declared browser/helper plan before
creating a store or downloading. It checks bundled identity/archive/tree
revocations, verifies each installation through `ManagedEngineStore`, rechecks
the selected trees before the launch callback and rejects path changes. Selection
does not read cache receipts. Tests use synthetic bytes and a fake callback; these
integration cases do not execute a browser/helper executable or shell script.
Separate read-only metadata tests spawn Node subprocesses without launching an
engine.

Production adoption still must load the immutable inventory from its verified app
location, derive a complete plan from the pinned driver and fence every actual
launch/helper path. This includes development-cache selection, explicit channels,
custom executable paths and optional features. The seam cannot detect an implicit
helper omitted by an incomplete caller plan. No production callback or ZIP/network
adapter is supplied, and existing production bypasses remain open under PS-04.

## Flavors, helpers and origins to cover

The source mapping is pinned to Playwright 1.63.0. It covers downloadable artifact
families, not a complete file inventory of unexamined browser ZIPs.

| Component / revision | Origin and selection path | Enrollment requirement |
| --- | --- | --- |
| `chromium` / 1243, CfT 153.0.8010.12 | `cdn.playwright.dev/builds/cft/<version>/<platform>/chrome-*.zip`; the current managed installer selects this executable explicitly | Exact full-browser archive plus complete extracted tree; embedded child executables, DLLs, libraries and resources must be covered |
| `chromium-headless-shell` / 1243, same CfT version | The same CfT namespace with `chrome-headless-shell-*.zip`; CLI installation selects it and default headless launch without an explicit executable may use it | Separate identity; a full-browser pin does not authorize the shell |
| `ffmpeg` / 1011 | Playwright helper CDN paths; CLI `resolveBrowsers` adds it. Driver video code can execute it; the current worker's context options do not enable video | Separate helper identity before any install/use; do not infer authenticity from a browser pin |
| `winldd` / 1007 | Playwright helper CDN paths, Windows x64; CLI adds it and Windows dependency validation invokes it | Separate Windows helper identity; no non-Windows artifact mapping is inferred |

The distribution fixture records both CfT flavors for macOS ARM64/x64, Linux
ARM64/x64 and Windows x64, FFmpeg archive names for those platforms, and Windows
`winldd`. Helper mirror bases are
`https://cdn.playwright.dev/dbazure/download/playwright`,
`https://playwright.download.prss.microsoft.com/dbazure/download/playwright`, and
`https://cdn.playwright.dev`. They are distribution alternatives, not independent
authentication witnesses. Official CfT catalog associations do not prove CDN/
Google ZIP byte equality. All these artifact identities remain missing.

The exact registry's `install([chromium])` installs the supplied target; the CLI's
`resolveBrowsers(["chromium"])` also selects the shell and helpers. These are
different call paths and must not be conflated. A tree pin must cover every entry,
including packaged child helpers, rather than just the primary executable. The
separately packaged Node/JavaScript runtime and system-installed browser trust
are additional app/discovery boundaries, not authorized by a CfT ZIP pin.

## Update, revocation and rollback limits

Route A carries revoked identity IDs, archive SHA-256s and tree SHA-256s in the
next authenticated app release. Its gate rejects those entries even if old cache
bytes remain unchanged or a receipt claims an earlier app's approval. Removing
an identity also leaves no fallback authorization. Archive/tree revocation cannot
be evaded by renaming an identity ID.

An older app does not learn later revocations without an authenticated update.
This preparation does not enforce minimum app versions, prevent installation of
an older signed app, provide trusted monotonic state/time, or promise immediate
revocation. App-signing/update-key compromise and rotation follow the app release
chain; no separate manifest key is created. Owners must resolve these residual
risks and recovery gates before enrollment. No online revocation/update service
is added to hide these gaps.

Focused tests cover both flavors, each declared helper, missing/mismatched scope,
identity/digest revocation, a new release reusing an old cache, and tampering while
a helper installs. Read-only registry fixtures inspect metadata and CLI target
selection. They do not verify native signatures, actual app distribution,
production launch coverage, packaging, deployment or runtime containment.
