# Linux Pro engineering packages

`packaging/linux` builds the Pro desktop as an unsigned AppImage and a Debian
package. Ubuntu 24.04 (glibc 2.39) is the minimum baseline. Builds are native:
amd64 uses an x86_64 host, and arm64 uses an aarch64 host. The recipe never
registers binfmt handlers or changes the host's emulation configuration.
The Lite and headless-daemon recipes remain separate.

These are **engineering artifacts, not approved release downloads**. The
production release signing key remains unset; verification stays fail-closed.
Nothing in this recipe signs, uploads, publishes or enables a host service.
The exact Qt notices/source offer and the AppImage runtime's static dependency
source/relink obligations in `licensing.md` must be completed before distribution.

## Build

Use a clean public checkout including its submodules, a task-owned build directory
outside temporary storage, and Docker on the native Linux host. A one-way Mutagen
source sync may transport the checkout; ignore `.git`, build trees and caches,
and retain a matching Git checkout on the destination for build identity. Sync
the committed source before the final build. Never run from a dirty source tree
when recording release provenance.

```sh
docker build --build-arg KETPLUS_DESKTOP=1 \
  --build-arg 'QT_MODULES=qt5compat qtwebengine qtwebview qtwebsockets qtwebchannel qtpositioning qtshadertools' \
  -f packaging/linux/Dockerfile.build -t ketplus-pro-builder .
mkdir -p "$BUILD_ROOT/home"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/build/home \
  -v "$PWD:/src:ro" -v "$BUILD_ROOT:/build" ketplus-pro-builder \
  bash /src/packaging/linux/build-desktop.sh /src /build/desktop
```

`BUILD_ROOT` must name a new or this task's existing absolute directory. The
output directory must be new; packaging refuses to delete existing artifacts.
`KETPLUS_LINUX_PACKAGE_OUTPUT_DIR` can select a new output when reusing a build.
The script builds Release Pro with `KETPLUS_PRODUCTION_RELEASE=ON`, remote demo
disabled, and the production control policy; it does not change account or
service configuration. Qt 6.11.1, Node and browser/sidecar dependencies use their
existing pins. The AppImage runtime is pinned by immutable asset ID and SHA-256
in `appimage-runtime.json`; a download or cache hash mismatch aborts packaging.

The build uses three compiler jobs and `SOURCE_DATE_EPOCH` from the public source
commit. Package timestamps and ownership are normalized. Dependency downloads,
the compiler image and generated code can still vary; this is a repeatable
recipe, not a claim of bit-identical artifacts across arbitrary SDKs or hosts.
Keep the Docker image identity, source commit, metadata and checksums with results.

## Installed layout and checks

The AppImage has `AppRun` and `usr/`; the deb installs the same runtime under
`/opt/ketplus-pro`, with a `KetPlus` launcher, desktop entry and hicolor icons.
`ketplus-pro --build-info` reports the bundled daemon's build identity and fixed
production URL. `ketplus-pro --daemon` starts that daemon. Both formats include
dynamic Qt libraries, QPA/QML plugins, the WebView backend, QtWebEngineProcess,
WebEngine resources/locales, the browser helper's pinned Node runtime, the Claude
sidecar, and license inventory. The browser engine follows the existing browser
runtime's first-use installation policy; the package does not preinstall Chromium.
Provider CLIs and provider authentication are user-managed.

The deb contains `ketplus-pro-daemon.service` as an **optional, disabled user
unit**. It has no maintainer script and never starts a daemon at install time.
Enable it only after a user explicitly chooses to run a background daemon.
Uninstall/rollback through the package manager; stop a user-enabled unit first.
AppImage rollback selects the previous complete AppImage. Neither path replaces
individual files inside an active installed runtime.

```sh
docker build -f packaging/linux/Dockerfile.desktop-smoke -t ketplus-pro-smoke .
docker run --rm --network=none -v "$BUILD_ROOT/desktop/linux-packages:/artifacts:ro" \
  ketplus-pro-smoke /artifacts/KetPlus-Pro-VERSION-x86_64.AppImage https://api.ketplus.app
docker run --rm --network=none -v "$BUILD_ROOT/desktop/linux-packages:/artifacts:ro" \
  ketplus-pro-smoke /artifacts/ketplus-pro_VERSION_amd64.deb https://api.ketplus.app
```

The clean image has no Qt SDK, account or provider executable. It tests actual
AppImage extraction/deb installation, dynamic linkage, fixed build-info policy,
daemon/CLI workspace operations with synthetic local data, token permissions,
SIGTERM and GUI startup under Xvfb. The root-only test disables the WebEngine
sandbox inside the disposable container; the shipped launcher never does.
No browser session, hosted account, provider quota or external network is used.

Artifacts include both packages, `BUILD-METADATA.json`, a preliminary SPDX 2.3
inventory and `SHA256SUMS`. `publicationAuthorized` is always false; a successful
smoke test does not satisfy the remaining licensing or release-signing gates.
The release checklist must add complete transitive notices, corresponding source
and any relinking materials, a per-platform audit, and Debian repository signing.

## Native CI

`linux-pro-packages.yml` is reusable. The normal build/release workflow calls it
for both architectures; a build-only integration PR workflow calls it for arm64
when packaging changes. amd64 uses Ubuntu 24.04; arm64 uses the native
`ubuntu-24.04-arm` runner with the same recipe. Read-only repository permissions
and a seven-day workflow artifact upload retain packages/checksums for QA.
There is no release upload, feed update, production secret or signing step.
No manual release workflow dispatch is needed. Native ARM CI is the selected
execution route; never enable host emulation as a fallback.
