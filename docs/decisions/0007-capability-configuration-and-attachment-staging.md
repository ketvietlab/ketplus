# ADR 0007: Capability configuration and encrypted attachment staging

Status: accepted for the internal Control API v2 slice.

## Decision

Advanced conversation controls are daemon-authoritative commands, not prompt
text or client-local switches. The daemon obtains model and reasoning choices
from the active provider adapter, returns them only inside the encrypted command
result, and validates every apply against the advertised list, session
generation and configuration revision. The initial execution policies are
`ask` and an explicitly acknowledged `full`; unsupported policy and mode values
are not advertised. Provider adapters translate this neutral state at the final
turn boundary.

Attachment staging uses four idempotent commands: begin, chunk, commit and
cancel. Each 32 KiB plaintext chunk is independently protected by the existing
session E2EE command envelope. The cloud stores only bounded opaque Command rows,
never filenames, media types or attachment plaintext, and redacts them with the
existing 24-hour command TTL. The backend caps a session at 1,280 live chunk
rows and 64 MiB ciphertext. It may lease chunks in parallel, but must not lease
commit or a later non-chunk command while a chunk for that session is in flight.

The endpoint daemon stores one SQLite BLOB row per chunk, limits a draft to five
files, 20 MiB per file and 40 MiB total, and accepts only PNG, JPEG and bounded
plain text. Commit requires the exact contiguous chunk count, byte count and
SHA-256 digest. Abandoned local staging expires after 24 hours. Image files are
materialized with owner-only permissions only for provider input, then removed
on completion or provider error; committed staging rows are removed with them.

## Consequences

- Relay/database rows remain bounded; no attachment becomes one large JSON row.
- A stale UI cannot silently overwrite model, effort or execution policy.
- Capability absence is represented by hiding or disabling the control.
- Cloud durability and retry do not expose attachment content to the service.
- The mobile client also keeps an atomic, device-only encrypted draft so a
  pre-submit app restart does not lose selected bytes.
- Raster parity still requires an authorized native capture; deterministic
  structure and interaction checks are not a substitute for that gate.

## Source organization note

This first cross-repository slice keeps the new handlers beside the existing
`SessionHost` command dispatch and the attachment transactions beside the
existing `LocalJournal` schema/queries, even though both existing translation
units now exceed 1,000 lines. Splitting them before the concurrent command-kind
reconciliation would duplicate shared private types and make the pending
session-create/files integration materially riskier. A behavior-preserving
split by advanced-control and attachment ownership is required immediately
after the canonical command union lands; no further behavior should be added to
these translation units first.

The visual acceptance gate is at most 5% raster mismatch for every
screen×state×viewport×theme. Missing or extra structural controls, clipping,
broken interaction, and accessibility failures remain hard failures regardless
of the raster percentage. Native capture is currently unavailable, so this ADR
does not claim that gate passed.
