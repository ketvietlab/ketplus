# ADR 0089: Reject malformed provider process generations

Status: accepted for malformed normal-sized stdout frames; broader RPC schemas remain separate.

The three adapters silently discarded invalid JSON and non-object frames. A broken
response could leave a call waiting indefinitely. A valid-looking suffix from the
same process could also complete initialization or a different pending call after
the corrupt frame. Qt's JSON decoder additionally accepts raw control bytes in
strings, so its parse-error result alone does not enforce this framing boundary.

Reject a normal-sized frame when parsing fails, its root is not an object, or it
contains raw JSON control bytes. Share only the control-byte rule: escaped control
characters and JSON whitespace outside strings remain valid. UTF-8 validation and
JSON syntax remain the Qt parser's responsibility. Empty whitespace lines retain
the existing framing behavior; this is not an incoming method/schema validator.

A rejected frame marks its provider process generation unusable before killing the
process, clears the line reader/buffer and closes stdout. Later buffered output
is drained without parsing. The existing finished path fails queued/pending calls
with a static protocol diagnostic, clears active runtime state and applies restart
backoff. Existing model-catalog error-to-fallback behavior remains in place. It does not publish a second stopped signal when initialization already
published its failure. A fresh launch resets rejection and can serve calls again.
Codex's line-reader generation fence also stops parsing the current coalesced chunk.

The fixture uses its own binary as a fake provider. It covers all three adapters,
initialize and ordinary outstanding calls, malformed UTF-8, NUL/control bytes,
raw tabs/CR inside strings, arrays and malformed syntax. A valid-looking suffix
must not be dispatched. Both outstanding callbacks fail once; diagnostics do not
contain fixture payloads. After the process exits a fresh call must succeed.
Valid JSON controls include blank lines, outside-string whitespace and escaped
control/quote/backslash characters. On original production the final fixture
reports 42 failures and 5 passing control/setup checks. Process cases explicitly
skip on Windows; no real provider or quota is used.

A QtCore-only header shares the control-byte check; existing adapter state owns
quarantine/lifecycle. CursorAdapter is already above 1,000 lines; this change is
limited to launch, stdout parsing and finished cleanup, without unrelated ACP
refactoring. Full Release Pro build/tests and independent exact-head review are
required. No UI implementation, installation, user journal or deployment changes.

Valid-object method schemas, wrong/unknown IDs and absent-response deadlines remain
separate RPC work. Oversized Codex selective extraction is unchanged. This does not
claim a universal QProcess memory bound or live-provider/platform acceptance.
