# ADR 0101: Previews for compacted images

Status: accepted; incremental part of #128.

## Context

ADR 0098 drops the inline base64 pixels of older Codex `imageGeneration` items
once history retention reaches them. The row then shows only its prompt and
saved path, and once the saved file is deleted it has no picture at all. The
#128 plan keeps "a 512 px JPEG preview (setting), or caption only".

Newer daemons already journal these items with a `result.content` media
reference (`McpMediaStore::externalizeItem`), and Desktop and the phone draw
that reference. The media store already encodes a bounded JPEG preview of any
image (`McpMediaStore::encodePreview`: at most the requested side, at most
192 KB, transparency flattened onto white).

## Decision

- `compactCompletedReplayItem` takes an optional `ImagePreview` callback. For a
  generated image with inline pixels:
  - It decodes the base64, accepting a `data:` URL prefix, and rejects
    malformed base64 before calling the callback.
  - When the callback returns a reference block, `result` becomes
    `{"content": [block]}`, which is the shape a newer build journals. The
    marker policy is `image-preview-v1`. `retained_bytes` is the size of the
    reference kept in the journal; the JPEG itself lives in the media store.
  - Otherwise the item is compacted as before (`image-pixels-v1`, caption only).
- Tool results (ADR 0099) use the same callback. In `mcpToolCall` and
  `dynamicToolCall` items, an inline `result.content` image becomes a media
  reference block in its place, up to four per call (`maximumImagesPerCall`,
  what clients draw). Further images, and every `contentItems` `inputImage`
  (which no client draws), stay `history_retention` placeholders. The
  `inline_images` marker keeps policy `tool-inline-images-v1` and gains
  `previews` (the count kept), and `retained_bytes` is the size of the kept
  references.
- `LocalJournal::setImagePreviewWriter` supplies the callback per session.
  `SessionHost` installs one that encodes a 512 px JPEG and stores it with
  `McpMediaStore::storeImage` under the item's session. It clears the writer in
  its destructor, because the journal outlives the host.
- Previews are content-addressed, so identical pictures share one file, and
  they are released with their session like any other media.
- Each detail pass lets at most one item decode previews (a tool item decodes
  its own images together, at most four), whether it is inline or in the large
  lane. Decoding costs the same whatever the stored size:
  a 65 KB PNG can describe 64 megapixels. A second item that needs a preview
  ends the pass, and the next pass takes it. Caption-only compaction keeps its
  existing limits.
- Eligibility, the one-image-per-pass large lane and every other rule of
  ADR 0098 are unchanged.

## Consequences

- A compacted generated image costs at most 192 KB in the media store instead
  of its full inline PNG (2–4 MB typically) in the journal.
- If the detail pass's transaction fails after a preview is stored, the preview
  stays referenced by its session until the session is deleted. It is never
  shown, and the next pass stores the same content-addressed file again.
- `retained_bytes` counts the reference kept in the journal; the JPEG's own
  size is in the block's `bytes` and is accounted for by the media store.
- The "caption only" choice is currently made by leaving the writer unset. The
  user setting belongs to Settings → Storage in the new-UI stream.
