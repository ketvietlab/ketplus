# SSH installer verification

Issue [#666](https://github.com/ketvietlab/ketplus/issues/666), ADR 0124.
Production installation remains disabled until the release owner supplies the
dedicated Ed25519 public key. No release, signing secret or production key was
created or published by this change.

## Linux proof

The offscreen test compiles the production Connections dialog, machine row,
model/controller, network verifier/downloader and SSH installer with Két Widgets.
It uses a fresh **same-UID HOME/XDG profile**, not a newly created OS user, and a
non-root loopback sshd with throwaway host/user keys. The release QA keypair is
outside the source tree; its private key is never logged or committed.

The daemon payload is the complete `cmake --install` stage from public revision
`3e61b29314df`, before the final unrelated integration merges, including Qt libraries/plugins,
sidecar, skills and licence notices. Its verified identity is `0.1.52`, control URL
`http://127.0.0.1:9`, runtime URL overrides disabled. It passes `--check-runtime`
without SDK library/plugin environment overrides; its installed RUNPATH is
`$ORIGIN/../lib`. No existing deployment, provider turn, real relay enrolment,
sudo or OS-user creation was used.

The final QA archive contains 6,858 entries, 267,220,682 compressed bytes and
581,745,817 expanded bytes. SHA-256:
`f2fbfc52a0b8454e4009c03035b3c88b1621f92f5ee13272c9089a646b30ec9f`.
The final installer is tested against this signed version-matched QA runtime.
The test generates a signed local manifest for this archive and verifies it with
an explicitly injected QA public key. This proves the consumer protocol; it is
not evidence that a production release/key exists.

Verified transitions:

- Missing public key: disabled install control and **zero HTTP requests**, including
  a direct controller request that bypasses the disabled button.
- Modified manifest signature: no consent, archive download, remote runtime or daemon.
- Decline after signed metadata review: no archive download or remote installation.
- Cancel immediately at runtime-ready: confirmed rollback, **zero connection requests**,
  no activation, data, socket or newly created external credential retained.
- Confirm install: signed full runtime → verified SSH → authenticated `daemon.hello`
  → Connected. An overlapping Remove request is refused while management is open.
- Stop the managed daemon, review and Start: Connected again, with **no further HTTP**.
- Details → Manage daemon → idle-only Uninstall: wait for the remote transaction to
  finish; active runtime and receipt removed, user data/credential retained.

## Final checks

With the explicit QA inputs, all six focused suites pass on Linux, offscreen;
no test is skipped in that proof:

| Suite | Result |
| --- | --- |
| `ketplus_ssh_install_ui_tests` | 8 passed, 0 failed, 0 skipped |
| `ketplus_remote_ssh_tunnel_tests` | 21 passed, 0 failed, 0 skipped |
| `ketplus_remote_ssh_backend_tests` | 18 passed, 0 failed, 0 skipped |
| `ketplus_daemon_upgrade_tests` | 6 passed, 0 failed, 0 skipped |
| `ketplus_daemon_release_tests` | 19 passed, 0 failed, 0 skipped |
| `ketplus_ssh_install_transaction_tests` | 22 passed |

The transaction suite uses disposable fixture runtimes to cover committed upgrade,
rollback after upgrade/uninstall/startup, interrupted uploads, checksum/runtime/
archive failures, busy refusal, downgrade refusal, writable parents, unknown or
modified runtime files, parent-traversal paths, and preservation/removal of
existing/new external tokens.
Rollback releases staging before restarting the previous process and confirms the
restored socket peer; the restart fixture refuses to start while staging remains.
These fixture tests are distinct from the real-daemon Connections proof above.

Public-tree hygiene: **4/4 pass**. Changed C++ files are clang-formatted and
`git diff --check` passes. The ordinary full Pro application was not built on
Linux: that host lacks the editor's Qt Core5Compat/WebView SDK. The optional
standalone target tests the actual changed Connections surface without those
unrelated editor modules; it does not replace a full platform release build.

## Rendered consent

Light/dark captures are linked in [PR #667](https://github.com/ketvietlab/ketplus/pull/667)
using the earlier evidence commit; no PNG assets remain in this change's final tree.
Both captures are from the final QA fixture, with synthetic host/key data. The
rendered description fits without clipping and its normal-text contrast is at
least 4.5:1 in both themes. The images were inspected; the consent UI is unchanged by the review fixes. Only the two confirmation
actions remain during consent; the connection action and obsolete choices are
hidden. See `docs/testing.md` for the reproducible opt-in QA configuration.

## Review constraints and retained output

The leader explicitly prohibited OS-user creation and sudo on the QA host and
approved fresh same-UID HOME/XDG isolation instead. The proof uses the actual
production Connections widgets/controller offscreen, not a separate OS account.
[Sanitized Linux output](linux-proof.txt) retains actual test output without
private paths, keys, hostnames or account details.

The Connections target is now registered in normal desktop builds/ctest and
reuses the desktop's kit target, with no second FetchContent pin in that mode.
Two consent-routing cases need no QA artifact or SSH server and run normally.
The full Linux host proof still requires a local signed fixture/runtime as
documented; it is not claimed to run without those opt-in QA inputs.

[macOS CTest output](macos-transaction.txt) proves the Python target skips cleanly
before Linux-only imports. This invokes the production registration module from
a minimal CMake/CTest project on macOS; no native build is needed for that target.
Python is found quietly outside Linux, so this addition imposes no new Python
requirement on other hosts.

The review-fix validation ran the five non-UI suites successfully on merged
integration, then reran the UI suite after correcting the new routing fixture
to attach the SSH backend and select the host. The final UI rerun is 8/8, no
failures/skips. No product code changed between those checks. A separate normal
invocation without QA inputs passes both routing cases (4 Qt passes including
init/cleanup); the four full-host cases explicitly skip without their inputs.
That no-QA invocation is not counted as the full Linux proof.

Additional regressions cover the archive-size timeout budget (including the
267,220,682-byte fixture and 512 MiB cap), decoded URL dot segments, and refusal
of runtime/data on different filesystems before writes. ADR 0124 documents the
SSH/logind `KillUserProcesses=yes` lifetime limitation; no service/linger change
is made. Windows was not run locally; it follows the same pre-import platform
skip branch proved on macOS.
