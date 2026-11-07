# ADR 0073: Decode oversized provider routing strings

Status: accepted for captured oversized-line metadata; full provider framing and request semantics remain under audit.

The oversized Codex scanner removed escape backslashes and decoded tokens with
fromUtf8. Valid escaped keys/values, Unicode escapes and surrogate pairs could
route differently from the equivalent normal JSON message. Invalid UTF-8 became
replacement characters, and literal dotted keys could alias nested paths. The
original focused fixture reproduces 24 failures across three input chunk sizes;
14 valid/refusal checks pass.

Keep bounded raw token bytes, validate JSON escape spelling and unescaped controls,
then use Qt's JSON string decoder for Unicode. Explicit lexical validation is
needed because local Qt 6.11.2 accepts some unknown escapes and raw controls.
The token cap remains 1 KiB of encoded bytes. Overlong unknown keys cannot name a
requested path and are ignored. A malformed captured key or invalid/overlong
requested string clears all routing fields for that line and prevents later fields
from repopulating them. This avoids combining a missing invalid method with an
otherwise valid request id. Clear/reset and the next line restore normal scanning.

Resolve paths structurally: a decoded key containing a dot cannot stand for a
nested path segment. The ordinary 64-frame/deeper-depth bound from ADR 0070 remains.
Normal-size lines and the public oversized notification shape are unchanged.
Withheld metadata does not invent a target or claim a pending request completed.
Request timeout/recovery and type validation remain H3.4 work.

The expanded fixture passes 91/91, including the previous nesting/memory suite,
escaped keys and values, Unicode, invalid UTF-8/escapes/controls, dotted-key aliases,
field limits, invalid metadata before/after a numeric id and valid subsequent lines.
MOC sees slot declarations; raw JSON fixture bodies are kept out of its parser.
The private state added to the header requires a full Pro build/test gate at the
final head and independent review. No UI implementation or live provider is used.

This is selective metadata extraction, not validation of every byte of an oversized
JSON document. Duplicate keys, malformed outer structure, RPC id/method typing,
CPU/time budgets and global I/O bounds remain separate. The POSIX memory probe
still skips on other platforms; Windows execution, install and deployment are not
claimed. This change depends on the bounded scanner package (#256).
