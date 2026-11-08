# ADR 0085: Bound stored policy JSON before materialization

Status: proposed; backend hardening H1.6, implementing the existing ADR 0042/0046 limit.

Both policy tables already reject ordinary writes above 4 KiB with a schema
CHECK. That does not make reads from unsupported or corrupt stored rows safe:
the previous readers materialized and parsed any stored JSON, including a valid
two-field object padded beyond the documented bound. Such a row could enable
automatic trimming instead of entering the existing invalid-policy state.

Project the policy column through a SQL CASE. Only TEXT/BLOB values whose
`octet_length(policy)` is at most 4096 reach Qt and JSON parsing. Keep the row in
the result with a NULL policy when rejected; filtering it out would incorrectly
turn corrupt stored state into default revision zero. Existing shape/revision
validation, transaction rollback and compare-and-swap remain unchanged. Valid
BLOB JSON accepted by the previous reader remains compatible. The byte check
uses the database encoding, consistent with the existing BLOB-cast schema CHECK.

SQLite documents that [octet_length of a TEXT/BLOB table column](https://www.sqlite.org/lang_corefunc.html#octet_length)
uses record metadata without reading the value and that [CASE is lazy](https://www.sqlite.org/lang_expr.html#the_case_expression).
Do not replace this with a post-read byte count or `length(CAST(... AS BLOB))`
projection. If the SQLite runtime lacks octet_length, query failure disables the
operation; there is no fallback that loads an oversized value or resets defaults.

An invalid stored policy preserves its row and replay detail, rejects a save,
and remains invalid after reopening. Repairing the row permits a revision-fenced
save and normal configured retention. Missing rows still use existing defaults
on a supported runtime. No schema migration, UI, default, or pressure-ordering
change is introduced. This bounds policy JSON admission, not all journal reads,
SQLite page-cache memory, corrupted file recovery or total daemon memory.

Validation uses temporary journals with 4095/4096-byte positive controls and
4097-byte/1 MiB rejection cases, for both policies and TEXT/BLOB storage. Normal
writes are first checked against the schema; only the synthetic fault injection
connection temporarily bypasses CHECK constraints to construct bad stored rows.
It then restores checks. Tests cover unchanged bytes/revision/history, reopen,
repair and a successful subsequent retention pass. No installed journal is used.
