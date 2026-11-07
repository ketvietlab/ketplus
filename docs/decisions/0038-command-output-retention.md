# ADR 0038: Bounded command-output retention

Status: accepted; incremental part of #128.

The idle replay-detail pass now handles intermediate usage samples and inline
`item.completed` command output. Both use the same turn eligibility predicate:
current idle generation, completed lifecycle, older than the cutoff, at least
20 actual newer turns, no raw rows awaiting client acknowledgment and no question
or approval events. The covering `events_turn` index remains required.

For command items whose status is completed or failed and whose UTF-8 output
exceeds 21 KiB, preserve at most the first 4 KiB and last 16 KiB without splitting
codepoints. The 1 KiB margin ensures the added notice/metadata does not turn a
near-limit output into a larger stored record. Keep all other item and envelope
fields, timestamps, row ordering and cursors. Add `item.compacted` with `at`,
`cutoff`, `original_bytes`, `retained_bytes` and policy `command-output-v1`.
An explicit output notice and middle-removal separator remain visible through
existing transcript formatting, including clients unaware of the new metadata.
The command-output receipt also returns `compacted`, marks `truncated=true` and
preserves the notice when its usual 40-line tail limit removes the original one.

The scan is still bounded to 128 rows per minute (API maximum 256). Parse at most
1 MiB of candidate JSON per transaction; a candidate exceeding the remaining
budget is retried next pass, with the scan cursor committed only after successful
updates and projection revision increments. At most one additional candidate
(up to 1 MiB) is materialized before the byte-budget check. SQLite eligibility
checks are not a hard wall-clock budget. Busy-timeout restoration, rollback and
snapshot continuation fencing are shared with usage retention.

Large-event manifests, payloads above 1 MiB, nonterminal command items, existing
compaction markers, tool results, image/reasoning content and superseded item
starts are preserved. This avoids changing blob ownership or transcript row
ordering before those retention paths exist. Raw and cloud outboxes are unchanged.
The fixed 30-day service cutoff, configurable retention/size budgets, Storage UI
and archived-history policy remain tracked by #128. No release or installed-user
journal result follows from synthetic tests.
