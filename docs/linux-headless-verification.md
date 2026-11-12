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
[`linux-headless-daemon.md`](linux-headless-daemon.md). Private relay and mobile acceptance procedures are maintained with those services.
This public repository documents only its native daemon/client checks.
