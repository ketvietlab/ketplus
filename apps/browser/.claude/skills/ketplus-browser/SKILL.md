---
name: ketplus-browser
description: Drive, inspect and visually compare web pages through the KetPlus canonical browser API (apps/browser, protocol browser.v0) instead of raw Playwright or ad-hoc screenshots. Use this whenever a task needs a real browser on a local or allowed URL — opening a dev server page, clicking or filling a form, checking what a UI actually renders, capturing screenshot evidence, verifying a UI change, or comparing a candidate page against a reference/Atlas design (pixel-perfect or visual-regression checks, sidebar exclusion) — even if the user never names the browser API. Also use it when writing scripts, tests or integrations that call browser.v0 over MCP (browser_* tools) or JSON-RPC.
---

# KetPlus browser API

`apps/browser` is a standalone browser worker. The model never touches
Playwright: it calls a small canonical protocol (`browser.v0`) that observes a
page as semantic nodes, acts only on nodes from the latest observation, and
compares pages using semantics, geometry, computed styles and pixels together.

It is a stdio process, not an HTTP service. Something must start it: an MCP
host, a script, or `npm run compare`.

## Browser routing inside KetPlus

Use the host-supplied `ketplus_browser` MCP tools for real-browser tasks without
waiting for an explicit skill mention. Do not search for a provider-native
browser, another browser MCP, raw Playwright/Puppeteer, or computer use on
another browser. If the tools are absent, disabled, fail, or cannot support the
task, report the limitation instead of switching tools or starting an ad-hoc
browser script. Host origin denials must never be bypassed, including through
another browser.

Computer use (desktop screen and input control) is allowed in two cases:

1. **Safari.** `ketplus_browser` drives Chromium only. When a task specifically
   needs Safari or WebKit (Safari-only behaviour, rendering or bugs, or the user
   asks for Safari), drive Safari with computer use and say why.
2. **Native desktop applications** that are not web browsers.

Never use computer use to drive Chrome, Edge, Firefox or another browser in
place of `ketplus_browser`.

The standalone interfaces below are for explicit browser-protocol development or
CI outside a KetPlus agent session; they are not fallbacks for unavailable tools.

## Choose the interface

| Situation | Use |
|---|---|
| `browser_*` / `visual_compare` tools are available in this session | The MCP tools directly. No code. |
| Quick verdict for two whole-page URLs | `npm run compare -- <referenceUrl> <candidateUrl>` in `apps/browser` |
| Explicit standalone protocol flow or CI outside a KetPlus agent session | A Node script using `scripts/browser-rpc.mjs` (below) |

MCP tool names map one-to-one to protocol methods: `browser_capabilities`,
`browser_create_session`, `browser_session_request`, `browser_pages`,
`browser_open`, `browser_close_page`, `browser_observe`, `browser_act`,
`browser_wait`, `browser_logs`, `browser_network`, `browser_inspect`,
`browser_capture`, `visual_compare`, `browser_close_session`.
`browser_close_all_sessions` is MCP-only: it closes every session the server
opened, and KetPlus calls it when a turn ends.
Parameters are identical to the JSON-RPC params in `references/protocol.md`.

## Setup

In packaged KetPlus the runtime is bundled (`KetPlus.app/Contents/Resources/ketplus-browser`,
with its own `bin/node`); nothing needs installing. In a checkout:

```sh
cd apps/browser
npm ci
```

The server uses the installed Google Chrome, else Microsoft Edge, else downloads
Chrome for Testing once into the KetPlus data directory. `BROWSER_UNAVAILABLE`
means none of these worked (offline with no Chrome/Edge, or
`KETPLUS_BROWSER_ENGINE` names a browser that is not installed): tell the user to
install Chrome or retry online; do not try to install browsers yourself.
`KETPLUS_BROWSER_ENGINE=playwright` (after `npm run install:browser`) uses
Playwright’s Chromium instead.

If `npm` is broken in the shell, run the same through
`node "$(dirname "$(which node)")/../lib/node_modules/npm/bin/npm-cli.js" ci`.
Tests: `node --test` in `apps/browser`.

To register the MCP server for an agent host (absolute path; the origin list is
the host ceiling, see below):

```sh
claude mcp add ketplus-browser -e KETPLUS_BROWSER_ALLOWED_ORIGINS=http://127.0.0.1:3000 -- node /abs/path/apps/browser/src/mcp.mjs
```

