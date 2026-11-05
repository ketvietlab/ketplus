# 0008: Codex browser tools, stage 0

Status: accepted; stage 0 opt-in superseded by wave 1 (below)
Date: 2026-09-15

Provider scope and source engine prerequisites are superseded by
[ADR 0011](0011-browser-tools-for-all-providers.md): Claude and Cursor now receive
the same runtime and policy. The historical sections below describe earlier stages.

## Wave 1 update (supersedes the stage 0 defaults)

User decisions: browser tools are on by default, all origins are allowed, and
every session uses an isolated temporary profile.

- **Default on.** `browser-tools.json` gains `schemaVersion: 2`; `enabled`
  defaults to `true`. Stage 0 files (no `schemaVersion`) wrote `enabled` and
  `allowedOrigins` on every save, so they carry no explicit choice: they load as
  enabled, and an `allowedOrigins` of exactly `loopback` loads as `*` (a custom
  list is kept). Any save writes version 2, after which `enabled: false` sticks.
- **All origins.** `allowedOrigins` defaults to `*`, passed as
  `KETPLUS_BROWSER_ALLOWED_ORIGINS="*"`. `normalizeAllowedOrigins` accepts `*`
  (any `*` entry wins; other entries are still validated), and still normalizes
  explicit lists with IPv6 brackets. The browser proxy only blocks and records
  requests when the user narrows the list. The profile is always a fresh
  ephemeral Playwright context; the user's browser profile is never used.
- **Runtimes.** `resolveBrowserLaunch` registers `BundledRuntimeProvider` first
  when `modules/browser/src/BundledBrowserRuntime.cpp` exists (CMake sets
  `KETPLUS_HAS_BUNDLED_BROWSER_RUNTIME`), then `SourceCheckoutProvider`. The
  source-checkout launch uses the saved Node path or, when none is saved, the
  Node recorded by the last prerequisite check. When nothing resolves Codex
  starts without the server (fail closed).
- **Settings → Browser** replaces Settings → Experimental: runtime status
  (bundled / source checkout / unavailable with the reason; asynchronous, cached
  as before), "Let agents use a browser" (never gated on the runtime), "All
  websites" or "Only these origins" with the list, the optional source checkout
  path, and a note on the temporary profile.
- **No restart.** `CodexLaunchReloader` (in `modules/browser`) watches
  `browser-tools.json` (file and directory, since saves replace the file) and an
  in-process notification sent by the Settings page after each save. Changes are
  debounced (400 ms), the effective launch arguments are recomputed and compared
  with the running process's, and only a difference triggers a restart. The
  daemon `CodexAdapter` and the direct-mode desktop `CodexAppServer` restart when
  idle (no active turn, in-flight call or queued call) and otherwise defer until
  the turn completes; a turn is active from sending `turn/start` until
  `turn/completed` or a final `error`. A planned restart has no crash backoff
  and is not reported as a stop: the daemon adapter emits `reloaded`
  (`browser_settings`) and the direct-mode app server `serverReloaded`, so idle
  sessions keep their state, get no `runtime.stopped` event or error row, and
  resume their thread on the next turn; the new process starts lazily with the
  new arguments. Desktop in daemon mode relies on the daemon's reload.
- A damaged or oversized `browser-tools.json` loads with the tools off and
  Settings → Browser shows the error. An origin list containing `*` is shown as
  "All websites", matching the launch.
- The built-in `ketplus-browser` skill is offered while the tools are enabled,
  now by default.

The stage 0 text below is kept for history; where it says opt-in, `loopback`
default, `*` rejected or "restart KetPlus", wave 1 applies.

## Context

`apps/browser` implements the canonical browser API (`browser.v0`) with a
JSON-RPC CLI and an MCP facade (`src/mcp.mjs`) over Playwright Chromium. It was
isolated from Desktop, the daemon and packaging until an architecture, security,
dependency and licensing decision existed (`repository-boundaries.md`). Codex
agents need a real browser for UI verification, and Codex app-server already
hosts stdio MCP servers.

## Decision

Stage 0 lets Codex use the browser API as an external MCP server, behind an
experimental, default-off setting, from a source checkout only.

- **Settings.** Settings → Experimental stores `enabled`, `allowedOrigins`
  (default `loopback`; comma-separated http(s) origins; `*` is rejected),
  an optional browser app path (default: auto-detected `apps/browser` above the
  executable) and the detected Node path in
  `$GenericDataLocation/Ketsuite/KetPlus/browser-tools.json` (0600). Desktop and
  daemon run as the same user and both read this file; there is no IPC for it.
  The page detects Node >= 22, `node_modules`, and Playwright Chromium (by asking
  the pinned Playwright for its executable path) and disables the toggle with a
  reason when one is missing. Nothing is downloaded. Detection runs off the GUI
  thread (at most 6 Node candidates, 1.5 s each); the last result is cached in
  `browser-tools.json` under `prereqCache`, keyed by the Node binary mtime and
  size, the `apps/browser/package-lock.json` hash, `node_modules` presence and
  the Playwright browser cache mtime, shown immediately and revalidated.
