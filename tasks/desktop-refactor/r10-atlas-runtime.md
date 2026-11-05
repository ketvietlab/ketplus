# R10 Atlas runtime handoff

## Implemented slice

- Library cards now include asynchronously generated preview artwork. Work is
  limited to two workers, at most 32 queued requests, visible/prefetch cards only,
  and a 64 MiB in-memory LRU. A card never starts KetAtlas or a WebView.
- Builder preview owns the full canvas. AI Chat is a right-inset overlay capped
  at 400 px; hiding it does not resize or recreate the preview.
- Bundle path, Builder conversation ID and KetAtlas flow ID are kept separately.
  Prompts receive a sanitized relative bundle path and `@Flow` ID, explicitly as
  preferred context rather than an isolation promise.
- A present but invalid manifest keeps and labels the last valid preview. A
  physically missing manifest enters explicit recovery after a debounce and can
  return to the library without discarding the Builder conversation.
- Missing CLI remains an explicit user-triggered locate/install/retry state.

## Verification and remaining visual gate

Focused Qt tests cover indexing metadata, lazy thumbnail population, worker and
cache bounds, invalid-versus-missing bundle recovery, overlay geometry, flow
identity, prompt sanitization, and Builder continuity. The normal offscreen lane
cannot prove native WebView stacking, focus, trackpad pan/zoom, or raster parity.
Those remain in `KETPLUS_NATIVE_ATLAS_TEST` and the design-evidence harness.

Pixel acceptance is a raster mismatch of at most 5% for every specified
screen/state/viewport/theme. Structural omissions, clipping and incorrect
interaction fail regardless of the percentage. No visual approval is claimed by
this slice because native capture permission is unavailable.
