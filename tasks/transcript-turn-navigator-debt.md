# Deferred: transcript turn navigator

Status: deferred by user; do not implement before transcript performance work.

Intent: a quiet rail of turn markers with a lightweight prompt/response preview
and explicit navigation to historical turns, inspired by the supplied reference.
The screenshot alone does not establish the reference application's interaction
or implementation details.

Prerequisites: bounded initial transcript window, lossless older/around paging,
stable turn/item identities, bounded data/layout caches and stable scroll anchors.

Future acceptance:
- Markers represent logical turns, not tokens or individual tool events.
- Reading position and hovered/selected marker are distinct states.
- Preview uses bounded metadata; no full transcript parse or model-generated summary.
- Clicking an unloaded turn loads a bounded window around its stable identity.
- Dense histories use grouped markers; no per-event QML object allocation.
- Preserve reading position, keyboard accessibility and reduced-motion behavior.
- Do not introduce transcript reflow or auto-scroll on hover.

No navigator UI, animation, endpoint or schema is enabled by this debt entry.
