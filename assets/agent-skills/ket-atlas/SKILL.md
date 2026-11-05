---
name: ket-atlas
description: Create or update interactive framework-native mockups and workflow maps in KetAtlas format. Use when a user requests KetAtlas output, an atlas.json project, or draggable flows connecting screens rendered by HTML, React, Vue, KetJS, or another product framework; also use to scaffold, serve, or audit an existing atlas.
---

# KetAtlas

The invocation name is `$ket-atlas`; the npm package and CLI remain `ketatlas`.

Deliver an editable `<name>.ketatlas/` bundle with a version 1 `atlas.json` that `npx ketatlas@0.6.1 serve` can open. KetAtlas supplies the draggable viewer; the selected product framework owns screen rendering. A screenshot gallery or a Mermaid diagram alone is not this deliverable.

## Read the brief and choose the scope

Use the user's requirements, existing project files, and design references to determine:

- Product, audience, requested flows, and the goal of each flow.
- Platform and viewport: mobile, web, mixed screens, or a process without screens.
- Design system and product content language. KetAtlas's own viewer is English; authored content can use the requested language.
- Output directory, existing screens to reuse, and the intended depth of interaction.

A brief may be a chat message or a Markdown file; it is not another KetAtlas JSON format. If details are missing, use reasonable defaults and record assumptions. Ask only for information that materially blocks the requested result. Do not introduce unrelated flows or build a production backend to make a mockup work.

## Choose the design system

Before scaffolding or editing visual screens, ask the user to choose a design system unless the current request already makes the choice explicit. Present these options in one concise prompt:

