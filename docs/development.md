# Development workflow

## Initial setup

Requirements are CMake 3.24+, Ninja, a C++20 compiler, and Qt 6.11+ with
Core5Compat, Svg, SvgWidgets, Concurrent, Qml, Quick, QuickControls2, WebView,
Widgets, and Test. Python 3 runs deterministic signing/provider fixture tests.
`dev` builds KétPlus Pro; `lite-dev` builds KétPlus, the editor in `core/`
alone (ADR 0029):

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

On macOS, a typical Homebrew environment is:

```sh
brew install cmake ninja qt qt5compat sccache
export QT_ROOT="$(brew --prefix qt)"
```

Use `CMakeUserPresets.json` for machine-specific settings. It is ignored and
must not be committed.

## Build presets

For the independent Linux server target, see
[`linux-headless-daemon.md`](linux-headless-daemon.md). `daemon-dev` and
`daemon-release` configure `KETPLUS_BUILD_DESKTOP=OFF`; their Qt dependency set
excludes Widgets, QML and WebView. Set the release Control URL explicitly before
staging a complete install tree. The product and bundled daemon version is 0.1.50;
replacement bundles advance it so an older installed daemon can upgrade when its turns are idle.

- `dev`: Debug Pro build with integration tests in `build/dev`.
- `lite-dev`, `lite-release`: the Lite edition in `build/lite-dev` and
  `build/lite-release`; the application is under `core/`.
- `KETPLUS_EDITION`: `lite` or `pro`; defaults to `pro` on macOS, `lite` elsewhere.
- `release`: Release build without tests in `build/release`.
- `KETPLUS_BUILD_TESTS`: controls Pro tests.
- `KETPLUS_BUILD_BENCHMARKS`: controls benchmark executables.
- `KETPLUS_USE_COMPILER_CACHE`: inherited from KetPlus CM and enabled by
  default. CMake selects `sccache`, then `ccache`, if installed.

Build only the affected target during iteration:

```sh
cmake --build build/dev --target ketplus_agent_tests
build/dev/ketplus_agent_tests
```

Build or inspect the headless local daemon with:

```sh
cmake --build build/dev --target ketplus_daemon
build/dev/ketplus_daemon --help
```

Desktop normally starts this executable on demand. To connect Desktop and a
local Control API to one already-running daemon, give both processes the same
`KETPLUS_DAEMON_SOCKET`, `KETPLUS_DAEMON_TOKEN_FILE`, and
`KETPLUS_DAEMON_STATE` paths. The Control API only needs the socket and token.

Run the desktop application with:

```sh
open build/dev/KetPlus.app         # macOS
./build/dev/KetPlus                # Linux
build\dev\KetPlus.exe              # Windows
```

For UI-only review of the Connections sign-in and Add-device QR flow, configure
a dedicated Debug tree and then opt in at launch:

```sh
cmake -S . -B build/remote-demo -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DKETPLUS_ENABLE_REMOTE_DEMO=ON \
  -DCMAKE_PREFIX_PATH="$QT_ROOT"
cmake --build build/remote-demo --target ketplus
KETPLUS_REMOTE_DEMO=1 open build/remote-demo/KetPlus.app
```

Both the compile-time option and environment flag are required. Continue in
browser then signs in without opening a browser, and Add generates a local
synthetic QR code. The bypass never stores credentials, enrolls a machine, or calls the
remote service. CMake does not compile it into Release builds.

## Developer packages

KétPlus, KétPlus Terminal and KétPlus Pro embed KÉT Sans 1.1 from the official
`KETSans-1.100.zip` release, verified by SHA-256 in `core/cmake/KetFonts.cmake`, and
KÉT Sans Mono 0.10.4 Medium from `core/assets/fonts` (no public release yet). Both are
SIL OFL 1.1. Configure downloads the archive once; offline builds can set
`-DKETPLUS_KET_SANS_ARCHIVE=/path/to/KETSans-1.100.zip` (the same hash is required).
The fonts are compiled into Qt resources and registered at startup; their licences
ship with the application notices. The Lite editor keeps Inter as its interface
default until #141 and #142 switch the defaults.

The macOS package contains one `KetPlus.app`; its per-user daemon is embedded at
`Contents/Helpers/ketplus_daemon`. Codex CLI and Cursor CLI remain external
integrations and are deliberately not copied into the application.

