# Linux headless verification — 0.1.29

Local verification completed on 2026-10-09. This records the implementation's
tested scope, not production deployment acceptance.

| Area | Result | Evidence |
| --- | --- | --- |
| Default macOS Desktop build | Passed | Full native build and 62/62 CTest targets; Desktop UI/composition unchanged |
| Independent Linux ARM64 build | Passed | Ubuntu 24.04, native ARM64, Qt 6.11.1, Release; 26/26 CTest targets |
| Installed Linux runtime | Passed | Clean Ubuntu without Qt SDK, display or provider accounts; SQLite plugin, relative library discovery, daemon/CLI, token mode 0600, zero-session catalog restart, SIGTERM and systemd unit syntax |
| Standalone release/privacy regression | Passed | 3/3 CTest targets |
| Fake Claude SDK checks | Passed | 14/14 Node tests; no provider calls |
| Control device authorization | Passed | Deployment `ketplus`: 12/12 SQLite and 12/12 PostgreSQL cases; additional focused log-privacy assertion passed |
| Encrypted relay platform matrix | Passed | Linux host with synthetic iOS, Android, macOS, Linux and Windows clients; all five topologies passed |
| Browser consent | Passed | Vietnamese/English, light/dark, 390×844 and 1280×800; mobile action 44px, desktop 36px, no horizontal overflow or CSP errors; approval/completion checked and all sessions closed |
| iOS | Passed | 449 Swift tests in 11 suites with `--no-parallel`; unsigned iOS Simulator build succeeded |
| Contract/boundary checks | Passed | Canonical/consumer OpenAPI byte-identical, all internal references resolve, presets/workflow parse, shell syntax, C++ formatting, diff checks, immutable CM tag unchanged |

The Qt remote-client checks cover pairing/cancellation, tampered session wraps and
events, host decryption of a command, identical ciphertext retry, signed command
results, account replacement, revoked-refresh fencing and reporting credential
removal failure during logout. The final two regressions failed before their fixes.

Reproduce native checks using `docs/testing.md`. Linux build, staging,
clean-runtime acceptance and administration commands are in
[`linux-headless-daemon.md`](linux-headless-daemon.md). Cloud commands run from
the `ketviet` repository's `apps/ketsuite` directory:

```sh
npm run test:ketplus-linux
KETPLUS_TEST_PG=postgres://TEST_USER@127.0.0.1:TEST_PORT/postgres npm run test:ketplus-headless:postgres
```

The PostgreSQL helper creates and drops unique test databases and refuses
non-loopback targets. iOS verification runs from the companion repository after
generating its Xcode project as described in that repository's development guide:

```sh
swift test --package-path apps/ios --no-parallel
xcodebuild -project apps/ios/KetPlusRemote.xcodeproj \
  -scheme KetPlusRemote -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  -derivedDataPath apps/ios/.build/simulator CODE_SIGNING_ALLOWED=NO build
```

The archive is `KetPlus-Daemon-0.1.29-Linux-aarch64.tar.gz`. It contains both
executables, deployed Qt libraries and SQLite plugin, Claude sidecar, agent
skills, license notices and `share/systemd/user/ketplus-daemon.service`.
The local artifact has compiled Control URL `http://127.0.0.1:43110` and cannot
override it at runtime. Rebuild for the intended relay before deploying; this
artifact is for isolated acceptance only.

An earlier x86_64 build compiled under ARM-hosted emulation, but Qt process
creation failed with `Resource error (fork failure): Invalid argument`, including
in a minimal QProcess fixture. This does not establish x86_64 runtime acceptance.
The added GitHub Linux lane targets native Ubuntu x86_64; its CI result must be
checked separately from these local results.

No systemd user manager boot, production relay rollout, real provider turn,
physical-phone pairing, push delivery or device installation was verified.
Desktop remote UI/composition remains deferred and Android remains its v1
preview. Remaining Qt backend integration limits are listed in the runbook.
