# SSH installer verification

Scope: issue [#666](https://github.com/ketvietlab/ketplus/issues/666), ADR 0123.
Production installation is disabled until the release owner supplies the dedicated
Ed25519 public key. No production key, release publication, CI secret, sudo,
OS-user creation or hosted relay enrolment is part of this change.

The Linux host proof uses an isolated **same-UID** HOME/XDG profile, task-owned
QA keypair and loopback manifest server. Its runtime is a complete `cmake --install`
stage with `control_url=http://127.0.0.1:9`, not a copied build-tree executable.
The private QA key is outside the repository and is never printed or committed.

Verification in progress; final native/GUI totals and captures are recorded here
only after the corresponding test run succeeds. The initial Linux filesystem
transaction suite passed 13 tests. The initial headless C++ targets compiled.
These facts alone do not establish UI install/Connected proof.