Build an unsigned local DMG with:

```sh
cmake --preset release
cmake --build --preset release
cmake --build build/release --target package
```

For a macOS app-only build (no DMG), use
`cmake --build --preset release --target ketplus_bundle_browser_runtime` so the app
includes its matching daemon, Claude sidecar and browser/Node runtime. With browser
bundling explicitly disabled, use `ketplus_bundle_daemon` instead. Building only
`ketplus` does not refresh those helpers. Product versions must advance when distributing changed daemon behavior;
equal versions do not trigger the idle-daemon replacement handshake.

Never distribute the app directly from the build tree or copy an individual executable
into an existing deployed bundle. Stage it with `cmake --install <build> --prefix <stage>`
so macdeployqt relocates the Qt dependencies, then replace the entire staged bundle.
Before providing replacement instructions, check `otool -L` on the main executable
for absence of Homebrew dependencies, verify `codesign --verify --deep --strict`,
and compare the bundled daemon's `--build-info` control URL with the intended
deployment (not merely an old build cache). Use a separate build directory and an
explicit `-DKETPLUS_CONTROL_URL=...` for each deployment.

`cmake --install` produces an ad-hoc-signed engineering bundle. Before giving it
as a replacement for the user's installed app, sign a copy with the same Developer
ID Application identity used for previous replacements:

```sh
packaging/macos/sign-notarize-dmg.sh --app <stage>/KetPlus.app \
    --output <signed-stage>/KetPlus.app \
    --identity "Developer ID Application: <team name> (<team id>)" --skip-notarize
```

Check `codesign -dv --verbose=4` for the Developer ID authority and expected Team ID,
and `codesign -d -r-` for a certificate-based designated requirement. A successful
`codesign --verify --deep --strict` alone also accepts ad-hoc signatures: their
designated requirement is tied to a build's cdhash, so macOS privacy grants may no
longer match after updates. Do not replace/reset the user's TCC database, broaden
their permissions, re-sign a running app, or interrupt its child processes.
Moving from an old ad-hoc build can require a new consent decision; previously
launched children keep their old attribution until the user restarts them.
`--skip-notarize` is for a local replacement; public distribution still requires
notarization and stapling as described in `release-process.md`.

Every build stages the Claude sidecar (`package-sidecar.mjs`, which runs
`npm ci`), so configuring needs a build-host `node` and the first build needs
network access; set `KETPLUS_CLAUDE_SIDECAR_DIR` to reuse an already staged
tree offline. There is no option to build without it. Claude sidecar staging
preserves npm's relative symlinks so the bundle remains
relocatable after the temporary install directory is removed. The macOS bundle
layout check rejects broken or absolute links inside the sidecar.

The sidecar pins Claude Agent SDK 0.3.289 in both package metadata and the lockfile.
The model picker uses the SDK's live model catalog, including Sonnet 5.5; changing
only a UI label does not upgrade the provider runtime.

### In-app updates (macOS)

Builds do not check for updates unless they are configured with an appcast and
its EdDSA public key (ADR 0019):

```sh
cmake --preset release \
  -DKETPLUS_UPDATE_FEED_URL=https://<update host>/ketplus/appcast.xml \
  -DKETPLUS_UPDATE_PUBLIC_ED_KEY=<base64 public key from generate_keys>
```

Configuring then downloads the pinned Sparkle release (set
`KETPLUS_SPARKLE_ARCHIVE` to a local copy offline). `cmake --install` embeds
`Sparkle.framework`, and the bundle check fails if the framework or the plist
keys do not match the configuration. Never pass the private key to CMake.
`packaging/macos/publish-update.sh` archives an installed app and regenerates
the appcast with the key from the Keychain or from a file outside the
repository. It does not upload. Without a feed, Settings → Updates says that
the build is not configured.

The direct-download install flow is: open the KetSuite-provided DMG, drag
`KetPlus.app` to its `Applications` shortcut, and replace the existing app when
upgrading. There is no separate daemon or Codex installer. Opening the installed
application negotiates the daemon product version. An idle older daemon exits
and Desktop launches the new bundled helper. An active daemon is retained until
a later launch so replacing the app never terminates a turn. Application state
stays below the user's application-data directory and is not stored in the
replaceable app bundle.

