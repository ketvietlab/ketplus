# ADR 0074: Preserve provider response id types

Status: accepted for matching responses to outgoing calls; broader RPC validation remains under audit.

All three adapters assign positive integer JSON ids to outgoing calls, but matched
responses by coercing QVariant to an unsigned integer. String ids, leading-zero
strings, true and fractional numbers could consume the numeric initialize callback.
Codex's oversized-response path made the same conversion and could fail the wrong
pending call. A fake-provider handshake reproduces 15 failures; 23 controls pass.

Use one QtCore-only response-id helper for the three ordinary response paths and
Codex's oversized response handling. Only a positive, integral JSON number that
fits the signed 64-bit representation used on the wire can look up the pending
call. Do not coerce strings, booleans, containers or fractions. Unknown/invalid
ids preserve pending callbacks, matching the existing unknown-id behavior; no
new timeout or failure policy is introduced. Provider-originated request ids are
not constrained by this outgoing-call helper.

The focused fixture passes 39/39. Its stdout barrier proves the malformed response
was processed before asserting that initialize remains pending. A correctly typed
response then completes initialization and one subsequent call exactly once.
It covers all three adapters, Codex oversized messages, valid numeric controls,
unrelated ids and numeric representation limits. The fixture launches its own
binary through temporary POSIX symlinks; those process cases explicitly skip on
Windows. No live provider or provider quota is used.

The helper replaces duplicated coercion rather than changing protocol generation.
CursorAdapter is an existing file above 1,000 lines; the change is limited to its
response lookup and include, leaving unrelated ACP behavior in place. Full Pro
build/tests and independent exact-head review are required for the shared header.

Response result/error shape, incoming request-id validation, duplicate fields,
request counter exhaustion, pending-call bounds/timeouts and real-provider/platform
acceptance remain H3.4 work. No UI implementation, user journal, installation or
deployment is changed.
