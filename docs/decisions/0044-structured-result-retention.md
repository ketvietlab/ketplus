# ADR 0044: Display-only structured tool-result excerpts

Status: accepted; incremental #128 detail retention after ADR 0043.

Successful completed MCP/dynamic items may compact `result.structuredContent`
objects/arrays above 5 KiB. Sanitize the complete JSON using the existing transcript
sanitizer, then retain a UTF-8 prefix in `result.structuredContent_excerpt` with an
explicit notice. Remove the original structured field: a partial JSON prefix is
not a machine-readable result. The prefix has at most 4 KiB, sharing that allowance
with existing text content blocks (existing notices conservatively count toward
it). Arguments retain their separate input budget. If text already consumes the
allowance, the structured excerpt contains only the explicit trim notice.

Preserve result content blocks, status/error metadata, arguments and unrelated
fields. Failed items, non-null item errors and `isError` results (including malformed flags)
keep their structured data. Scalars,
unknown markers and existing excerpts are untouched. Conservatively skip JSON
with depth above 64, keys `type`, `uri`, `url`, `path`, `mimeType`, `blob`, `data`,
keys containing `media`, `resource` or `ref`, or strings containing `data:`/`file:`.
These checks preserve recognized resource-bearing shapes pending media retention;
they are intentionally conservative, not a general schema-aware resource detector.
Large manifests/rows above the existing 1 MiB candidate bound stay out of this pass.

`compacted.structured_result` records policy `tool-structured-result-v1`, timestamp,
cutoff, original/retained bytes excluding the notice, `excerpt_only` and
`redacted_before_excerpt`. First-time compaction uses the same top-level policy.
Known text/argument markers can gain this metadata once without changing existing
accounting; unknown policies remain immutable. UI receipts show `Result details
trimmed` (or `Result and arguments trimmed` when both changed), while the sanitized payload carries the explicit excerpt.

All existing transaction, age, idle, ACK, latest-20-turn, question/approval and
per-pass row/byte guards apply. This is retained history, never live tool data.
Media/large-payload retention, size enforcement, archive expiry and complete Storage
controls remain #128 work; this change does not release or install a daemon.