## Origin policy comes from the host

`KETPLUS_BROWSER_ALLOWED_ORIGINS` (comma-separated origins, `loopback`, or `*`;
default `loopback`) is the ceiling for the whole server process. A session's
`allowedOrigins` can only narrow it. Every network hop — redirects, images,
frames, fetch, WebSockets, popups — goes through a per-session proxy that
enforces it.

So when you hit `NAVIGATION_DENIED` or `ORIGIN_NOT_PERMITTED`, the fix is a
host decision: tell the user which origin is needed and how to set the
variable. Do not try `"*"` from inside a session; it is rejected by design, and
widening access is exactly what the policy exists to stop.

Narrow sessions to what the task needs (`allowedOrigins: ["http://127.0.0.1:3000"]`):
a page that redirects or embeds something unexpected then fails loudly instead
of silently loading it.

## The core loop: observe → act → observe

1. `browser.session.create` — pick the viewport and theme you mean to verify.
2. `browser.open` — returns `pageId`.
3. `browser.observe` — returns `snapshotId` and `nodes`. Each node has a `ref`
   such as `snapshot-12:n7`, plus `role`, `name`, `states`, `stableId`
   (`data-canonical-id`, `data-testid` or DOM id), `text` and `bounds`. Computed
   `styles` and `clipped` come only with `detail: "full"`.
4. Choose the target by meaning: `stableId` first, then `role` + `name`, and
   check `states` (for example `enabled`, `checked`, `expanded`).
5. `browser.act` with `expectedSnapshotId` set to that `snapshotId` and
   `action.target.nodeRef` set to the node's `ref`.
6. Observe again before the next node action.
7. Close every session you created with `browser_close_session` when browser work
   ends, before your final response (see mandatory cleanup below).

Why the ceremony: a ref is bound to one snapshot. Every action — even one that
fails or times out — invalidates the snapshot, because the page may already
have changed. Before acting, the service re-checks the element's tag, role and
name, so a list row that was recycled into different content fails with
`NODE_CHANGED` instead of receiving your click. Operations on one page are
serialized, so two concurrent actions cannot both use the same snapshot.

`navigate` and untargeted `scroll` need no snapshot. After a click, check
`openedPages` in the result: popups become separate pages (see `browser.pages`),
and a page whose `allowed` is `false` is outside the policy.

Prefer `browser.wait` over sleeping: `{type:"text", text:"Saved"}`,
`{type:"url", startsWith:"http://127.0.0.1:3000/done"}`, `{type:"load",
state:"networkidle"}`, or `{type:"settled"}` (fonts plus two animation frames;
actions already settle by default).

## Recover from errors

Errors carry a stable string code (MCP: `structuredContent.code`; JSON-RPC:
`error.data.code`, with `error.data.details`).

| Code | Meaning | Do |
|---|---|---|
| `STALE_SNAPSHOT` | Page changed (action, navigation) since that snapshot | Observe again, re-select the node |
| `NODE_CHANGED` | The element no longer has the observed tag/role/name | Observe again; the UI moved under you |
| `NODE_NOT_FOUND` | Ref not in the latest snapshot | Use a ref from the snapshot you pass |
| `ACTION_FAILED`, `TIMEOUT` | Engine could not perform the action | Observe again (the snapshot is gone), check `states`, retry once |
| `NAVIGATION_DENIED`, `REQUEST_DENIED` | URL or final page outside the session policy | Ask the user to allow the origin |
| `ORIGIN_NOT_PERMITTED` | Session asked for more than the host ceiling | Same; do not widen from the session |
| `EVIDENCE_MISMATCH` | Capture and snapshot come from different page states | Take both with one `observe(includeScreenshot:true)` |
| `INVALID_PARAMS` | Schema violation (`details.issues` lists paths) | Fix the parameters |
| `BROWSER_UNAVAILABLE` | No usable Chrome, Edge or Chrome for Testing | Tell the user what the message names; do not retry in a loop |

The full list is in `references/protocol.md`.

## Mandatory cleanup

Track every `sessionId` you create. You MUST call `browser_close_session` for each
one as soon as browser work ends and before your final response. This applies to
successful, failed, blocked and abandoned work. Close unused pages promptly;
closing a page alone does not close its session.