- **Launch.** `ketplus_browser_tools` (`modules/browser`, Qt Core only) builds
  the extra `codex app-server` arguments used by both the daemon `CodexAdapter`
  and the desktop `CodexAppServer`:
  `-c mcp_servers.ketplus_browser.command="<node>"`,
  `-c mcp_servers.ketplus_browser.args=["<apps/browser/src/mcp.mjs>"]`,
  `-c mcp_servers.ketplus_browser.env={KETPLUS_BROWSER_ALLOWED_ORIGINS="…",KETPLUS_BROWSER_ARTIFACTS="…"}`
  with TOML-escaped strings. The command, arguments and package environment come
  from `ToolPackageRegistry::resolve("ketplus.browser", "mcp-stdio")`, which
  returns a `ToolLaunch` (command, args, env, working directory, package version,
  source); `codexArguments` only adds the policy variables. Stage 0 registers one
  provider, `SourceCheckoutProvider` (the auto-detected or saved `apps/browser`,
  the saved Node path, entry `src/mcp.mjs`). A downloadable QAPack package
  provider (a `qapack.json` manifest, signed archives, bundled Node, Chromium
  downloaded on first use) is planned as another provider and will be decided in
  ADR 0009 (not written yet). Artifacts live in the owner-only
  `$GenericDataLocation/Ketsuite/KetPlus/browser/artifacts`. Settings are read
  at each Codex process start; a running provider keeps its configuration until
  it restarts (restart KetPlus and the daemon to apply immediately). Any invalid
  origin list or missing runtime file launches Codex without the server.
- **Transcript.** `mcpToolCall` items from `ketplus_browser` render as activity
  rows (`browser.<operation>`, tool name, status, sanitized argument summary).
  In direct (non-daemon) mode, inline PNG/JPEG image blocks are written once by
  content hash to the owner-only `transcript-media` cache (at most 4 images,
  16 MiB each). In daemon mode the journal holds `media_ref` blocks that resolve
  to the daemon media store next to `DaemonClient::defaultStatePath()` (no copy);
  a ref whose file was pruned renders a "Screenshot unavailable" placeholder.
  Both are shown through the existing image gallery. Base64 media (image/audio
  `data` and embedded resource `blob`s) never enters model rows or sanitized
  payloads; artifact paths are not exposed by the facade.
- **Launch environment.** The MCP entry also gets
  `mcp_servers.ketplus_browser.cwd` (the package working directory; accepted by
  codex-cli 0.154.0) and forwards `PLAYWRIGHT_BROWSERS_PATH` when set. Env names
  outside `[A-Z0-9_]` are never written as TOML keys. Origins keep IPv6 brackets
  (`http://[::1]:3000`).
- **Settings writes.** The page writes a field only when the user edits it,
  merging into the latest file; a refresh only records the detected Node path.
  While enabled, the toggle stays usable when prerequisites break so the feature
  can be turned off; turning it on is refused (with a message) when prerequisites
  are missing or the origin field is invalid.
- **Guidance.** Users do not need to name the tools or the skill.
  - The MCP server sends a compact summary of the `ketplus-browser` skill as its
    `instructions` at initialize (`apps/browser/src/instructions.mjs`, under 2 KiB):
    the observe/act loop, fencing errors, origin policy, output budget, comparison
    and session cleanup.
  - While browser tools are enabled, the desktop lists `ketplus-browser` as a
    built-in skill in every repository. It is not auto-enabled (that would add the
    full skill to every turn); `$ketplus-browser` adds it to a turn on demand.
  - Root `AGENTS.md` still points agents at the skill. No developer instructions
    are injected. A future QAPack manifest ships the skill with its tool.

## Security model

- The origin ceiling is enforced inside the browser process by its origin proxy
  for navigations, subresources, popups, WebSockets and `browser.session.request`.
  Tool input can only narrow it.
- The desktop Codex path runs turns with `approvalPolicy: never`, and the daemon
  does too under the `full` execution policy (otherwise `on-request`). KetPlus
  adds no browser-specific approval, so the user-configured ceiling is the guard.
  Keep it at `loopback` unless a remote origin is explicitly required.
- `browser.session.request` sends same-session cookies to allowed origins. A
  page that the user signs in to inside the agent browser grants the agent those
  credentials for every allowed origin. Sessions are ephemeral and never reuse the
  user's own browser profile.
