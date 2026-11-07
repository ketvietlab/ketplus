# ADR 0075: Validate matched provider response envelopes

Status: accepted for the shared response envelope; provider method schemas remain under audit.

After matching a call id, all three adapters converted a missing/scalar result to
an empty object and reported success. An invalid initialize reply could therefore
mark the provider ready. A response containing both result and error was also
accepted, and non-object errors were silently converted to an empty object.

Require exactly one of `result` and `error` for matched responses. An error must
be an object. A result must be an object or explicit null: ProviderAdapter's
callback contract carries QJsonObject, while existing void replies may use null.
Keep message-only errors and string error codes used by the Claude sidecar;
this guard is not a strict JSON-RPC 2.0 error schema or method-result validator.
In particular, null compatibility does not prove a valid initialize capability
schema. That remains separate work.

A malformed matched envelope completes only that pending call, once, with the
static error `Malformed provider response envelope.` Never include the malformed
payload in that diagnostic or narrow capabilities using an ambiguous error.
Initialize follows the existing error/shutdown path. Ordinary bad responses leave
unrelated pending calls and the process available. Late duplicate replies cannot
complete a removed callback a second time. Invalid/unrelated ids retain ADR 0074.

The fake-provider regression adds 87 cases across Codex, Claude and Cursor.
Original source fails 72 malformed-envelope cases; 17 checks (including setup and
cleanup) pass. The combined focused suite passes 126/126 with the fix: initialize
refusal, ordinary call errors, another outstanding call, duplicate replies and a
subsequent valid call. Object/null results and message-only/string-code/empty-object
errors retain compatibility. The helper launches only the test binary through
POSIX symlinks; process cases skip on Windows and use no provider quota.

A small QtCore-only header shares the three adapters' identical envelope rule.
CursorAdapter is already above 1,000 lines; this change only adds the include and
guard at its response dispatch, with no unrelated ACP refactor. Shared-header
changes require full Pro build/tests and independent exact-head review. No UI,
installed daemon, user journal or relay deployment changes are included.

Duplicate keys, malformed method classification, method-specific result schemas,
pending-call admission/timeouts/cancellation, stderr privacy and counter exhaustion
remain open. This package depends on the response-id fix and must be revalidated
at its final rebased head after that parent is integrated.