Preserve any evidence needed for the deliverable before closing: session closure
deletes its captures. Verify each close succeeds. If cleanup fails or tools become
unavailable, report the unclosed session IDs; do not claim they were closed.
In a KetPlus agent session the host closes any session still open when the turn
ends, so none carries into the next turn; outside KetPlus, only leave a session
open if the user explicitly requests it. Do not close
user-owned or other agents' sessions, and do not kill shared browser processes.
For standalone scripts using an owned worker, keep cleanup in `finally` as in the
example below.

## Keep the output small

Every observation lands in your context (MCP sends it twice: text and
structured). The default `detail: "summary"` keeps it under about 32 KB: it
leaves out computed styles, clips node text to 160 characters, the ARIA
snapshot to 6,000 and nodes to 150, and stops adding nodes at the byte budget
(`truncated` is then `true`). Refs of nodes left out stay valid.
`detail: "full"` returns computed styles and every node up to `maxNodes`; a
real page is then roughly 150–200 KB, so ask for it only when the summary
lacks what the task needs.

- Scope with `rootSelector` (standard CSS only, for example `main` or
  `[data-testid='settings-form']`).
- Lower `maxNodes` (default 150, or 300 with `detail: "full"`) to what you need; `truncated` and
  `visibleNodeCount` tell you what was left out.
- Lower `maxAriaChars` (default 6,000, or 20,000 with `detail: "full"`) when you act from `nodes` only.
- For comparisons, read `status` and `summary` before `findings`; cap with
  `policy.maxFindings`.
- Close pages and sessions you no longer need; closing a session also deletes
  its captures.

## Screenshots and evidence

A capture is a PNG of a few hundred KB that lands in your context and, through
MCP, inline in the KetPlus transcript where the user already sees it.

- **Measure before you capture.** State, text, focus, layout and spacing are
  checked from `browser_observe` (`states`, `bounds`) and `browser_inspect`
  (`computedStyle:<property>`, `bounds`, `scroll`). Capture only what needs
  a look at the pixels.
- **Capture sparingly.** Take one screenshot per distinct page state that needs
  visual review, never after every action. When a brief asks for a
  state × viewport × theme matrix, verify every cell with observe/inspect (or
  `visual_compare`) and capture only the cells that fail or that the user must
  see. Keep to about five images per turn unless the user asks for more.
- **Reuse the real PNG file.** `browser_capture` returns `path` and
  `browser_observe` with `includeScreenshot: true` returns `capturePath`, alongside
  the image shown in the transcript. These are original local PNGs written by the
  browser worker. Use or copy them as assets/evidence for the authorized task,
  respecting normal filesystem permissions; no extra confirmation is needed for
  an already authorized screenshot asset. Copy needed files into the project
  **before closing the session**: its temporary captures are deleted on close.
  `visual_compare` also returns `raster.diffPath` when it produces a diff PNG.
- **Keep image bytes out of text.** Do not rebuild PNGs from base64/data URIs in
  tool output, create `.b64` files or print binary image data. Copy the returned
  original file instead. If an older worker returns no path, report that its
  runtime needs updating; do not invent a path or reconstruct image blocks.
- **Record evidence as text.** Give the URL, viewport, theme, state,
  `snapshotId`/`captureId`, measurements and `visual_compare` findings. When using
  a screenshot asset, record the destination and SHA256 from the capture metadata.
  Keep browser-session cleanup mandatory even when an asset is retained.

## Visual comparison recipe

Both sides must share an environment (viewport, device scale factor, theme,
locale, reduced motion, browser engine and version); otherwise `status` is `blocked` with
`environmentMismatch`. Fix the sessions rather than loosening the policy.

```text
ref  = session.create({viewport, theme, allowedOrigins:[referenceOrigin]})
cand = session.create({same environment, allowedOrigins:[candidateOrigin]})
open both → observe each with includeScreenshot:true (and the same rootSelector)
visual.compare({
  reference: {snapshotId, captureId},   // from the reference observation
  candidate: {snapshotId, captureId},
  policy: {...}
})
```

Take the screenshot in the same `observe` call. A separate `capture` after any
action or navigation belongs to a different page state and is refused.

Reading the result:

- `passed` — no structural errors and `raster.mismatchRatio` within
  `maxPixelMismatchRatio` (default 0.05).
