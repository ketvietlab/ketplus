# ADR 0130: OS credential storage and recoverable file migration

Status: Implemented, pending security review and platform release verification.

## Decision

Security audit DA-03 is an informational storage-contract discrepancy. Base64 in
the previous owner-only JSON file is encoding, not encryption. Decision D-7 chooses
the existing promise: long-lived credentials belong in an OS credential store.

`ketplus_control_crypto` keeps the `CredentialStore` API shared by the daemon and
remote client. Each service namespace has one versioned secret record containing
its accounts. An `update` or `removeIf` replaces that record atomically; unchanged
transactions do not write. A per-service process lock serializes migration and
read/modify/write operations by cooperating clients. The empty owner-only lock file
is not a credential store; its kernel lock is released on close or process exit,
including when migration cleanup fails. Do not unlink it while writers are running.
`CredentialStore::batch` provides an operation-scoped view under that same lock.
After any required migration, it decodes one fresh record and commits staged
changes at most once. Read-only batches do not write. The view must not escape its
synchronous callback, dispatch events, call external callbacks or perform network
requests. Aborting a callback discards its staged changes. Failed commits do not
publish envelopes or retain metadata encrypted with uncommitted keys. There is no
process-wide snapshot or TTL: each subsequent operation reads the store again.
The one exception is key material: a `CloudCrypto` keeps the private and session keys it
has read or created in memory, because sealing a backlog read the whole record twice per
event on the daemon's only thread until clients timed out. Within one enrollment and
workspace a key never changes under its account, so this cache only skips reads. The daemon client clears it
whenever keys can be removed or replaced: sign-in, sign-out, enrollment, workspace adoption,
transport or device revocation, and pruning of deleted conversations. Every other account is
still read from the store. The keys stay in the process's memory for longer than one
operation; they were already readable by that process.
A cooperating writer that encounters the held lock receives a retryable error;
a successful later operation sees its committed changes.

Heartbeat metadata sealing, distribution and pairing preparation use these views.
Distribution rechecks pins in the same view used to prepare envelopes, and device
list filtering parses the pin map once per list. Existing account/enrollment
fences and sender-cache pin checks still apply. This does not change the record
format, native store permissions or key lifecycle. Storage and local cryptography
remain synchronous: batching bounds native fetches, not event-loop latency.

Records are bounded to 16 MiB;
exceeding the bound is an explicit error, never truncation or a file fallback.

- macOS uses a generic-password item in the default login keychain. The service
  name is prefixed with `dev.ketplus.credentials.v1.`, and the account is `accounts`.
  Items do not opt into synchronization. Default Keychain access binds the creating
  application; a differently signed or rebuilt executable may need authorization.
  Access is refused when the keychain is locked. The legacy login-keychain API is
  used for desktop and headless compatibility; it is deprecated by Apple. The
  implementation suppresses interaction for each serialized synchronous operation
  and restores the process's prior interaction setting afterward. This also covers
  legacy ACL prompts that do not honor the per-query UI suppression flag.
- Linux uses the Secret Service default collection through Qt DBus on the OS user's
  local Unix session bus. The collection must already exist and be unlocked.
  Locked/unavailable collections, denied access, malformed replies and operations
  requiring a prompt fail explicitly. Prompt objects are dismissed, never displayed;
  KetPlus does not unlock a collection or collect its password. Calls have a
  three-second timeout. The `plain` session algorithm is restricted to the local Unix
  bus; it is not network encryption. Secret Service provides the at-rest store.
- Other platforms currently return an unavailable-store error. Pro Windows release
  remains deferred; no plaintext compatibility fallback is provided.

An OS store does not isolate secrets from every process running as the same user.
macOS item ACLs and Linux service policy differ. Linux policy may permit other
processes in the same login session to read unlocked items. Protect the OS account,
its session bus and the store's unlock credentials. Provider CLI credentials and
local IPC tokens use their own storage contracts and are unchanged.

## Migration and recovery

The previous `credentials-<service>.json` is a migration source only. No new
credential values are written there. A backend error is never interpreted as an
empty store, and the file is never used as an alternative while OS storage is down.

1. Acquire the service lock and read the OS record. Fail on unavailable/corrupt data.
2. If a legacy file exists, open without following symlinks and verify a regular,
   singly linked file owned by this OS user with no group/other permissions. Parse
   bounded JSON and strict base64 values; unsafe/corrupt sources remain untouched.
3. If no OS record exists, store the complete account map with a digest of the
   source bytes. Read back and compare the complete record before deleting anything.
4. Re-read the source and require unchanged bytes before removing it. Cleanup
   failures remain errors; a later call retries cleanup without importing again.
5. When an OS record already exists, its data is authoritative. A matching legacy
   digest permits cleanup only, including after a crash or restoration of an old
   backup. A different legacy file is a conflict: preserve both, report the error,
   and require explicit recovery. Never resurrect removed enrollment or pins.

On a locked/denied store, unlock or authorize the installed application through the
OS's administration tools and retry the failed operation. Headless Linux needs a
Secret Service implementation and unlocked default collection available to the
same user/service session; installing the daemon alone does not provision that
store. Missing service access prevents enrollment and cloud cryptography but does
not intentionally disable local-only editing or provider authentication.

For a conflicting legacy backup, stop credential writers, preserve both copies,
and have the user choose which enrollment to recover. Move a rejected legacy copy
out of the migration path before retrying; do not merge identities automatically.
Do not delete the OS record just to bypass a migration error. Loss of the OS store
without a usable backup can require re-enrollment, re-pairing and loss of access to
older encrypted content. Sign-out preserves existing identity/content-key retention
semantics while removing enrollment in one transaction; a failed durable write must
remain a reported failure, not a successful logout claim.

