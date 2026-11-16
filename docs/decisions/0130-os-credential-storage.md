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

## Verification and limitations

Native fixtures use an isolated temporary macOS keychain, synthetic credentials,
and a separate executable to check access denial without UI. Generic tests cover
migration, strict parsing, source retention, read-back failure/retry, restored backup
conflicts, locking and atomic removal failure. Secret Service protocol tests use a
private D-Bus daemon and fake service, including lock/denial, prompt dismissal and
service absence. Ordinary test mode refuses the production OS backend; other daemon
fixtures continue linking in-memory credentials.

Real Linux Secret Service implementations, Linux OS-user ACLs/backups, signed
Desktop upgrade prompts, Windows, packaging and deployment still require platform
verification. Passing a fake D-Bus service on macOS is not Linux native acceptance.
Storage remains synchronous; performance under large real credential catalogs is
not established by these security fixtures.

References: [Apple Keychain search attributes](https://developer.apple.com/documentation/security/search-attribute-keys-and-values),
[Secret Service API](https://specifications.freedesktop.org/secret-service/latest-single/),
[Secret Service prompts](https://specifications.freedesktop.org/secret-service/latest/prompts.html).