1. **Auto (recommended)** — use [Két Design System](https://github.com/ketvietlab/ketjs/tree/develop/packages/design-system).
2. **Két Design System** — use the same canonical Két source explicitly.
3. **Carbon Design System**.
4. **GitHub Primer**.
5. **Microsoft Fluent 2**.
6. **No design system** — create product-specific shared CSS without claiming conformance to a named system.
7. **Custom source** — ask for a repository URL or local design-system path if it was not included with the selection.

Do not start visual screen implementation until the user answers. If the user explicitly delegates the choice to the agent, treat that as **Auto**. This selection prompt is still required for an existing atlas; its documented/current system may be offered as the likely choice. A request that already names a system or supplies a design-system source counts as an answer and must not be asked again.

Inspect the selected source, its usage documentation, tokens, components, icons, relevant product patterns, and implementation framework before mocking. Reuse its actual public components, assets, and composition patterns when available; do not merely imitate its color palette. Replace incompatible starter styling rather than layering multiple design systems. For one shared iOS/Android prototype, create one framework implementation with reusable presenters and states unless separate variants were requested.

Prefer a local adapter descriptor with `schemaVersion: "ketatlas.design-system-adapter.v1"` when the selected design system provides one. Discover it from the user-supplied path, a repository `design-system.atlas.json`, or the package's documented `./atlas/profile.json` export. Read the descriptor before using its assets or commands; do not assume capabilities it does not declare. If there is no adapter, use the design system through its documented native framework interface and record that no reproducible adapter lock is available.

For Két Design System, prefer the descriptor exported at `@ketvietlab/design-system/atlas/profile.json` and its declared `ket-design-system-atlas` materializer. Read that descriptor for asset names, root attributes, slots, hooks, and state ownership instead of hard-coding them.

## Use the product's native framework

Inspect the product and selected design-system source before choosing how screens render. Match their implementation framework exactly:

- React components render through the product's React runtime and routes.
- Vue components render through the product's Vue runtime and routes.
- KetJS server components render on a KetJS server. Do not reconstruct their output with client-side DOM builders or handwritten HTML strings.
- Apply the equivalent rule to any other declared framework. Use static HTML only when the product is actually static HTML, no framework exists, or the user explicitly requests static HTML.

Choose the existing product framework first. If there is no product implementation yet, use the selected design system's canonical framework. If neither declares a framework, static HTML is the fallback. Preserve the rendering model as well as the library name: server components must render on their server, while client components remain native client components. If the product and design system require incompatible frameworks and no documented adapter exists, pause for a design-system/framework decision instead of translating either implementation.

Do not create a second component representation for Atlas. In particular, do not translate framework components into a KetAtlas-specific component JSON DSL, duplicate their markup in `screens/`, or imitate them with local CSS. Fix an incorrect canonical design-system component or adapter at its source before continuing; do not hide the mismatch in an Atlas-only override.

Separate screen presentation from business behavior. A screen presenter receives a deterministic view model and composes the real design-system components. Atlas routes call that presenter with fixtures; production routes call the same presenter with real data, permissions, and actions. Atlas may select fixture states through routes or query parameters, but it does not own product markup.

When a workspace contains multiple atlases, put reusable presenters, component compositions, fixture factories, tokens, and Atlas route helpers in one shared framework module outside the individual `.ketatlas` bundles. Each bundle owns only its flow graph, per-atlas fixture/state declarations, renderer sidecar, and documentation. Never copy shared component or style files from one atlas to another. Prefer one native renderer application that exposes namespaced `screenBasePath` routes for all atlases.

Keep static atlas bundles self-contained and dependency-free. For framework-native atlases, reuse the product's existing package manifest, lockfile, framework server, and shared UI source outside the atlas bundle; do not add another package installation or bundler inside `.ketatlas`. Materialize or copy permitted assets only for a genuinely static product. Record the selected system, implementation framework, shared module location, source URL/path, pinned version or commit when known, adapter strategy, and any license or fidelity limitation in the bundle README.

### Declare the native renderer

Place `atlas.renderer.json` beside `atlas.json`. This sidecar is the executable renderer contract and does not change the version 1 workflow schema. Use an argument array, never a shell command string:

```json
{
  "$schema": "https://unpkg.com/ketatlas@0.6.1/renderer.schema.json",
  "version": 1,
  "framework": "react",
  "command": ["npm", "run", "atlas:serve", "--", "--host", "{host}", "--port", "{port}"],
  "cwd": "../..",
  "readyPath": "/__atlas/ready",
  "screenBasePath": "/__atlas/customer-care/"
}
```

`cwd` is relative to the bundle. KetAtlas substitutes `{host}`, `{port}`, and `{atlasDirectory}` and also provides `KETATLAS_HOST`, `KETATLAS_HTML_PORT`, and `KETATLAS_PROJECT_DIR`. `readyPath` must return HTTP 2xx when the framework server is ready. `screenBasePath` is origin-relative and ends in `/`; screen URLs in `atlas.json` resolve beneath it. Multiple atlases may use the same command and shared renderer source while choosing different namespaced base paths.

Start the viewer and HTML renderer on separate loopback ports:

```sh
npx --yes ketatlas@0.6.1 serve ./tasks/mockups.ketatlas --renderer --port 60550 --html-port 60551
```

`--renderer` is an explicit trust boundary because it executes the declared local command. The viewer remains on the first port; framework HTML, scripts, styles, assets, and same-origin requests stay on the second. Do not proxy or rebuild framework output in the viewer process.

## Scaffold or extend

Node.js 22 or newer is required. For a new, empty destination:

```sh
npx --yes ketatlas@0.6.1 scaffold ./tasks/mockups.ketatlas --template basic
```

Choose `basic` for mobile, `web` for desktop, or `process` for steps without UI. These are starting examples, not required product flows. Replace their sample content with the requested product.

For an existing atlas, read its JSON, renderer contract, and referenced framework source or static screen files. Preserve useful IDs and URLs; scaffold refuses a nonempty directory and has no `--force` option. Pin the CLI version in the README commands or use a global installation. A consumer atlas does not need a local KetAtlas installation.

### Backfill legacy projects

Before changing an existing atlas, run `ketatlas discover <workspace> --json`. If its manifest is not located at `<name>.ketatlas/atlas.json`, migrate it as part of the task:

1. Inventory the manifest, sibling progress file, schema, README, screens, styles and assets. Record every relative `$schema`, screen URL and node URL before moving anything.
2. If the containing directory is a self-contained Atlas project, rename that directory to a concise `<name>.ketatlas`; moving the whole directory preserves relative URLs.
3. If the manifest shares a directory with unrelated application code, create a sibling `<name>.ketatlas` bundle. Move only Atlas-owned files, preserve Git history when possible, and rewrite relative paths for resources that intentionally remain outside the bundle.
4. Keep one canonical manifest. Do not leave a copied legacy `atlas.json` behind or delete files whose ownership is unclear.
5. Run `ketatlas discover <workspace> --json`, `ketatlas validate <name>.ketatlas`, and `ketatlas audit <name>.ketatlas --strict` after migration. Fix discovery errors and broken paths before editing product flows.

Legacy JSON paths remain accepted by the CLI for migration, but new and updated deliverables must use the bundle pattern so desktop tools can discover them without scanning arbitrary JSON.

A self-contained project typically has:

```text
tasks/mockups.ketatlas/
  atlas.json
  ketatlas.schema.json
  screens/                 HTML pages and shared CSS/JavaScript
  styles/                  Starter design assets, when used
  README.md                Run commands, flow coverage, assumptions, verification
```

A framework-native bundle replaces `screens/` and copied styles with `atlas.renderer.json`; its actual routes, shared presenters, fixtures, and styles stay in the product's framework source. Multiple bundles should point to the same shared renderer module instead of growing parallel component implementations.

For static mode, keep product assets inside the served directory when practical. For native mode, deliver JSON/schema, `atlas.renderer.json`, per-atlas fixtures or declarations, and documentation while changing shared framework source in its owning product module. Do not scaffold a `package.json`, lockfile, `node_modules`, asset build scripts, or a copied viewer/test harness in the atlas folder just to use KetAtlas. The installed CLI supplies scaffold, serve, validate, and audit. Browser verification can use the agent's external tooling. Preserve unrelated application tooling when extending an existing repository.

Do not create a wrapper viewer or a custom canvas: `serve ./tasks/mockups.ketatlas` provides it.

## Model the journey

Sketch the requested flow inventory before implementing screens: start, user action, condition, destination, and outcome. Include the relevant success, validation, empty, loading, error, and recovery states supported by the brief; do not multiply every screen into states that have no meaning for that product.

Use the generated `ketatlas.schema.json` as the field reference. `version: 1` is the atlas format version, not the npm package version. Unknown properties are rejected. Store requirements, coverage notes, and backlog items in Markdown rather than inventing JSON fields.

Core rules:

- Register each reusable screen once in `screens` with `id`, `title`, and `url`. Paths resolve relative to `atlas.json`.
- Give each flow an `id`, `title`, nonempty `nodes`, and `edges`. Set `start` and `ends` explicitly when authoring a journey so entry and outcomes are unambiguous.
- A screen node references a screen ID. A repeated occurrence gets a new node ID and may override `url`, for example `./screens/sign-in.html?state=invalid`.
- For a decision or off-screen operation, use a node with `type: "note"`, `title`, and optionally `description`. Use `type: "external"` for a handoff. There is no `decision` node type.
- Edges require `from`, `to`, and a nonempty `label` describing the action or condition. `kind` is `primary`, `conditional`, or `recovery`.
- Edge endpoints, `start`, and `ends` reference node IDs within the same flow. Cross-flow edges are unsupported; reuse the destination screen or explain the handoff with a note.
- IDs start with a letter or digit and contain only letters, digits, dots, underscores, or hyphens. Screen and flow IDs are unique across the atlas; node IDs are unique within their flow.
- Set `column` and `row` explicitly for branches. Both are integers from 0 to 100; no two nodes in a flow share a cell. This is a grid, not an automatic graph layout engine.
- Set the atlas `viewport` or a screen override to the intended layout size. The mobile default is 390 × 844; choose desktop dimensions from the brief. Width and height are integers from 160 to 4096.

Example static structure; for native routes, replace the `.html` paths with paths relative to `screenBasePath`:

```json
{
  "$schema": "./ketatlas.schema.json",
  "version": 1,
  "title": "Account access",
  "viewport": { "width": 390, "height": 844 },
  "screens": [
    { "id": "sign-in", "title": "Sign in", "url": "./screens/sign-in.html" },
    { "id": "home", "title": "Home", "url": "./screens/home.html" }
  ],
  "flows": [
    {
      "id": "sign-in",
      "title": "Sign in",
      "start": "entry",
      "ends": ["success"],
      "nodes": [
        { "id": "entry", "screen": "sign-in", "column": 0, "row": 0 },
        {
          "id": "invalid",
          "screen": "sign-in",
          "title": "Invalid credentials",
          "url": "./screens/sign-in.html?state=invalid",
          "column": 1,
          "row": 1
        },
        { "id": "success", "screen": "home", "column": 2, "row": 0 }
      ],
      "edges": [
        { "from": "entry", "to": "success", "label": "Credentials accepted" },
        {
          "from": "entry",
          "to": "invalid",
          "label": "Credentials rejected",
          "kind": "conditional"
        },
        { "from": "invalid", "to": "entry", "label": "Try again", "kind": "recovery" }
      ]
    }
  ]
}
```

## Build the actual screens

For static HTML mode, make each screen a complete HTML document with a viewport meta tag and `body { margin: 0; }`. For framework-native mode, make every declared screen URL a directly addressable framework route whose response is a complete document at the selected state. Keep product content padding, but do not wrap the page in a second phone bezel, presentation frame, reviewer sidebar, or outer mockup margin. The iframe is the screen boundary.

Use the shared framework presenters/styles and deterministic fixtures. States referenced by node URLs must render directly when opened or refreshed, without requiring a previous login or click. Implement the relevant buttons, forms, validation feedback, and navigation in the native framework: graph arrows do not wire screen interactions automatically.

Static screens use an opaque sandbox by default. Native renderer mode places screens on a distinct origin and enables `allow-same-origin` there so framework modules and same-origin assets work without granting access to the viewer origin. Keep framework routes trusted and loopback-only. Test in **Try this screen**, not just a standalone tab.

## Synchronize Light/Dark with screen content

When theme synchronization is requested, wire the product's existing theme system to the viewer. Keep the selected design system: use its semantic light/dark tokens or theme provider, including overlays, forms, surfaces, and text. Do not simulate dark mode with CSS inversion or replace product colors with hard-coded viewer colors.

**Availability:** Theme synchronization requires KetAtlas 0.5.0 or newer. Earlier versions, including 0.4.2, do not provide the sync controls or API. Confirm that the running viewer has **Appearance → Light/Dark**, **Sync screen theme**, and `setThemeSync` before claiming end-to-end support. Upgrade older viewers before testing synchronization. Static templates from 0.5.0 include the bridge; inspect before adding another listener.

The viewer's theme and screen theme are independent by default. Turning on **Sync screen theme** sends the selected theme to both canvas thumbnails and interactive previews. The protocol is:

- Viewer → screen: `{ type: "ketatlas:theme", theme: "light" | "dark" | null }`.
- Screen → viewer: `{ type: "ketatlas:theme-ready" }`, after its listener and theme provider are ready.
- `null` means synchronization was turned off: restore the screen's original theme setting, including an absent attribute or a product's `system` preference, rather than forcing light mode.

For static screens using `data-theme`, register this once in the HTML head:

```js
(() => {
  const root = document.documentElement;
  const original = {
    theme: root.getAttribute("data-theme"),
    scheme: root.style.colorScheme,
  };
  window.addEventListener("message", (event) => {
    const message = event.data;
    if (event.source !== window.parent || message?.type !== "ketatlas:theme") return;
    if (!["light", "dark", null].includes(message.theme)) return;
    const theme = message.theme ?? original.theme;
    if (theme === null) root.removeAttribute("data-theme");
    else root.setAttribute("data-theme", theme);
    root.style.colorScheme = message.theme ?? original.scheme;
  });
  if (window.parent !== window) window.parent.postMessage({ type: "ketatlas:theme-ready" }, "*");
})();
```

For React, Vue, KetJS, or another framework, install one listener in the shared presenter/provider lifecycle, adapt the assignment to the product's theme API, and remove the listener on unmount. Capture the original product preference before the first synchronized update. Send `theme-ready` after registering the listener so asynchronously mounted routes receive the current theme. Reuse the adapter across screens and atlases; avoid competing DOM listeners and framework state. Synchronized changes must not persist over the user's product theme preference, reload the iframe, remount the screen, or reset navigation/form state.

Always check `event.source === window.parent`, the message type, and the theme value. The `"*"` target supports opaque sandbox origins and carries only appearance data. Never send credentials over this channel, access `parent.document`, or relax sandbox permissions to make themes work. Accessible same-origin content also receives `data-theme` and `color-scheme` from the viewer, but sandboxed and separate-origin framework screens need the bridge. Product CSS still needs to support both themes.

Verify Light → Dark → Light with sync enabled in both a canvas preview and an interactive preview. Enter a form value or navigate within the preview first and confirm it survives. Turn sync off and confirm the original product theme is restored while the viewer keeps its selected theme. Check newly loaded screens and framework route changes, plus readable contrast for text, inputs, borders, and overlays in both themes. Keep screenshot evidence when browser automation is available; do not claim synchronization from a standalone screen screenshot alone.

Theme settings are viewer options, not fields in `atlas.json` or `atlas.renderer.json`. For an embedded supporting viewer, use `theme`, `syncTheme`, `instance.setTheme("dark")`, and `instance.setThemeSync(true)`. See the [public integration contract](https://github.com/ketvietlab/ketatlas/blob/develop/docs/integration.md#workspace-and-screen-themes) for state and events. If content cannot support the bridge or both palettes, preserve its own theme and report the limitation.

KetAtlas 0.5.1 preloads only the selected flow and caches up to 24 viewer-owned frames by default (`maxCachedPreviews`, browser API, range 3–128). Selecting or panning to another flow queues that flow; unvisited flows and unused screens are not preloaded. Dialogs load only when opened. Cached frames preserve form/navigation state until evicted; eviction or refresh resets it. Mock-owned nested iframes are outside the viewer's limit. Register the theme bridge during initialization because scripts execute during flow preload. This is an in-memory session cache, not an offline download.

`serve --cache-size 24` sets the cache limit (default 24, range 3–128). It cannot be combined with `--no-preload` and does not modify atlas JSON. This flag is not supported by npm 0.5.0.

Since KetAtlas 0.5.2, the local server uses content-based ETags and 304 responses for unchanged files. It is distinct from the iframe session cache: retained frames need a reload to see edits, and recreating an evicted frame still runs its JavaScript again. Do not add Service Workers, offline caches, or cache tooling to consumer bundles for this feature. A separate framework renderer owns its own HTTP cache headers.

Check the installed version and `serve --help`. The CLI in 0.5.1 accepts `--preload` (default selected-flow mode) or `--no-preload` (visible screens and an open dialog only). npm 0.5.0 accepts neither flag, preloads the entire atlas, and only exposes `preloadScreens: false` through the embedded API. With preloading disabled, offscreen/closed frames are discarded; test theme changes and state preservation within an open preview. These are viewer options, not JSON fields. Await `instance.ready` for layout, then read `instance.previewsReady` after selecting a flow to await queued attempts. `getState().preload` reports cumulative attempts, not retained frame count. Inspect automated light/dark screenshots; DOM assertions alone are not visual verification.

## Validate and hand off

Run from the user's project with the same root for audit and serve:

```sh
npx --yes ketatlas@0.6.1 discover . --json
npx --yes ketatlas@0.6.1 validate ./tasks/mockups.ketatlas
npx --yes ketatlas@0.6.1 audit ./tasks/mockups.ketatlas --strict
npx --yes ketatlas@0.6.1 serve ./tasks/mockups.ketatlas --renderer --port 60550 --html-port 60551
```

Omit `--renderer` and `--html-port` only for a genuinely static atlas. Audit validates a discovered `atlas.renderer.json` and treats screen URLs as framework routes, but deliberately does not execute the command. The browser check must therefore prove the ready route, every requested screen state, framework scripts/styles, and important interactions through the two-port viewer.

Use `--root .` on both audit and serve if screens or assets intentionally live outside the atlas directory but inside the project. Use `--port 4180` or another free port when necessary. Refresh after file edits; there is no hot reload.

For local projects, fix structural errors, missing assets, and unreachable nodes until strict audit passes. Remote screen/assets URLs produce warnings and are not fetched by audit: when those are intentional, run the normal audit, report the warnings, and verify embedding separately rather than claiming a strict pass. With `audit --json --strict`, check the exit status and warnings as well as `valid`.

When browser automation is available, inspect the map and actual HTML at the declared viewport. Exercise each requested flow's primary route and relevant recovery route, and check for console errors and accidental clipping. Audit alone does not prove interaction, appearance, or complete coverage. If browser verification is unavailable, state that limit and still deliver the auditable files.

Update the project's README with flow-to-screen/state coverage, assumptions, demo inputs, and run commands. Return the output location, the serve command or live preview URL, audit results, and any remaining limitations. Do not claim that a mockup implements production APIs or native application behavior.

## Maintain screen delivery progress

When implementing or reviewing screens, track progress in `atlas.progress.json` beside the bundle manifest. Keep version 1 workflow JSON unchanged. Read `ketatlas progress <name>.ketatlas --json`, preserve existing evidence/check IDs and state references, then update one record with `--set <screen-id> --record <record.json> --expect <revision>`. Re-read and reconcile conflicts instead of overwriting another writer. Run `audit` after updates.

Map task scope to screen IDs before assigning progress. Start unknown coverage at `unassessed`; a mockup is not implementation evidence. Use `planned`, `in_progress`, `in_review`, `implemented`, `verified` with a separate blocker reason/next action. Link PRs and exact merge commits, pin/release evidence, and test evidence at the revision/environment actually checked. Do not infer deployment, full screen coverage or verification from a PR merge or green aggregate CI. Completed acceptance checks need evidence; `verified` requires passed test evidence with revision and environment for every recorded check and no blocker. Track error/recovery states using check `nodes` references. Leave unrelated or unreviewed product surfaces unassessed and say why. See `docs/progress.md` in the package for the complete contract.

Sidebar badges show Checks (completed recorded checks / all recorded checks) and Implemented (Implemented + Verified screens / all unique screens). No recorded checks shows — for Checks. Verified-screen completion stays separate on the project top bar. Do not assign arbitrary weights to intermediate statuses or remove unknown/unscoped screens to inflate completion. Workflow counts deduplicate screen IDs, including variants; process-only flows have no screen percentage.