OS-store backups may still contain encrypted credential records. This change does
not erase historical plaintext backups, guarantee secure deletion on flash storage,
or promise cross-device keychain recovery. Protect and retire legacy backup copies
according to the user's backup policy. No automatic backup export is added.

## Enrollment transaction and restore errors

The enrollment caller uses one `CredentialStore::update` for the empty device-pin set,
hosted service and identity fields, account labels, key version and workspace content-key transition.
The complete enrollment is one commit unit, including recovery after an interrupted or failed
write. The workspace transition helper mutates the transaction's account map without nested
store operations. When crossing services, removal of upload cursors belongs to this transaction too.

Daemon restore checks read errors before applying legacy migration, then verifies that the
relevant accounts still match inside the migration transaction. Failure preserves the stored
record and grants no in-memory enrollment. The Control v2 client also rejects partial session
and host-pin restoration; it never treats an unreadable pin map as an absent legacy map.
Malformed pin records are errors. Recovery requires restoring OS-store access and recreating
the client so the complete state is read again. No fallback credential layout is introduced.

Enrollment invalidates trust/Atlas access even if its storage transaction fails. Persisted old
credentials are retained on failure, but delayed responses cannot complete as the next
enrollment. Successful commit cancels earlier credential waiters before starting fresh
challenge/redemption. Both reply stages check account epoch, enrollment revision and identity;
the sign-in epoch itself is retained for the initiating callback.

## Verification and limitations

Native fixtures use an isolated temporary macOS keychain, synthetic credentials,
and a separate executable to check access denial without UI. Generic tests cover
migration, strict parsing, source retention, read-back failure/retry, restored backup
conflicts, locking and atomic removal failure. Secret Service protocol tests use a
private D-Bus daemon and fake service, including lock/denial, prompt dismissal and
service absence. Ordinary test mode refuses the production OS backend; other daemon
fixtures continue linking in-memory credentials.

### Full Pro validation follow-up

On 2026-11-16, implementation commit
`b4a046f13d7b1c667a592248a24f38d65e9d857c` completed a full Pro Release build
on macOS 26.2 arm64 with Qt 6.11.2 and Apple Clang 21. Desktop and tests were
enabled; Computer app bundling/signing was disabled. The unfiltered serial run
(`ctest --test-dir build/credential-pro --output-on-failure -j1`) registered
214 suites: 210 passed, three failed and one was skipped, in 1698.59 seconds.
The skipped SSH install transaction suite requires Linux `/proc`, `fcntl` and
`SO_PEERCRED`; this run provides no coverage for it.

The failures were retained and investigated:

- Desktop session discovery reproduced independently because ordinary test mode
  disables the native credential backend, while this fixture had no fake store.
  Linking the existing in-memory credential fixture and resetting it before each
  case restored synthetic daemon identity creation. The complete discovery suite
  then passed in 7.38 seconds; production credential behavior was not changed.
- Usage retention failed while seeding large tool payloads, and storage jobs
  failed while saving storage options after creating an 850 MB fixture. Free disk
  space was approximately 1.1 GiB near the failures and 842 MiB after the run.
  Both unchanged test executables passed together in 76.49 seconds after free
  space recovered to approximately 12 GiB. This is consistent with disk pressure,
  but does not establish the original write error conclusively. No journal code
  or assertions were changed to obtain the passing rerun.

A subsequent full build at `23d1e298e51e25918857db1e1509188678c9f8b8` passed,
but its serial test run exposed three additional missing-fingerprint failures in
machine end-to-end, headless daemon and SSH daemon integration fixtures. That run
was stopped after 100 completed suites. The subprocess fixtures launched the
production daemon with temporary data directories, which did not isolate the
native credential namespace. The earlier run is therefore not evidence of native
store isolation for these subprocess tests. Their test-only daemon now links a
persistent synthetic backend, with no production fallback or runtime opt-in.
Native-store behavior remains covered separately by the disposable Keychain and
private-bus fixtures.
With this fixture isolation, the complete machine end-to-end, headless daemon,
SSH daemon integration and Copilot CLI suites passed together (4/4, 15.60 seconds).
The headless suite includes fingerprint persistence across subprocess restarts.

The final full Pro Release build and unfiltered serial CTest run at
`fab40c16ddd8850aee9c2f97fc2afa66e0950e9d` both exited successfully. All 214 suites
were considered: 213 passed, zero failed and the Linux-only SSH transaction suite
was skipped. CTest took 1524.03 seconds. This includes the previously failing
journal, discovery and subprocess suites, plus the credential, trust-lifecycle
and sender-cache suites. No test reruns were required for this final run.
Desktop and tests remained enabled, and Computer app bundling/signing remained
disabled. Live provider opt-ins were disabled. The following evidence-only
documentation update does not change the tested source or build configuration.

Real Linux Secret Service implementations, Linux OS-user ACLs/backups, signed
Desktop upgrade prompts, Windows, packaging and deployment still require platform
verification. Passing a fake D-Bus service on macOS is not Linux native acceptance.
Storage remains synchronous; performance under large real credential catalogs is
not established by these security fixtures.

References: [Apple Keychain search attributes](https://developer.apple.com/documentation/security/search-attribute-keys-and-values),
[Secret Service API](https://specifications.freedesktop.org/secret-service/latest-single/),
[Secret Service prompts](https://specifications.freedesktop.org/secret-service/latest/prompts.html).
