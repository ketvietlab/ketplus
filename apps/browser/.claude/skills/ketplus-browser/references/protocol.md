# browser.v0 reference

The source of truth is `apps/browser/src/protocol.mjs`; `browser.protocol.schema`
returns the same contract as JSON Schema. Unknown fields are rejected.

## Contents

- [Sessions and pages](#sessions-and-pages)
- [Observe](#observe)
- [Act](#act)
- [Wait](#wait)
- [Logs](#logs)
- [Network](#network)
- [Inspect](#inspect)
- [Capture](#capture)
- [Compare](#compare)
- [Limits](#limits)
- [Error codes](#error-codes)
- [Transports and environment](#transports-and-environment)

## Sessions and pages

`browser.capabilities` `{}` → `protocolVersion`, `engine` (`playwrightVersion`,
`preference` (`auto`|`chrome`|`msedge`|`bundled`|`playwright`), resolved `name` and
`source` (`system`|`ketplus-cache`|`downloaded`|`playwright-cache`) and
`browserVersion` once launched, `isolatedProfile: true`), `methods`, `actions`, `waits`, `limits`,
`hostPolicy.allowedOrigins`, `limitations`.

`browser.protocol.schema` `{}` → JSON Schema for every method's params.

`browser.session.create`
- `viewport` `{width, height}` integers 1–4096 (default 1440×900)
- `deviceScaleFactor` >0, ≤3 (default 1)
- `theme` `light` | `dark` | `no-preference` (default `light`)
- `locale` BCP 47 tag (default `en-US`)
- `reducedMotion` `reduce` | `no-preference` (default `reduce`)
- `allowedOrigins` ≤32 entries: http(s) origins, `loopback`, or `*`; must fit the host ceiling
- `evidenceContext` flat object (string ≤512, number, boolean, null), ≤4096 bytes; copied into every snapshot and capture
- → `{sessionId, environment, allowedOrigins}`

`browser.session.request` — for login endpoints that set cookies
- `sessionId`, `url`, `method` (`GET` default, `POST`, `PUT`, `PATCH`, `DELETE`), `headers`, `json` (not with GET/DELETE), `timeoutMs`
- Rejected headers: `Cookie`, `Host`, `Content-Length`, hop-by-hop, `Proxy-*`
- Redirects are not followed.
- → `{outcome: accepted|rejected, sessionId, url, status, ok, redirected, contentType}`

`browser.session.close` `{sessionId}` → `{outcome: closed, sessionId, blockedRequestCount}`. Deletes the session's snapshots, captures and diffs.

`browser.pages` `{sessionId}` → `{pages: [{pageId, url, openerPageId, documentRevision, allowed}], blockedRequestCount, blockedRequests}` (up to 50 redacted URLs).

`browser.open` `{sessionId, url, waitUntil?, timeoutMs?}` — `waitUntil`: `commit` | `domcontentloaded` (default) | `load` | `networkidle`. Fails with `NAVIGATION_DENIED` if a redirect leaves the policy. → `{sessionId, pageId, url, title, documentRevision}`

`browser.page.close` `{sessionId, pageId}` → `{outcome: closed, sessionId, pageId}`

## Observe

`browser.observe`
- `sessionId`, `pageId`
- `maxNodes` 1–1000 (default 300)
- `maxAriaChars` 0–100,000 (default 20,000)
- `includeScreenshot` (default false): adds `captureId` from the same page state
- `label` for the capture file name
- `rootSelector` standard CSS; Playwright engines (`text=`, `xpath=`, `>>`, `internal:`) are rejected
- `visibleOnly` (default false): keep only nodes that intersect the viewport and the visible area of every scroll container around them

Result:
- `snapshotId`, `sessionId`, `pageId`, `documentRevision`, `page {url, title}`
- `environment`, `evidenceContext`, `rootSelector`, `visibleOnly`
- `logs {cursor, errors, errorsSinceObserve, warningsSinceObserve}`: counts only; entries come from `browser.logs`. Observing resets the `SinceObserve` counts.
- `ariaSnapshot` (password and one-time-code values redacted), `ariaSnapshotTruncated`, `ariaSnapshotSha256` (of the full text)
- `nodes[]`: `ref`, `stableId`, `tag`, `role`, `name` (≤200), `level` (headings), `text` (≤300; empty for form fields), `states`, `bounds {x, y, width, height}` (CSS px relative to the root), `clipped`, `styles` (display, position, fontFamily, fontSize, fontWeight, lineHeight, letterSpacing, color, backgroundColor, borderTopWidth, borderTopColor, borderRadius, opacity, overflowX, overflowY)
- `truncated`, `visibleNodeCount`, `subframeCount`, `captureId?`

States: `enabled`/`disabled`, `checked`, `mixed`, `expanded`/`collapsed`, `selected`, `pressed`, `current`, `required`, `readonly`, `invalid`, `focused`.

`<details>` is a `group`; its `<summary>` is a `button` with `expanded`/`collapsed` from the `open` state.

Hidden, `aria-hidden`, `inert`, zero-size and `role=none` (non-focusable) elements are omitted. Open shadow roots are included; subframes are not.

## Act

`browser.act` `{sessionId, pageId, expectedSnapshotId?, action, settle? (default true)}`

| `action.type` | Fields | Needs snapshot |
|---|---|---|
| `click` | `target`, `timeoutMs?` | yes |
| `fill` | `target`, `value` (string), `timeoutMs?` | yes |
| `press` | `target`, `key` (e.g. `Enter`, `Control+A`), `timeoutMs?` | yes |
| `hover` | `target`, `timeoutMs?` | yes |
| `check` | `target`, `checked?` (default true), `timeoutMs?` | yes |
| `select` | `target`, `value` or `values` (option value or label), `timeoutMs?` | yes |
| `scroll` | `target?`, `deltaX?`, `deltaY?` | only with `target` |
| `navigate` | `url`, `waitUntil?`, `timeoutMs?` | no |
| `history` | `direction` (`back` \| `forward`), `waitUntil?`, `timeoutMs?` | no |
| `reload` | `ignoreCache?` (default false), `waitUntil?`, `timeoutMs?` | no |

`target` is `{nodeRef}`. Default action timeout is 10 s. `history` traverses the page's own session history, including `pushState` entries, which fire `popstate` without reloading; at either end it does nothing. `reload` with `ignoreCache` refetches every subresource, for example a stylesheet the HTTP cache still serves.

→ `{outcome: applied, sessionId, pageId, url, documentRevision, navigation, openedPages, dialogsDismissed, logs, requiresObservation: true}`

- `navigation`: `full-load` (a new document was committed), `same-document` (pushState, replaceState, fragment or history traversal within the document), or `none`
- `logs`: the same counts as observe; `errorsSinceObserve` counts errors since the latest observation

## Wait

`browser.wait` `{sessionId, pageId, condition, timeoutMs? (default 10 s)}`

- `{type: "load", state?: domcontentloaded|load|networkidle}`
- `{type: "text", text, exact? (default false, substring), state?: attached|detached|visible (default)|hidden}`
- `{type: "url", equals}` or `{type: "url", startsWith}` (exactly one)
- `{type: "settled"}`

→ `{outcome: satisfied, conditionType, url, documentRevision}`. Waiting does not invalidate the snapshot.

## Logs

`browser.logs` `{sessionId, pageId, since?, levels?, kinds?, limit?}` — read-only; does not invalidate the snapshot.
- `levels` ⊆ `error`, `warning`, `info`, `log`, `debug`
- `kinds` ⊆ `console`, `pageerror` (uncaught exception), `unhandledrejection`, `requestfailed` (network failure), `http` (status ≥ 400)
- `since` a previous `cursor`; `limit` 1–500 (default 100)
- → `{sessionId, pageId, entries, cursor, hasMore, dropped}`
- Entry: `seq`, `kind`, `level`, `text` (≤2000), `source {url, line, column}` (1-based) or null, `stack?` (≤4000), `count`, `time`

Consecutive identical entries collapse into one with `count`; a repeat moves the entry past older cursors. The page keeps its latest 500 entries; `dropped` counts newer-than-`since` entries that no longer fit. Collection starts when the page opens and spans its navigations.

## Network

`browser.network` `{sessionId, pageId, since?, urlContains?, methods?, resourceTypes?, failedOnly?, limit?}` — read-only, finished requests of the page.
- `methods` upper-case (`GET`, `POST`, …); `resourceTypes` Playwright types (`document`, `stylesheet`, `script`, `fetch`, `xhr`, …)
- `failedOnly` keeps network failures and status ≥ 400; `limit` 1–1000 (default 100)
- → `{sessionId, pageId, entries, cursor, hasMore, pendingRequests, dropped}`
- Entry: `seq`, `method`, `url` (redacted), `resourceType`, `status` (null on failure), `failure?`, `responseBodyBytes`, `durationMs`, `startedAt`

Bodies and headers are not returned. The page keeps its latest 1000 requests.

## Inspect

`browser.inspect` `{sessionId, pageId, selector | target, expectedSnapshotId?, fields, maxElements?}` — reads named fields; never runs caller script and does not invalidate the snapshot.
- Exactly one of `selector` (standard CSS, may address `html` or anything outside an observation root) or `target {nodeRef}` (needs the latest `snapshotId`)
- `fields` 1–32 of `attributes`, `scroll`, `value`, `bounds`, `text`, `computedStyle:<property>` (camelCase, kebab-case or `--custom`)
- `maxElements` 1–50 (default 10)
- → `{sessionId, pageId, selector | nodeRef, matchCount, elements, truncated}`
- Element: `index`, `tag`, `id?`, then per field: `attributes` (≤64, values ≤500), `scroll {top, left, width, height, clientWidth, clientHeight}` (the document's for `html`), `value` (form fields; `checked`, `values` when relevant; null otherwise), `bounds`, `text` (≤1000), `computedStyle {property: value}`

Password, one-time-code and payment-card fields return `value: "[redacted]"` and omit the `value` attribute.

## Capture

`browser.capture` `{sessionId, pageId, label?}` → `{captureId, sessionId, pageId, snapshotId (null), mediaType, width, height (device px), sha256, url, documentRevision, actionSequence, environment, evidenceContext, recordedAt, path (JSON-RPC only)}`. Viewport only, animations disabled, caret hidden.

## Compare

`visual.compare` `{reference: {snapshotId, captureId}, candidate: {snapshotId, captureId}, label?, policy?}`

`visual.compare.native` compares a browser reference with a native-app PNG. Its
`candidate` has `{path, viewport: {width, height}, deviceScaleFactor, theme}`.
Create the capture directory at `${TMPDIR}/ketplus-browser-native-${UID}` with
mode `0700` and write the PNG there as the current user. The service rejects
symlinks, files outside that directory, files over 20 MB, and mismatched
viewport, scale or theme. It returns raster evidence only: it cannot infer Qt
accessibility, text or computed styles from a PNG. Set
`policy.maxPixelMismatchRatio` to the requested threshold, for example `0.05`.
The source PNG remains owned by the caller; closing the browser session deletes
the comparison diff but does not remove that source file.

Each capture must match its snapshot's session, page, document revision and action sequence (`EVIDENCE_MISMATCH` otherwise).

Policy (all optional):

| Field | Default | Notes |
|---|---|---|
| `geometryTolerance` | 1 | CSS px per bound |
| `pixelThreshold` | 0.1 | pixelmatch color threshold 0–1 |
| `maxPixelMismatchRatio` | 0.05 | of compared (non-ignored) pixels |
| `styleProperties` | 11 visual properties | subset of node `styles` keys |
| `ignoredRegions` | `[]` | CSS px rectangles, relative to the compared surface |
| `rasterRegions` | none | `{reference, candidate}` CSS px crops, positive size |
| `allowViewportWidthMismatch` | false | needs equal raster regions and equal viewport heights |
| `pairRepeatedGroups` | true | pair same role/name groups in order (confidence 0.5) |
| `maxFindings` | 200 | 1–1000 |
| `includeMatching` | false | adds `matching` pairs with strategy/confidence |

Environment keys that must match: `viewport`, `deviceScaleFactor`, `theme`, `locale`, `reducedMotion`, `browserEngine`, `browserVersion`.

Result:
- `status`: `passed` | `failed` | `blocked` (`environmentMismatch` lists keys)
- `policy` (resolved), `summary {errors, warnings, byKind}`, `matchedCount`, `matching?`
- `findings` (errors first), `findingsTotal`, `findingsTruncated`
- `raster {width, height, scale, differentPixels, comparedPixels, ignoredPixels, mismatchRatio, diffId, diffSha256, diffPath (JSON-RPC only)}`

Node pairing order: unique `stableId` on both sides → unique role+name → equal-sized role+name groups in document order. Unmatched generic nodes without a stable id are not reported.

Finding kinds: `accessibility-tree` (`expectedSha256`, `actualSha256`, `firstDifference {line, expected, actual}`), `missing-element`, `extra-element` (warning), `semantic`, `text`, `geometry` (`delta`), `clipping`, `style` (`property`, `expected`, `actual`).

## Limits

| Limit | Value |
|---|---|
| Viewport side | 4096 |
| Device scale factor | 3 |
| Nodes per observation | 300 default, 1000 max |
| ARIA snapshot chars | 20,000 default, 100,000 max |
| Log entries kept per page | 500 (100 per read by default) |
| Network entries kept per page | 1000 (100 per read by default) |
| Inspected elements | 10 default, 50 max |
| Timeout | 120,000 ms max |
| Findings | 200 default, 1000 max |
| Allowed origins per session | 32 |
| Evidence context | 4096 bytes |
| MCP inline image | 4,000,000 pixels (`KETPLUS_BROWSER_MAX_IMAGE_PIXELS`) |

## Error codes

| Code | Raised by |
|---|---|
| `INVALID_PARAMS` | schema validation (`details.issues[{path, message}]`) |
| `INVALID_ARGUMENT` | semantic argument checks (selector syntax, headers, url condition) |
| `METHOD_NOT_FOUND` | unknown method |
| `SESSION_NOT_FOUND`, `PAGE_NOT_FOUND` | unknown or closed ids |
| `BROWSER_UNAVAILABLE` | no installed Chrome/Edge and Chrome for Testing is missing and cannot be downloaded, or `KETPLUS_BROWSER_ENGINE` names a missing or invalid engine |
| `BROWSER_DISCONNECTED`, `BROWSER_LAUNCH_FAILED`, `SESSION_CREATE_FAILED`, `PAGE_CREATE_FAILED` | engine lifecycle |
| `ORIGIN_NOT_PERMITTED` | session origins exceed the host ceiling |
| `NAVIGATION_DENIED`, `REQUEST_DENIED` | URL or final page outside the session policy |
| `NAVIGATION_FAILED`, `REQUEST_FAILED` | network or load failure |
| `OBSERVATION_ROOT_NOT_FOUND`, `OBSERVATION_FAILED` | observe |
| `INSPECTION_FAILED` | inspect |
| `STALE_SNAPSHOT`, `NODE_NOT_FOUND`, `NODE_CHANGED` | fencing |
| `ACTION_FAILED`, `UNSUPPORTED_ACTION`, `WAIT_FAILED`, `TIMEOUT` | act and wait |
| `CAPTURE_FAILED`, `ARTIFACT_NOT_FOUND` | capture and artifacts |
| `SNAPSHOT_NOT_FOUND`, `CAPTURE_NOT_FOUND`, `EVIDENCE_MISMATCH`, `INVALID_RASTER_REGION`, `CAPTURE_DIMENSION_MISMATCH` | compare |
| `ENGINE_ERROR`, `INTERNAL_ERROR` | unexpected; message is redacted to one line |

## Transports and environment

JSON-RPC (`node src/cli.mjs`): one JSON-RPC 2.0 message per line. Notifications get no response. Integer codes: `-32700` parse, `-32600` invalid request, `-32601` method not found, `-32602` invalid params/argument, `-32603` internal, `-32000` other browser errors; the string code is in `error.data.code`. `server.shutdown` drains in-flight requests, closes the browser and exits.

Engine environment: `KETPLUS_BROWSER_ENGINE` (`auto` default: Chrome, then Edge, then Chrome for Testing), `KETPLUS_BROWSER_CACHE` (Chrome for Testing directory), `KETPLUS_BROWSER_DOWNLOAD=0` (never download).

MCP (`node src/mcp.mjs`): tools listed in SKILL.md. Results carry the JSON as text and as `structuredContent`; observe-with-screenshot, capture and compare add a PNG image block. Errors are `isError: true` with `{code, message, data?}`.

Environment variables (read by both transports):

| Variable | Default | Meaning |
|---|---|---|
| `KETPLUS_BROWSER_ALLOWED_ORIGINS` | `loopback` | host origin ceiling |
| `KETPLUS_BROWSER_ARTIFACTS` | per-process temp directory | captures and diffs |
| `KETPLUS_BROWSER_MAX_IMAGE_PIXELS` | 4,000,000 | MCP inline image cap |
