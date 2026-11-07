# ADR 0047: Bounded large-tool payload retention

Status: accepted; bounded extension of #128 detail retention, not image retention.

The idle detail pass can replace a completed large-tool manifest with its normal,
explicitly marked compacted payload. It reuses the existing command, tool-result,
argument, structured-result, reasoning and diff transformers. Only version-1
fragment-backed JSON payloads whose bytes plus manifest fit the existing 1 MiB
per-pass budget qualify. The 128-row scan budget is unchanged. Legacy monolithic
blobs, larger items, image types and unknown manifest extensions remain intact.

Before hydration, apply the same current-generation, idle-state, terminal-turn,
latest-twenty, age and question/approval guards. This lane additionally requires
**no raw events anywhere in the session generation**, stronger than the ordinary
per-turn ACK check: it prevents the orphan-reference check from scanning another
turn's large raw backlog. Sessions with pending raw events are revisited later.

Validate the manifest against stored item identity, revision, byte count, encoding
and digest. Read at most nine indexed chunks (each at most 128 KiB), require
contiguous offsets and the exact total, and verify SHA-256 before parsing. SQL
bounds the selected blob value even if a corrupt chunk exceeds its stored format.
Corrupt, incomplete or unsupported data is retained. Account failed hydration
against the pass byte budget as well. An individually oversized item advances the
scan instead of stalling it forever.

Only accept a compacted payload that shrinks and fits the ordinary 256 KiB event
limit. Keep unknown top-level fields; if they prevent that bound, retain the whole
large item. Replace the replay row, remove its large-payload metadata/chunks only
if no other retained reference names them, and increase projection revision in
one transaction. Any failure, including orphan cleanup or revision update, rolls
back the row and blobs together. Shared manifests keep their payload readable.
Readers holding an old projection receive the existing snapshot-changed fence;
an unreferenced transfer is no longer fetchable after commit.

This does not decode images or delete external files, media-store objects, user
messages or unsupported resource results. Asynchronous preview conversion,
media policy, large payloads above this bound, archive expiry and complete
storage-limit acceptance remain separate #128 work. No deployment or installed
journal migration is implied.

The retained-reference lookup uses `replay_events_item_transfer` on session,
generation, item ID and the manifest transfer expression. Journal initialization
creates it for existing databases too, so GC seeks an exact transfer instead of
JSON-parsing every retained update for the item. Building that index is a one-time
startup cost proportional to retained history; multi-GB startup is not yet
validated. The regression reopens an old-format journal with 300,000 same-item
replay rows, checks the expression query plan and preserves those protected rows.

The correlated GC comparison deliberately uses `= +p.transfer_id`. Unary plus
removes the column's TEXT affinity so SQLite can seek the expression index's
fourth key. Tests EXPLAIN the production predicate with bound parameters; a
literal-only approximation does not exercise this affinity behavior.

See SQLite [expression affinity](https://www.sqlite.org/datatype3.html#affinity_of_expressions) for the unary-plus rule.
