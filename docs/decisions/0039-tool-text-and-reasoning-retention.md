# ADR 0039: Tool text and reasoning detail retention

Status: accepted; incremental part of #128, following ADR 0038.

Use one storage-independent `compactCompletedReplayItem` transformation behind the existing
journal transaction and eligibility predicate. Command output retains its ADR 0038
policy. Completed/failed MCP and dynamic-tool items can also compact known text
result shapes: a string `result`, `result.content` text blocks, and `contentItems`
text/inputText blocks. If their combined UTF-8 text exceeds 5 KiB, preserve at most
4 KiB of original text across those fields, on codepoint boundaries. A preview
never skips ahead to later blocks after an earlier block exceeds the budget.

Trim text in place and label the first text block of each affected array. Preserve
all block positions, non-text values and text-block metadata/annotations, including
audience restrictions. Blocks past the shared prefix retain empty text plus their
metadata, rather than moving content across annotation or audience boundaries. Preserve tool/server,
arguments, status/error, `isError`, structured content and unknown result fields.
This policy does not claim to cap the entire result or prune media ownership.

For completed reasoning items, clear raw string content or arrays of strings only
when a nonempty summary remains. Preserve its summary and all other fields. Unknown
structured raw content, missing/empty summaries, and explicitly nonterminal status
are kept. Raw content of 1 KiB or less is kept so marker overhead does not grow it.
The UI's existing hidden reasoning behavior is unchanged. Tool rows, including browser
rows, add a visible `Text trimmed` title suffix for this policy; existing accessibility
names consume that title. Expanded details retain the explicit text preview notice.

Each transformed item carries `compacted` with timestamp, cutoff, policy,
`original_bytes` and `retained_bytes`. Tool policy `tool-result-text-v1` counts
original/retained text bytes, excluding notices and separator newlines. Reasoning
policy `reasoning-content-v1` counts original raw content (compact JSON for arrays),
with zero retained raw bytes; its summary is unchanged. Existing markers make all
policies idempotent. The limits and busy-timeout/rollback/revision behavior from
ADR 0038 still apply, including the 1 MiB candidate parse budget and blob exclusion.

Remaining #128 work includes argument/structured-result caps, large-payload and
image retention, file-change caps, safe superseded starts, configurable retention,
size budgets, archive expiry and Storage UI/warnings. No live journal is modified
by the synthetic tests, and no multi-GB or installed-device result is implied.
