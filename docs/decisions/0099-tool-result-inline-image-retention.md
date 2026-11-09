# ADR 0099: Tool-result inline image retention

Status: accepted; incremental part of #128. Amends ADR 0039 and ADR 0044, which
kept media blocks unchanged until a media policy existed.

## Context

Before MCP media moved to the media store, `mcpToolCall` results were journaled
with base64 `image` content blocks. Browser screenshots are a common case.
`dynamicToolCall` items stored images the same way, as `inputImage` items with a
`data:` URL. These blocks are the largest part of old tool rows.

The text, argument and structured-result policies leave media blocks alone. A
payload of more than 1 MiB never reaches any lane (ADR 0047).

#128 decided that, past the full-detail period, a tool call keeps its tool,
arguments, status, error and a text preview, but loses large result bodies and
inline images.

## Decision

- After the existing tool policies, `compactCompletedReplayItem` rewrites
  completed or failed `mcpToolCall` and `dynamicToolCall` items:
  - each `result.content` block of `type: "image"` with a non-empty `data`
    string;
  - each `contentItems` entry of `type: "inputImage"` whose `imageUrl` is a
    `data:` URL.
- Each such block becomes
  `{type, omitted: true, reason: "history_retention", original_bytes, mimeType?}`
  in the same position.
  - Blocks with a `media_ref` (the current form), remote image URLs and every
    other block are unchanged.
- The marker is `compacted.inline_images`, with policy `tool-inline-images-v1`.
  It sits beside an existing text, argument or structured-result marker, or
  becomes the marker when there is none. Items with an unknown policy are left
  alone, and the change is idempotent.
- Selection for inline rows: a row already trimmed by an earlier text policy is
  selected again when it has no `inline_images` marker and its payload contains
  an image block. This is a textual prefilter; the compactor decides.
- Selection for large payloads: the one-per-pass image lane (ADR 0098) also takes
  `mcpToolCall` and `dynamicToolCall` manifests of at most 16 MiB, but only when
  one of their chunks contains an image block (SQLite `instr`, no hydration).
  Larger text-only results still stay, as ADR 0047 requires.
- Desktop: `mcp_media::materializeImages` turns a `history_retention` omitted
  block into the same unavailable placeholder used for pruned media. The
  gallery keeps the image's slot instead of silently losing it.

The eligibility rules are unchanged (ADR 0038). Forever/off keeps everything.

## Consequences

- Old screenshots in turns past the full-detail period show as unavailable
  placeholders. Their tool name, arguments, text and status remain.
- A dedicated "trimmed by history retention" label is left to the new
  transcript UI (#160). The marker and `original_bytes` give it what it needs.
- An image block split exactly across two chunks can miss the prefilter. That
  payload then simply stays.