When distributing a replacement build, increment the product version even for a
packaging-only repair. The daemon upgrade handshake accepts only a newer version;
reusing the version can leave the previous daemon running from the renamed backup
bundle. Finish active turns before replacing the app, and retain backup bundles
until no process is using them.

The DMG uses a deterministic Finder layout rather than controlling Finder while
packaging. Its compact retina background, fixed icon positions, hidden chrome,
and drag direction are therefore reproducible in local and headless release
builds without macOS Automation permission.

This target proves layout and runtime dependency deployment only. A package is
not a production artifact until the application, nested helper, and container
have passed the signing, notarization, notice, and release gates.

## Cache stages

Local and CI builds use independent cache stages:

1. Qt SDK, managed by the package manager locally and the Qt action in CI.
2. FetchContent sources in `.cache/fetchcontent`, shared by all build trees.
3. Compiler objects in the selected `sccache` or `ccache` store.
4. Generated and linked output in the preset-specific `build` directory.

Changing a pinned dependency invalidates the source cache key in CI. Changing
source normally invalidates only the affected compiler objects and downstream
links. For a cold build, remove only the stage being investigated.

## Formatting and review

Format changed C++ sources before committing:

```sh
clang-format -i path/to/changed.cpp path/to/changed.h
git diff --check
```

Do not mechanically format vendored files or unrelated source. Check the final
diff for generated data, absolute paths, and credentials.

## External tools

The regular build does not require an authenticated provider call. Codex CLI,
Mermaid CLI, and KetAtlas are runtime integrations. Their installation and
opt-in checks are documented in `agent-workspace.md`, the root README, and
`testing.md`.

On macOS the desktop build also builds `KetPlus Computer.app` from
`apps/computer` with the Xcode Swift toolchain (`KETPLUS_BUNDLE_COMPUTER_AGENT`,
default ON). The default ad-hoc signature changes on every build, which resets
its Screen Recording and Accessibility grants. To keep the grants across
rebuilds, configure `-DKETPLUS_COMPUTER_SIGN_IDENTITY=<codesigning SHA-1>`.
Release signing (`sign-notarize-dmg.sh`) re-signs the nested app with the
release identity.

The standalone browser feasibility app is outside the CMake build. It requires
Node.js 22+. It uses system Chrome/Edge or downloads Chrome for Testing on first
use. The pinned Playwright Chromium below is an optional deterministic test engine:

```sh
cd apps/browser
npm install
npm run install:browser
npm test
```

Run `npm start` for its raw newline-delimited JSON-RPC surface or
`npm run start:mcp` for an Agent-facing MCP server. Neither command starts or
changes KetPlus. The host origin ceiling is set with
`KETPLUS_BROWSER_ALLOWED_ORIGINS` and defaults to loopback.

## Keep a compatible running daemon

For a Desktop-only staging replacement, configure
`-DKETPLUS_AUTO_UPGRADE_DAEMON=OFF`. The default is `ON`; `OFF` prevents Desktop
and `ketplusctl` from requesting an idle daemon upgrade while preserving local
authentication and feature negotiation. The complete bundle still includes its
matching helper for startup when no daemon is running.

The existing daemon keeps its own compiled relay URL. This option cannot switch
it to staging; inspect its `--build-info` independently. Activity summaries need
a daemon advertising `transcript.activity_groups/1`; an older daemon continues
with the existing local grouping until a separately scheduled daemon upgrade.

### Native host transport (phase 3)

The daemon build fetches the pinned, data-only libdatachannel dependency from
`cmake/WebRtc.cmake`. Offline builds may provide
`FETCHCONTENT_SOURCE_DIR_KETPLUS_LIBDATACHANNEL` with the complete pinned source
and its three pinned submodules. Dependency build trees stay local to each build.
The daemon advertises `transport.webrtc/1`; it admits offers only after a heartbeat
negotiates that capability. Relay feature flags remain the rollout control.
No provider account or quota is needed for `ketplus_daemon_peer_transport_tests`.

`KETPLUS_CAPTURE` and `KETPLUS_BENCHMARK_STARTUP` runs of Lite use a temporary
INI preference store. They neither restore nor overwrite the installed app's
session/window settings. Captures therefore start from a clean profile; use
`KETPLUS_CAPTURE_THEME=light|dark` to choose the preview theme.
