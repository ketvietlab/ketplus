# ADR 0043: Explicit tool-argument excerpts

Status: accepted; incremental #128 detail retention after ADR 0039.

For eligible completed/failed MCP and dynamic-tool items, arguments larger than
5 KiB become a UTF-8 prefix of at most 4 KiB plus an explicit retention notice.
Object/array arguments are serialized as compact JSON before taking the prefix;
string arguments retain their text representation (valid JSON object/array strings
are decoded for redaction). The existing QtCore-only transcript sanitizer is shared
with the daemon: redact sensitive keys, credentials in URLs, recognized secret text
and inline media **before** serialization/truncation. Otherwise flattening an object
could bypass key-based UI redaction. This is the same heuristic redaction boundary,
not a promise to recognize arbitrary secrets. Embedded URL redaction now walks an
immutable input with `globalMatch` and builds one output, avoiding repeated QString
detachment/copies for many URLs. Matching and URL-redaction rules are unchanged;
a heavy URL fixture measures the bounded candidate path. The threshold leaves
room for metadata so a small record does not grow simply to carry a marker.

Remove the original `arguments` field and store `arguments_excerpt` as display
text. A partial JSON prefix is never presented as a complete argument object or
reusable tool input. Old clients may show less argument context; new clients label
normal and browser receipts `Arguments trimmed` or `Text and arguments trimmed`.
Tool/server identity, status, error, result, media and unknown item fields are
unchanged by the argument transformation. The separate text-result policy can
still apply in the same pass. Links or argument-derived details beyond the excerpt
are intentionally no longer available; this is completed-history detail retention,
not a live tool-call transformation.

`compacted.arguments` records policy `tool-arguments-v1`, timestamp, cutoff,
original/retained bytes (excluding the display notice), `excerpt_only: true`, and
original representation `format` (`text` or `json`), and
`redacted_before_excerpt: true`. Retained bytes count the sanitized prefix. If this is the first
transformation, the top-level marker uses that policy too. If a text-only
`tool-result-text-v1` marker exists, preserve its accounting and append the argument
metadata without trimming its result again. Unknown markers, existing excerpts and
already-capped arguments remain untouched. This is a deliberate, narrowly allowed
extension of ADR 0039's marker idempotence rule.

The journal candidate predicate permits that one known text-marker upgrade. It
keeps the same transaction, row/byte budgets, age, idle, ACK, current-generation,
latest-20-turn, and pending-question/approval guards. Manifest/oversized payloads
remain in the separate large-payload retention scope. No user message, approval,
question or active item is transformed. The JSON excerpt is not reparsed by UI.

Large structured results, blobs/media policy, archive expiry, size pressure and
complete Storage controls remain #128 work. This does not imply a production
release, installed-journal compaction or a 9 GB acceptance result.
