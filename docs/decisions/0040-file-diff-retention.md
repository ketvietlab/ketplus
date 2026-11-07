# ADR 0040: Explicit file-diff excerpts with original statistics

Status: accepted; incremental part of #128, following ADR 0039.

Completed/failed `fileChange` items participate in the existing bounded idle
replay-detail transaction. The turn-level age, acknowledgment, current generation,
latest-20-turn, lifecycle and question/approval guards are unchanged. Inline
candidates still share the 1 MiB per-pass parsing budget; large-event manifests
and oversized candidates are not transformed by this policy.

For each string `change.diff` exceeding 64 KiB of UTF-8, retain a codepoint-safe
prefix of at most 63 KiB in `change.diff_excerpt` and remove `change.diff`.
The margin pays for metadata. A partial patch must never masquerade as the full
patch: old consumers see missing diff/unknown counts, and consumers that need to
apply a patch must not use the excerpt. Small diffs and unknown non-string shapes
remain unchanged, as do path, operation, rename destination and all other fields.
Existing per-change compaction markers or excerpts are not overwritten.

Before trimming, calculate full added/removed line counts using the same QtCore
helper as transcript presentation. Add/delete diffs count content lines; update
diffs count +/- lines only within unified hunks. Unknown formats remain unknown.
Each changed entry carries `compacted` with policy `file-diff-v1`, timestamps,
original/retained UTF-8 byte counts, `excerpt_only: true`, and
`stats: {added, removed, known}`. The item-level marker aggregates the changed
entries' byte counts and makes the transformation idempotent. Invalid stored
counts are presented as unknown rather than counted from an excerpt.

Desktop uses the stored original statistics and adds `Diff trimmed` to normal
and compact single-file rows, including their accessible names. File identity and
rename navigation remain unchanged. This is a label addition to existing rows,
not a new details viewer. Other clients may ignore the excerpt/marker safely;
no installed-device or native VoiceOver verification is implied.

This does not implement argument/structured-result caps, blob/media retention,
superseded starts, configurable age/size budgets, archive expiry or Storage UI.
Those remain in #128. Tests use synthetic temporary journals only.
