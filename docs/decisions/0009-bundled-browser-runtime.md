# 0009: Bundled browser runtime

Status: accepted
Date: 2026-09-15

## Context

Stage 0 (`0008-codex-browser-tools-stage-0.md`) launches `apps/browser` from a
source checkout with a user-installed Node, `npm ci` and a Playwright Chromium
download. Packaged KetPlus users have none of these. Browser tools can only be
default-on if the product carries everything needed to start the browser.v0
server, without a network round-trip before the first session when a Chromium
browser is already installed.

## Decision

**Bundle the runtime in the app.** `KetPlus.app/Contents/Resources/ketplus-browser/`
holds the tree below. It is not in `Contents/Helpers`: codesign treats every file
under Helpers as nested code, so signing the app fails on the first JavaScript or
license file there.

```text
bin/node                         Node.js v24.21.0 LTS (arm64), unmodified nodejs.org binary
src/                             browser.v0 server (mcp.mjs, cli.mjs, engine.mjs, ...)
node_modules/                    production dependencies only, pruned
package.json, package-lock.json
.claude/skills/ketplus-browser/  the skill shipped with its tool
THIRD_PARTY_NOTICES.md, licenses/
runtime.json                     {name, version, nodeVersion, node, protocols:["browser.v0"],
                                  entrypoints:{mcp, jsonrpc}, skill, playwrightCore, packageLockSha256}
```

- **Node binary plus app directory**, not a single-executable application. SEA
  would need Playwright's on-disk assets (injected scripts, `webp_codec.wasm`,
  `xdg-open`, the installer child process) to be extracted or virtualised and
  would still need a separate binary for `engine-installer.mjs`. Shipping `node`
  unmodified also keeps Node's upstream signature semantics simple.
- **`playwright-core` instead of `playwright`** at runtime. `playwright` only adds
  the test runner and CLI; it is now a dev dependency (source checkouts still get
  `node_modules/playwright` and `npm run install:browser`). Neither package
  contains browsers.
