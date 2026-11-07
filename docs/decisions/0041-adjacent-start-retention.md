# ADR 0041: Retain ordering while removing redundant tool starts

Status: accepted; conservative incremental part of #128.

The existing idle replay-detail transaction can remove a superseded `item.started`
for command, MCP, dynamic-tool and file-change items. All existing age, idle-state,
current-generation, acknowledgment, latest-20-turn and question/approval guards apply.
The immediately following retained event in that session/generation must be an
`item.completed` with the same turn and nonempty item identity. An event from any
other item or turn between the pair prevents removal, even if it would be hidden.
This preserves the first-seen ordering used by legacy and current transcript readers.

The completion must be completed/failed and have matching item identity/type. All
start fields except item lifecycle status must equal their completion fields;
additional completion fields are allowed. Empty known progress placeholders (for
example null tool result or empty command output) may be replaced by the final
value; changed nonempty fields and unknown empty fields still prevent deletion. Envelope fields must also be preserved.
Keep unknown types, start-only or changed fields, assistant/user message starts,
nonterminal completions, existing item compaction markers, manifests and pairs
whose combined inline JSON exceeds 1 MiB. This deliberately favors preservation
over maximum reclamation; interleaved starts require a separate ordering protocol.

Pair parsing shares the existing 1 MiB per-pass budget with completed-item
compaction. If the remaining budget cannot fit a pair, retry it next pass without
advancing the scan beyond it. As before, one bounded candidate can be materialized
before the budget check. Start deletion and projection revision changes commit
atomically with other retention work. A stale transcript/activity continuation
must reload. Keep existing activity index identities and group anchors: they denote
original ordering, and expanded pages can serve the retained completion.

Tests compare actual desktop projections before/after, retained activity groups,
full replay and transcript pages, including adjacent and interleaved tools.
Additional storage tests protect unsupported/large/partial snapshots, age, ACKs,
active sessions, questions and the newest turns. No live user journal or installed
client is modified by these tests. Configurable budgets, media and Storage UI remain
separate #128 work.
