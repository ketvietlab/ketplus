# ADR 0098: Generated-image pixel retention

Status: accepted; incremental part of #128.

## Context

Older Codex builds journaled a generated image inline: the completed
`imageGeneration` item carried the PNG as a base64 `result` string. On one
measured journal, 422 such items held 1.17 GB in `large_event_payload_chunks`.
Newer daemons move these pixels into the media store before journaling
(`McpMediaStore::externalizeItem`), so this is legacy data.

Clients never draw the inline string. Desktop reads only a `result` object with
media references, and otherwise falls back to `savedPath`. The phone reads media
references. The bytes take space and show nothing.

The replay-detail pass could not reach them. Its large-payload lane reads at most
1 MiB per pass, and it read at most 9 chunks of a payload. A typical image
payload is 2–4 MB.

## Decision

- `compactCompletedReplayItem` removes an `imageGeneration` item's `result` when
  it is a non-empty string.
  - It keeps `id`, `type`, `status`, `revisedPrompt`, `savedPath` and all other
    fields.
  - It adds `item.compacted` with policy `image-pixels-v1`, `original_bytes`
    (the base64 length) and `retained_bytes: 0`.
  - An item with a media-reference `result`, no `result`, or an existing marker
    is left alone. The transformation is idempotent.
- The detail pass has a separate image lane for large-event manifests whose
  `item_type` is `imageGeneration` and whose payload is at most 16 MiB.
  - It reads the whole payload, verifies its digest, compacts it and stores the
    small result inline. It then drops the transfer's payload and chunks in the
    same transaction.
  - It takes at most one image per pass and does not use the shared 1 MiB
    parsing budget. A second eligible image ends the pass, and the next pass
    takes it.
  - Larger payloads stay.
- `boundedLargePayload` reads the manifest's own chunk count instead of a fixed
  9. Any surplus chunk still fails validation.
- Inline (non-chunked) legacy image items go through the ordinary candidate
  lane.

The same eligibility applies as for other detail:
- the turn is past the full-detail period and completed;
- the session is idle;
- there are at least 20 newer turns;
- no raw rows await acknowledgement (for the large lane, in the whole session);
- the turn has no question or approval.

Forever/off keeps everything.

## Consequences

- Each idle minute reclaims at most one legacy image, so the measured 422 images
  take about seven idle hours. Free pages return through incremental vacuum.
- No transcript change: the row still shows its prompt and saved path. A deleted
  saved file shows no thumbnail, as before, since the inline bytes were never
  drawn.
- Not covered: a preview image for compacted items, and pixels inside
  `mcpToolCall` results stored before media externalization. Both remain part
  of #128.
