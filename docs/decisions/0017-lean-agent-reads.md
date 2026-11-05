# 0017: Lean agent reads

Status: accepted
Date: 2026-09-29

## Context

Agents fill their context window with what tools print. Two patterns dominated
long sessions: every `browser_observe` returned computed styles for up to 300
nodes (roughly 150–200 KB of JSON on a real page, sent twice by MCP as text and
structured content), and agents read repositories with whole-file `cat`, several
files per command, and printed tool lists. Context filled early and providers
compacted repeatedly.

## Decision

- `browser.observe` accepts `detail: "summary" | "full"` and defaults to
  `summary`. A summary leaves out computed `styles` and `clipped`, clips node text
  to 160 characters, defaults to 150 nodes and 6,000 ARIA characters, and stops
  adding nodes once the reply reaches about 32 KB, reporting `truncated`.
  `detail: "full"` keeps the previous behaviour.
- The service stores the full snapshot either way. Refs of nodes left out of a
  summary stay valid, and `visual.compare` still sees computed styles.
- KetPlus adds a `[KetPlus reading policy]` block after the browser policy in the
  instructions it gives every agent (Codex developer instructions, Claude system
  prompt append, Cursor leading prompt block). It asks agents to search with `rg`
  before reading, read only the line ranges they need, not `cat` several files
  at once, not print tool lists or schemas, cap long output, and request a full
  browser observation only when the summary lacks what the task needs. It is sent
  whether or not the browser is available.
- Screenshots (added 2026-10-08). An MCP capture arrives inline as a base64 PNG
  of a few hundred KB, and the KetPlus transcript already shows it. One agent,
  asked by a design brief for a screenshot per state, viewport and theme,
  copied that base64 into `.b64` files in the repository. A single Codex
  `turn/diff/updated` message then reached 18 MiB, and KetPlus dropped it. The
  KetPlus browser policy and the `ketplus-browser` skill now say:
  - check state and layout by measurement (`observe` bounds, `browser_inspect`)
    before capturing;
  - capture one image per distinct state that needs a visual check, about five
    per turn unless the user asks for more;
  - for a matrix, capture only failing cells;
  - never write image data into files;
  - record evidence as text;
  - ask the user when screenshot files must be saved in the project.

  The MCP server instructions, which must stay under 3 KB, carry only the short
  form: about five captures per turn, and never write image data into files.

## Consequences

The guidance lives in KetPlus rather than in each repository's `AGENTS.md`, so
it applies to every workspace. Agents that need styles or every node must ask for
them explicitly. Callers that read `styles` from default observations must pass
`detail: "full"`.