- The MCP process runs with the user's privileges and inherits Codex's
  environment; it is not sandboxed beyond Chromium's own sandbox.

## Consequences

- Only Codex gets browser tools; the feature is unavailable in packaged builds.
- The daemon never journals inline MCP media. Before `item.started` /
  `item.completed` is appended, `McpMediaStore` validates image blocks
  (PNG/JPEG magic, 16 MiB, 4 per call; embedded resource image `blob`s are
  treated the same), writes the bytes to
  `<daemon state dir>/media/<sha256>.<ext>` (0700 directory, 0600 files,
  deduplicated by hash) and journals
  `{type:"image", media_ref:"sha256:<hex>", mime, bytes, width, height}`;
  audio, non-image resource blobs and invalid blocks become
  `{type, omitted:true, reason}`. Pruning: per-session ref files are released
  when a session is deleted and, at daemon start, for every ref file whose
  session is no longer in the journal; the 512 MiB cap evicts oldest
  unreferenced media first and evicts media still referenced by a session only
  when that alone cannot fit (logged). The desktop resolves refs from the same
  directory (both run as the same user; hello advertises `media.local_store`),
  so no bytes cross IPC.
- Cloud projection (policy version 2; envelopes cached under earlier versions
  are rebuilt) keeps `mcpToolCall` tool/server/status, media refs, at most 32
  content blocks, at most 256 characters of sanitized result text and a
  sanitized error, plus an argument summary that drops free-text and credential
  keys (`text`, `value`, `script`, `code`, `body`, `content`, `data`, …, and any
  key containing `token`, `secret`, `password`, `auth`, `cookie`, `header`,
  `session` or ending in `key`/`Id`/`Ref`), strips URL user info, query and
  fragment, redacts `token=`/`Bearer` style secrets and caps values without
  splitting surrogate pairs. The same projection applies to mobile/web
  `transcript.restore_window` pages: every event payload is projected before
  fragmenting and encryption (budgets and digests cover the projected bytes); a
  large item whose projection differs from its raw payload is inlined as its
  projection (or `cloud_omitted`), its raw chunk events and partial-item
  transfer reference are dropped, and continuations for its bytes are refused.
  Desktop-local transcript windows are unprojected.
- Changing the setting does not reconfigure a running Codex process (stage 0;
  wave 1 restarts idle processes).

## MCP tool approvals

Workspace conversations run with `execution_policy = ask`, so the daemon starts
turns with `approvalPolicy = on-request`. In that mode codex-cli 0.154.0 asks the
client before every MCP tool call with a `mcpServer/elicitation/request` (form
mode, `_meta.codex_approval_kind = "mcp_tool_call"`). The daemon used to answer
every request other than command/file-change approvals with
`{"decision":"decline"}`, which Codex reported as "user rejected MCP tool call".

- Browser tools do not prompt. The launch config sets
  `mcp_servers.ketplus_browser.default_tools_approval_mode = "approve"` (valid
  values in 0.154.0: `auto`, `prompt`, `writes`, `approve`; an unknown value
  stops Codex from starting). As defence in depth, the daemon and desktop direct
  mode also accept a tool-call elicitation from `ketplus_browser`.
- Tool-call elicitations from other MCP servers become a normal approval card
  (`approval.requested`, `request_type = "mcp_tool_call"`) for desktop and remote
  clients, answered `{"action":"accept","content":{}}` or
  `{"action":"decline"}`; expiry declines.
- Requests KetPlus cannot answer (URL/user-verification or real input
  elicitations, `item/permissions/requestApproval`, `item/tool/call`, unknown
  methods) are refused in their own response shape and journaled as
  `provider.request_declined` (`method`, `reason`, optional `server_name`; no
  arguments). Desktop renders it as a muted "Request declined" note; it never
  fails the turn. The cloud projection passes this metadata-only payload through.

## Stage 1 (planned)

A provider-neutral daemon `BrowserBroker` owns browser processes, per-conversation
origin grants and evidence retention, and exposes the API to Claude, Cursor and
Router API providers through a `ketplus_daemon browser-mcp` stdio shim. Stage 1
requires packaging Node/Playwright (or a replacement runtime), a dependency and
licensing audit, per-conversation origin UI, and interactive approval for origin
expansion and credentialed requests.

## Alternatives considered

- Passing settings over daemon IPC: more protocol surface for a single-user,
  same-machine file; deferred to stage 1's broker.
- Writing `~/.codex/config.toml`: mutates user configuration outside KetPlus.
- Developer instructions via `-c developer_instructions`: would override a
  user's own value; the skill pointer and MCP instructions are sufficient.
