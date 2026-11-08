# ADR 0070: Bound oversized provider scan depth

Status: accepted for the Codex oversized-line scanner; broader provider framing remains under audit.

ProviderLineReader discards lines over the configured byte limit while extracting
shallow routing fields so Codex can finish a skipped turn without stopping every
conversation. The scanner said it only inspected 64 container levels, but kept
appending placeholder frames below that depth. A stream of nested brackets still
allocated memory proportional to its length. A fresh-child Release regression
measured 201,637,888 bytes of peak resident growth for two million nested arrays;
the routing behavior passed, so functional-only tests did not expose the bound.

Keep at most 64 routing frames. Count deeper containers with a scalar depth;
ignore their keys, scalars and punctuation while continuing to track quoted and
escaped strings. Once that depth unwinds, resume the outer routing stack.
Clear/reset also clears the skipped depth. Do not change the normal-line limit,
message routing, oversized notification policy or the shared provider process.

The fixture streams arrays and objects across 1/7/1024-byte chunks around the
scan boundary, preserves shallow fields after the nested value, ignores nested
spoofed fields, checks escaped brackets and the next line, and resets an unfinished
discard. An isolated child verifies peak resident growth stays below 16 MiB with
two million levels; the fixed local Release probe measured 147,456 bytes. This is
a generous resource-regression threshold, not a product-wide memory guarantee.
The memory assertion uses POSIX getrusage and explicitly skips on other platforms.

Focused checks pass 46/46. The shared header requires the full Pro build and test
matrix before merge, in addition to independent exact-head review. No live
provider, credentials, user journal, UI implementation, installation or deployment
is exercised. Aggregate process reads, CPU/time budgets, Claude/Cursor line bounds,
malformed/UTF-8 inputs and stderr/version-probe handling remain separate H3.1 work.
