# Canonical browser server

The browser.v0 server. It is not linked into KetPlus and does not read Atlas
manifests; a caller supplies ordinary HTTP(S) URLs. macOS Release builds bundle it
as a separate helper runtime (see [Bundled runtime](#bundled-runtime)).

The prototype validates three ideas:

1. Models act through a small canonical API rather than Playwright objects.
2. Every action targets a node reference from the latest observation, so stale
   references fail instead of being silently re-resolved to another element.
3. Browser-to-browser comparison combines semantic nodes, rendered geometry, selected computed
   styles, accessibility output, and raster evidence. A screenshot score cannot
   override structural findings.

For native Desktop fidelity review, `visual.compare.native` accepts a PNG made
by the native app in a private per-user temporary directory. It checks matching
viewport, display scale and theme, then reports raster mismatch only. Browser
and native screenshot pixels may differ because of font rasterization and window
chrome; crop matching content regions with `rasterRegions` before interpreting
the score. No arbitrary local file can be loaded through this API.

## Runtime

Node.js is an implementation choice, not part of the wire contract.
Playwright (`playwright-core`) controls a dedicated Chromium-family browser
process with a fresh temporary profile. A future broker can
be implemented in C++, Rust, or Go and retain the same JSON-RPC messages while
keeping this process as a replaceable browser worker.

`src/protocol.mjs` is the single source of the `browser.v0` contract. Every
transport goes through `BrowserService.dispatch`, which validates parameters
and limits before the engine sees them. `browser.protocol.schema` returns the
same contract as JSON Schema for non-Node implementations. Selectors are
standard CSS only; Playwright selector engines are rejected.

Install and verify:

```sh
cd apps/browser
npm ci
npm test
```

## Browser engine

No browser is bundled. At the first session the server picks, in order:

1. installed Google Chrome,
2. installed Microsoft Edge,
3. Chrome for Testing in `KETPLUS_BROWSER_CACHE` (default
   `~/Library/Application Support/Ketsuite/KetPlus/browser-runtime/chrome` on macOS,
   `$XDG_DATA_HOME/Ketsuite/KetPlus/browser-runtime/chrome` on Linux), downloaded on
   first need through Playwright’s registry (pinned revision; HTTPS from Playwright’s CDN, no archive checksum, used only once `INSTALLATION_COMPLETE` exists).

`KETPLUS_BROWSER_ENGINE` overrides the order: `auto` (default), `chrome`,
`msedge`, `bundled` (Chrome for Testing only), or `playwright` (Playwright’s own
Chromium from `npm run install:browser`, for development). An explicit engine never
falls back. `KETPLUS_BROWSER_DOWNLOAD=0` forbids the download. When no engine is
usable, `browser.session.create` fails with `BROWSER_UNAVAILABLE` and a message
naming what is missing. The user’s personal browser profile is never used.

`browser.capabilities.engine` reports `preference`, the resolved `name` and
`source`, and `browserVersion` after launch. Session environments include
`browserEngine` and `browserVersion`; comparisons require both to match.

Tests use the same selection; run them against Playwright’s Chromium with
`npm run install:browser` and `KETPLUS_BROWSER_ENGINE=playwright npm test`.

## Bundled runtime

`node scripts/package-runtime.mjs --out <dir> [--node-cache <dir>]` stages the
self-contained runtime that macOS Release builds copy into
`KetPlus.app/Contents/Resources/ketplus-browser` (CMake option
`KETPLUS_BUNDLE_BROWSER_RUNTIME`; `KETPLUS_BROWSER_RUNTIME_DIR` bundles a prebuilt
one). It contains the Node.js version pinned with its sha256 in
`runtime/node-runtime.json`, `src/`, production `node_modules` from `npm ci
--omit=dev`, the skill, third-party notices and `runtime.json`. `node
scripts/smoke-runtime.mjs <dir>` (`npm run smoke:runtime -- <dir>`) starts the
staged MCP server with the staged node, captures a local page and checks that a
public origin is refused; CI stages and smokes a runtime on every change.
`runtime.json` records `sourceSha256`, a digest of the package files and the
skill. CMake refuses a runtime whose digest differs from the checkout, unless
`KETPLUS_BROWSER_RUNTIME_ALLOW_STALE=ON` is set. It checks at configure time and
again before every bundle and install, since sources or the runtime can change
after configure. Install replaces the runtime embedded in the built app bundle
and checks the installed copy. See `docs/decisions/0009-bundled-browser-runtime.md`.

Start the newline-delimited JSON-RPC 2.0 server:

```sh
npm start
```

Each input line is one JSON-RPC request and each output line is one response.
Notifications receive no response. Errors use the standard integer codes
(`-32700`, `-32600`, `-32601`, `-32602`, and `-32000` for browser errors), with
the canonical string code in `error.data.code`. `server.shutdown` is a
transport method that drains in-flight requests and exits.

```json
{"jsonrpc":"2.0","id":1,"method":"browser.capabilities"}
```

Core methods are `browser.session.create`, `browser.session.request`,
`browser.session.close`, `browser.pages`, `browser.open`, `browser.page.close`,
`browser.observe`, `browser.act`, `browser.wait`, `browser.capture`, and
`visual.compare`.

Agents should load the `ketplus-browser` skill in
`.claude/skills/ketplus-browser/`. It covers interface selection, the
observe/act loop, error recovery, output budgets and comparison recipes, and
bundles `scripts/browser-rpc.mjs`, a JSON-RPC client for scripted flows.

For an Agent or MCP inspector, start the MCP facade instead:

```sh
npm run start:mcp
```

It exposes the same operations as `browser_capabilities`,
`browser_create_session`, `browser_session_request`, `browser_pages`,
`browser_open`, `browser_close_page`, `browser_observe`, `browser_act`,
`browser_wait`, `browser_logs`, `browser_network`, `browser_inspect`,
`browser_capture`, `visual_compare`, and `browser_close_session`. Screenshot-producing calls include a native MCP image
content block, up to `KETPLUS_BROWSER_MAX_IMAGE_PIXELS` (default 4,000,000).
The MCP facade returns the original local PNG as `browser_capture.path` or
`browser_observe.capturePath` when `includeScreenshot` is true; visual comparison
returns `raster.diffPath` for a diff PNG. Copy these files to an authorized project
asset/evidence destination before closing the session. Closing deletes temporary
captures, while copies outside the session directory remain. Image blocks still
render in the transcript; agents should reuse the file instead of rebuilding
image data from tool-output base64. Paths belong to the machine running the worker;
normal filesystem permission rules still apply.

## KetPlus agent integration

KetPlus injects this MCP server into Codex app-server, Claude Agent SDK queries,
and Cursor ACP sessions. All three use Settings → Browser and prefer the bundled
runtime over a source checkout. Claude and Cursor receive updated configuration
on their next turn, including resumed sessions. No user/project MCP config files
are modified. See [ADR 0011](../../docs/decisions/0011-browser-tools-for-all-providers.md).

`node src/check-engine.mjs` is the read-only Settings probe. It follows the same
engine selection without launching or downloading a browser. An available result
with `downloadRequired: true` means first use still needs a successful download.

## Origin policy

The host sets the origin ceiling with `KETPLUS_BROWSER_ALLOWED_ORIGINS`
(comma-separated origins, `loopback`, or `*`; default `loopback`). A session's
`allowedOrigins` may only narrow that ceiling, so a model cannot grant itself
new origins.

`*` means public websites and this computer. It never reaches the local
network (10/8, 172.16/12, 192.168/16, 100.64/10, fc00::/7), link-local
addresses (169.254/16 including cloud metadata, fe80::/10) or reserved ranges.
Loopback addresses need a loopback name (`localhost`, `127.x`, `::1`), so a
public name cannot rebind to a local service. A named origin may also reach the
private network (`https://staging.internal`), and an IP-literal origin
(`http://192.0.2.5:3000`) reaches exactly that address.

The proxy resolves each hop's host once, keeps only the addresses the policy
permits and connects to one of them, so a later DNS answer cannot move an
allowed connection elsewhere. The proxy listens on loopback without
credentials: it grants no access that a local process lacks, and Playwright
disables the HTTP cache for contexts that use proxy credentials.

Each session runs its browser context through a private local proxy. Every
network hop is checked there: top-level and frame navigations, each redirect
hop, subresources, `fetch`, WebSockets, popups, and `browser.session.request`.
Playwright route handlers are not used for enforcement because they do not see
redirect hops. WebRTC UDP is disabled because it would bypass the proxy.
Service workers and downloads are disabled. Pages that end up outside the
policy (for example a popup) are tracked but refuse observe, act, and capture.
`browser.pages` lists popups and the blocked requests.

`browser.session.request` supports login handshakes that are exposed only as an
HTTP endpoint. It shares the browser context's cookie jar, refuses redirects,
rejects `Cookie`, `Host`, and hop-by-hop headers, and returns neither response
headers nor response bodies. The credentials themselves are still tool
arguments, so a caller that must keep them out of a model transcript has to
perform this request on the host side.

## Observation and actions

`browser.observe` returns an ARIA snapshot and visible semantic nodes with a
snapshot-scoped `ref`, role, accessible name, states, stable identity, bounds,
clipping state, and a bounded computed-style whitelist. Role and name follow the
broad accname order (labelledby, aria-label, native label, content, title,
placeholder) and skip `aria-hidden`, `inert`, and invisible elements. This is
still a heuristic; a production implementation should consume the browser
accessibility tree. Field values are never read into nodes, and password,
one-time-code, and payment field values are redacted from the ARIA snapshot.

Output is bounded. The default `detail: "summary"` leaves out computed styles
and clipping state, clips node text to 160 characters, defaults to 150 nodes
and 6,000 ARIA characters, and stops adding nodes once the reply reaches about
32 KB. `detail: "full"` returns everything up to `maxNodes` (default 300, max
1000) and `maxAriaChars` (default 20,000). The stored snapshot is always full,
so `visual.compare` sees computed styles either way. `truncated`,
`ariaSnapshotTruncated`, and `visibleNodeCount` report what was left out. Pass `rootSelector` (for example
`main`) to scope semantic and accessibility evidence.

Operations that read or change a page are serialized per page. An action
invalidates the snapshot before it runs, so a failed or timed-out action also
requires a new observation. Before acting, the target is re-verified by tag,
role, and accessible name; a recycled node fails with `NODE_CHANGED`. A
navigation during observation fails with `STALE_SNAPSHOT`. JavaScript dialogs
are dismissed and counted.

## Comparison

`visual.compare` requires each capture to come from the same page state as its
snapshot (same page, document revision, and action sequence); otherwise it
fails with `EVIDENCE_MISMATCH`. Environments must match, including
`reducedMotion` and the exact browser version.

Nodes are paired first by `data-canonical-id`, `data-testid`, or DOM ID when
that identity is unique on both sides, then by a unique role/name pair.
Equal-sized repeated role/name groups are paired in document order at
confidence 0.5; set `policy.pairRepeatedGroups: false` to leave them unmatched.
Findings are ordered errors first and capped by `policy.maxFindings` (default
200), with `summary` and `findingsTotal` always complete. Node matching is
returned only with `policy.includeMatching`.

All regions are CSS pixels and are scaled by `deviceScaleFactor`.
`policy.rasterRegions` crops equal-sized reference and candidate surfaces
before pixel comparison. `policy.ignoredRegions` are relative to the compared
surface (after cropping) and are removed from the raster mismatch ratio; combine
them with a scoped observation to exclude the same chrome from structured
evidence. `policy.allowViewportWidthMismatch` is honored only when both raster
regions are given with equal sizes and the viewport heights match.

Both rasters are checked from their PNG chunk structure before decoding. A file
that is not a PNG, is truncated, does not start with exactly one 13-byte IHDR,
or is interlaced fails with `INVALID_RASTER`; one larger than `maxRasterPixels`
(the largest session capture, 4096 x 4096 at 3x) fails with `RASTER_TOO_LARGE`.
pngjs would otherwise adopt a later IHDR's size and inflates interlaced data
without an output limit.

## Artifacts

Captures and diffs are written with owner-only permissions to a per-session
directory under `KETPLUS_BROWSER_ARTIFACTS` (default: a process-specific
directory in the system temp folder). Closing a session deletes its snapshots,
captures, and any diff that used them. `npm run compare` is the exception: it
keeps its files under `apps/browser/artifacts/`, which Git ignores. Captures can
contain authenticated page content; do not copy that directory elsewhere.

For a direct feasibility run against any two caller-supplied URLs:

```sh
npm run compare -- http://127.0.0.1:4000/reference http://127.0.0.1:5000/candidate
```

The command grants exactly the two input origins, opens them in separate
ephemeral contexts with an identical viewport, captures both canonical
snapshots, prints a structured comparison report, and exits nonzero when the
result fails or is blocked.

## Deliberate limits

- Chromium-family browsers (Chrome, Edge, Chrome for Testing) are the only engines.
- A service holds at most 8 open sessions (including ones still starting) and a
  session at most 20 pages (`LIMITS.maxSessions`, `LIMITS.maxPagesPerSession`).
  `browser.session.create` and `browser.open` past the cap fail with
  `LIMIT_REACHED` (`details.limit`). A popup the site opens past the cap is
  closed instead of adopted.
- DOM mutation without navigation does not increment `documentRevision`. Node
  re-verification catches changes to tag, role, or accessible name, but not a
  node that keeps them while its meaning changes.
- Subframe content is not part of semantic nodes (`subframeCount` reports how
  many frames were skipped).
- Captures cover the viewport only, while semantic nodes include off-screen
  elements.
- Comparison does not yet support reviewed masks, animation clocks, nested-frame
  provenance, pseudo-elements, canvas semantics, or interaction scenarios.
- The prototype is not an approval system and cannot bless reference captures.
- Page content is untrusted. MCP results of `browser_observe`, `browser_inspect`,
  `browser_logs` and `browser_network` start with an "Untrusted website content
  follows" text block, and the server and host instructions tell agents never to
  follow instructions found in page content. This is guidance for the model, not
  an enforcement boundary; the origin policy is the enforcement boundary.
