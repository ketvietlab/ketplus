# Mermaid appearance

Diagrams look as they do on mermaid.live. `resources/mermaid/light.json` and
`dark.json` select what Mermaid itself ships, so the same diagram reads the same in
KétPlus and in the browser:

- `layout: elk`: orthogonal edges and grouped subgraphs. mermaid-cli registers the
  ELK layout package it installs with Mermaid; no extra install is needed.
- `look: neo` with the `redux` (light) or `redux-dark` theme: square white- or
  dark-bordered nodes, boxed edge labels and outlined subgraphs.
- KÉT Sans (then Inter), so the widths Mermaid measures in its browser match the font
  Qt draws with.

The configuration keeps the security settings: strict security level, SVG text labels
instead of HTML (`htmlLabels: false`, since Qt SVG does not draw `<foreignObject>`),
the text and edge limits, and a `secure` list that stops a diagram's front matter or
init directive from changing theme, look, layout or styles.

## Qt SVG compatibility

`MermaidRenderer` runs `mmdc` and rewrites each SVG once, before it is cached, for what
Qt SVG reads differently from a browser:

- `!important` is dropped from colours, which Qt SVG would otherwise paint black.
- `hsl()` and `hsla()` stylesheet colours, which the redux themes use, become hex.
  Qt SVG has no alpha for them, so a translucent colour becomes opaque.
- Stylesheets move to the start of the SVG root, because Qt styles only elements
  parsed after a stylesheet.
- Label rows (`text-outer-tspan`) are flattened to positioned `<text>` rows.
- State labels get an explicit fill from the Két `text-main` token.

Qt SVG does not draw the neo look's drop-shadow filter; diagrams are otherwise drawn
as in the browser. Changing the configuration or this rewrite bumps the cache version
in `MermaidRenderer.cpp`, so diagrams cached under the old look are rendered again.

## Verification

- `ketplus_preview_tests` checks the configuration handed to `mmdc`, hex
  conversion of `hsl()` colours with Qt SVG pixel checks, and that rectangle corners
  are left as Mermaid wrote them. `ketplus_scheme_rendering_tests` checks that the cache
  key follows the scheme's main text colour, the one colour the app still supplies.
- A flowchart with subgraphs and edge labels, a state, a sequence and a class diagram
  were rendered with the real `mmdc` in both themes and drawn through Qt SVG after the
  rewrite above; they match the browser rendering except for drop shadows.