- **Reproducible staging.** `apps/browser/scripts/package-runtime.mjs` takes the
  archive for the pinned version from a local cache or nodejs.org and refuses it
  unless its sha256 matches `apps/browser/runtime/node-runtime.json` (copied from
  that release's `SHASUMS256.txt`). It runs `npm ci --omit=dev --ignore-scripts`
  with the npm inside that archive, prunes types, maps, docs and Playwright's
  trace-viewer UI, writes notices and `runtime.json`, and smoke-imports the server
  with the staged node before replacing the output directory.
- **Build integration.** `apps/desktop` target `ketplus_bundle_browser_runtime`,
  option `KETPLUS_BUNDLE_BROWSER_RUNTIME` (default ON for macOS Release, OFF
  otherwise). `KETPLUS_BROWSER_RUNTIME_DIR` bundles a prebuilt staged runtime
  without network; `KETPLUS_NODE_CACHE` supplies the Node archive. Staging needs a
  build-host node >= 22 only to run the script.
- **Browser selection, no Chromium in the bundle.** At the first session the
  server picks, in order: installed Google Chrome, installed Microsoft Edge, a
  KetPlus-managed Chrome for Testing in `KETPLUS_BROWSER_CACHE` (default
  `$GenericDataLocation/Ketsuite/KetPlus/browser-runtime/chrome`). Chrome for
  Testing is downloaded only when neither system browser exists, through
  Playwright's registry (its pinned revision, retries and install lock) in a
  child process. Playwright 1.63 does not verify a checksum or signature for
  browser archives, so integrity relies on HTTPS from Playwright's CDN: the
  installer child runs without any `PLAYWRIGHT_*` variable (including
  `PLAYWRIGHT_DOWNLOAD_HOST` and `PLAYWRIGHT_CHROMIUM_DOWNLOAD_HOST`),
  `NODE_OPTIONS`, `NODE_PATH` or `NODE_TLS_REJECT_UNAUTHORIZED`, so an inherited
  environment cannot redirect the host or disable TLS verification. Standard
  proxy variables are kept (the proxy must still present the CDN certificate).
  An install is used only when Playwright's `INSTALLATION_COMPLETE` marker is
  present; a directory without it (interrupted download or extraction) is
  deleted and downloaded again. The cache directory is created `0700`. `KETPLUS_BROWSER_ENGINE=auto|chrome|msedge|bundled`
  forces a choice (`playwright` selects the Playwright cache for development);
  `KETPLUS_BROWSER_DOWNLOAD=0` forbids the download. Failure is
  `BROWSER_UNAVAILABLE` with a message naming the missing browser or the failed
  download.
- **Isolation.** Every engine is launched by Playwright with a fresh temporary
  user data directory and ephemeral contexts behind the origin proxy. The user's
  own Chrome or Edge profile, cookies and extensions are never used.
- **Evidence.** `browser.capabilities.engine` reports `preference`, `name`,
  `source` and `browserVersion`; session environments carry `browserEngine` and
  `browserVersion`, and `visual.compare` blocks when either differs, so a system
  Chrome capture is never compared with a Chrome for Testing capture.
- **Discovery.** `modules/browser/src/BundledBrowserRuntime.{h,cpp}` validates
  `runtime.json` (entrypoints must stay inside the runtime directory, node must be
  executable) from `Contents/Resources/ketplus-browser`, then `Contents/Helpers/ketplus-browser`,
  then a sibling `ketplus-browser`, from Desktop (`Contents/MacOS`) or the daemon
  (`Contents/Helpers`), and
  `BundledRuntimeProvider` resolves `ketplus.browser` + `mcp-stdio` to
  `node src/mcp.mjs` with `KETPLUS_BROWSER_CACHE`. `resolveBrowserLaunch`
  registers it before the source checkout provider; settings are owned by the
  default-on browser tools work (ADR 0008).
- **Updates.** The runtime is versioned and updated with the app; there is no
  separate updater. System Chrome and Edge update themselves; Chrome for Testing
  follows the Playwright revision pinned in the bundled `playwright-core` and is
  refreshed when an app update moves that pin.

## Size

darwin-arm64, Node v24.21.0, playwright-core 1.63.0: about 145 MB uncompressed
(`bin/node` 128 MB, `node_modules` 16 MB) and about 43 MB zipped. A Chrome for
Testing download adds roughly 375 MB to the user's data directory only when no
system browser is installed.

## Licensing

Node.js (MIT, with its bundled third-party licenses), playwright-core
(Apache-2.0, `NOTICE` and `ThirdPartyNotices.txt` retained), the MCP server SDK
and core (MIT), pixelmatch (ISC), pngjs (MIT) and Zod (MIT) are redistributed;
the staging script copies every production package's license and notice files
into `licenses/` and lists them in `THIRD_PARTY_NOTICES.md`. Chrome, Edge and
Chrome for Testing are not redistributed. See `docs/licensing.md`.

## Signing and notarization

- `bin/node` is a Mach-O executable and must be signed with the app's Developer
  ID, hardened runtime and a secure timestamp before the app bundle is signed
  (inside-out; do not rely on `--deep` for release). V8 needs
  `packaging/macos/ketplus-browser-node.entitlements`
  (`com.apple.security.cs.allow-jit`,
  `com.apple.security.cs.allow-unsigned-executable-memory`), which match Node's
  own notarized builds. Those entitlements apply to
  `Contents/Resources/ketplus-browser/bin/node` only, never to KetPlus or
  `ketplus_daemon`. CMake only copies the runtime and does not sign anything:
  the release signing pipeline must sign `bin/node` with these entitlements
  (`codesign --options runtime --timestamp --entitlements
  packaging/macos/ketplus-browser-node.entitlements`) before signing the app,
  or the notarized node cannot JIT and browser tools fail to start.
- `BundledRuntimeProvider` launches node with `NODE_OPTIONS` set to empty (and
  the `PLAYWRIGHT_*` download host variables empty), because Codex passes the
  launch env to the MCP server on top of the inherited environment and the
  launch map can only add or override variables.
- No other Mach-O files ship (no native addons; `webp_codec.wasm` and
  `xdg-open` are data or scripts). Signing seals the JavaScript as bundle
  resources, so the runtime directory must not be modified after signing.
- Chrome for Testing is downloaded by the runtime without a quarantine
  attribute into the user's data directory and is not covered by the app's
  notarization. System Chrome and Edge are signed by their vendors.
- Local check: ad-hoc signing inside-out (`bin/node` with the entitlements and
  `--options runtime`, then `ketplus_daemon`, then the app, all without `--deep`)
  passes `codesign --verify --deep --strict`, and the signed node still serves
  MCP and captures. `codesign --deep --force` on the app strips the helper
  entitlements. Notarization is not exercised here.

## Consequences

- The app grows by about 145 MB (43 MB compressed in the DMG).
- Packaged builds can offer browser tools without Node, npm, a checkout or a
  browser download when Chrome or Edge is installed.
- Rendering depends on the user's browser build; comparisons record and match
  it, and a pinned engine (`KETPLUS_BROWSER_ENGINE=bundled`) gives repeatable
  evidence across machines.
- Node and Playwright security updates require an app release; the pin file,
  lockfile and notices must be updated together.

## Alternatives considered

- **Node SEA:** rejected for now (asset extraction, installer child process).
- **Bundle Chromium or Chrome for Testing:** adds about 375 MB to every install
  and a Chromium notice/source obligation for a browser most users already have.
- **Downloadable QAPack package (manifest, signed archives):** still possible as
  another `ToolPackageProvider`; not needed while the runtime ships with the app.
- **Use the user's Chrome profile:** rejected; agents must not inherit personal
  cookies, extensions or sessions.
