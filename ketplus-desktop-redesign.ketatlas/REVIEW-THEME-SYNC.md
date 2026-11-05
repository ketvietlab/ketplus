# KetAtlas 0.5.0 theme integration — 2026-09-29

The upgrade starts from desktop `origin/develop` at `315b456`. The global CLI,
desktop install command and documented viewer commands now use KetAtlas 0.5.0.
The product's Két Design System 0.1.7 assets and adapter lock are unchanged.

All ten registered screens load one shared bridge in the document head. Viewer
Light/Dark messages update semantic tokens and native form color schemes in
place. Disabling sync restores the screen's product preference, including live
system appearance. Saving settings and following links retain product preferences
without persisting the viewer's temporary override. No sandbox permissions or
workflow IDs changed.

## Automated checks

- Node/jsdom suite: 220 tests, covering every declared route plus all ten screens'
  theme restoration, DOM identity, draft/attachment preservation, new routes,
  invalid message/sender rejection, system changes and settings persistence.
- `ketatlas validate` and `ketatlas audit --strict`: 13 flows, 10 screens, no
  warnings or errors. The workflow schema remains version 1.
- Focused `ketplus_atlas_tests`: passed, including the 0.5.0 install command.
- The public CM gitlink and generated design-system assets are unchanged.

## Browser limits

KetPlus browser MCP loaded the 0.5.0 viewer and observed its Light, Dark and
Sync screen theme controls. Observation reported 366 subframes for this bundle.
Full observations were slow, and screenshot capture did not complete before
the verification session was closed. End-to-end theme switching in both canvas
and interactive previews, contrast and screenshot evidence remain unverified.
The tool also omits iframe descendants from actionable nodes. All browser
sessions created for this check were explicitly closed and closure was confirmed.

No native Qt visual acceptance or browser end-to-end approval is claimed. Existing
native progress statuses and blockers remain unchanged; the new evidence records
only the deterministic mock checks above.