- `failed` — look at `summary.byKind`, then `findings` (errors first). Kinds:
  `accessibility-tree` (has `firstDifference`), `missing-element`,
  `extra-element` (warning), `semantic`, `text`, `geometry` (`delta` in CSS px,
  tolerance `geometryTolerance`, default 1), `clipping`, `style` (`property`,
  `expected`, `actual`).
- A good raster score never overrides structural errors; report both.

Excluding application chrome such as a sidebar:

- Same layout width on both pages: observe with `rootSelector` for the product
  surface (geometry becomes relative to that root) and add
  `policy.ignoredRegions: [{x:0, y:0, width:<sidebar px>, height:<viewport px>}]`
  for the pixels.
- Different sidebar widths: crop equal-sized content surfaces with
  `policy.rasterRegions: {reference:{x:<refSidebar>, y:0, width:W, height:H},
  candidate:{x:<candSidebar>, y:0, width:W, height:H}}`. If the viewports were
  sized so both content areas have the same width, also set
  `allowViewportWidthMismatch: true` (only honored with equal regions and equal
  viewport heights). `ignoredRegions` are then relative to the cropped surface.
- To find the chrome width, observe without `rootSelector` and read the
  `bounds` of the `complementary`/`navigation` or `main` node.

All regions are CSS pixels; the service scales them by `deviceScaleFactor`.
Nodes sharing a role and name are paired in document order at confidence 0.5;
set `pairRepeatedGroups: false` to leave such groups unmatched instead.

## Scripted use (JSON-RPC)

`scripts/browser-rpc.mjs` starts `src/cli.mjs`, correlates responses and maps
errors. Import it by absolute path from a script anywhere (for example a
scratch directory):

```js
import { connect } from "/abs/path/apps/browser/.claude/skills/ketplus-browser/scripts/browser-rpc.mjs";

const origin = "http://127.0.0.1:3000";
const browser = await connect({ allowedOrigins: [origin] });
try {
  const { sessionId } = await browser.call("browser.session.create", {
    viewport: { width: 1440, height: 900 },
    allowedOrigins: [origin],
  });
  const { pageId } = await browser.call("browser.open", { sessionId, url: `${origin}/login` });
  let snapshot = await browser.call("browser.observe", { sessionId, pageId, rootSelector: "main", maxNodes: 80 });
  const email = snapshot.nodes.find((node) => node.role === "textbox" && node.name === "Email");
  await browser.call("browser.act", {
    sessionId, pageId, expectedSnapshotId: snapshot.snapshotId,
    action: { type: "fill", target: { nodeRef: email.ref }, value: "agent@example.test" },
  });
  snapshot = await browser.call("browser.observe", { sessionId, pageId, rootSelector: "main", maxNodes: 80 });
  const submit = snapshot.nodes.find((node) => node.role === "button" && node.name === "Sign in");
  await browser.call("browser.act", {
    sessionId, pageId, expectedSnapshotId: snapshot.snapshotId,
    action: { type: "click", target: { nodeRef: submit.ref } },
  });
  await browser.call("browser.wait", { sessionId, pageId, condition: { type: "text", text: "Welcome" } });
} catch (error) {
  console.error(error.code, error.message, error.details ?? "");
  process.exitCode = 1;
} finally {
  await browser.close();
}
```

`connect()` options: `allowedOrigins` (host ceiling for this process),
`artifactsDir`, `env`. Thrown errors have `code` (canonical string), `rpcCode`
and `details`. For a one-off call: `node scripts/browser-rpc.mjs
browser.protocol.schema` prints the JSON Schema of every method.

Captures and diffs are files under the artifacts directory (JSON-RPC results
include `path`; MCP results never do, and include the image inline instead; see
"Screenshots and evidence"). They can contain authenticated page content: do
not commit or upload them.

## What it cannot do (do not work around it)

- No arbitrary JavaScript, file upload, downloads, or cookie/storage access.
- `browser.session.request` returns only status and content type, never
  headers or bodies. Its credentials are still tool arguments, so avoid putting
  real secrets there unless the user asked for it.
- JavaScript dialogs are auto-dismissed (`confirm()` returns false);
  `dialogsDismissed` counts them.
- Subframe content is not in `nodes` (`subframeCount` says how many frames were
  skipped). Captures are viewport-only: scroll, then observe with a screenshot.
- Role and name are heuristics close to accname, not the browser's
  accessibility tree. Chromium only.

When the task genuinely needs one of these, say so instead of reaching for raw
Playwright; bypassing the service also bypasses its origin and evidence rules.
