# KetPlus architecture

## Boundaries

Linux remote-host topology is specified in
[`linux-headless-daemon.md`](linux-headless-daemon.md) and ADR 0022. The server
runs the daemon independently; remote clients use the encrypted Control
relay. `ketplus_remote_control` prepares a Qt client backend without changing
Desktop UI or its current local-daemon composition.

```text
Qt application shell
    -> editor adapter
        -> Scintilla + Lexilla
        -> native persistent typography settings
    -> document core
    -> workspace explorer
        -> QFileSystemModel (lazy directory loading, no content index)
    -> Agent Workspace (lazy)
        -> Qt Quick scene graph (Metal on macOS)
        -> virtualized chat transcript and repository/conversation sidebar
        -> per-worktree conversation registry
            -> one long-lived Codex app-server JSONL process
            -> persistent Codex thread id per worktree
        -> contextual editor or terminal panel (closed by default)
    -> Git service
        -> system Git CLI via short-lived asynchronous processes
    -> terminal panel (lazy)
        -> libvterm screen and keyboard model
        -> PTY process backend
            -> forkpty (macOS/Linux)
            -> ConPTY (future Windows backend)
    -> reusable settings shell from KetPlus CM
        -> shared appearance snapshot and built-in theme preview
        -> private pages remain consumer-owned through SettingsPage
```

- `apps/desktop/src` owns windows, menus, tabs, and user interaction.
  `RepositoryRouter` routes repository and file activation to the window owning
  the file's exact linked worktree, using the Git common directory as repository
  identity. Its local-socket request validates the worktree identity and file
  existence before switching a window, opening the file, or requesting its diff.
  A window registers after opening a folder. The Git lookup behind that
  registration runs on the router's own bounded pool, which drains when the
  router is destroyed, and only the window's latest folder is registered, so
  opening a folder never waits for Git.
  Desktop repository pickers use a separate disposable SQLite metadata cache
  for immediate names, with Git validation in the background. IPC routing
  trusts cached metadata only when the identity lookup times out, and only if
  the checkout owning the path still belongs to the cached identity
  ([ADR 0004](decisions/0004-repository-navigation-metadata-cache.md)). Batch reads (the sidebar's recent
  repositories) share one SQLite connection. Those validation jobs run on a window-owned
  bounded thread pool, which is drained before child services and QApplication
  tear down so Git processes and SQLite connections cannot escape shutdown.
  See ADR 0004. Conversation-owned worktrees use a second desktop SQLite map
  (`conversation-worktrees.sqlite`) written at conversation create and turn
  completion; see ADR 0010. The daemon journal is not the source of that map.
- `core/src/editor` adapts Scintilla to KetPlus concepts. Scintilla API calls must not
  leak into the application shell. Editor typography is normalized and persisted
  here, then shared with read-only diff views.
- `core/src/core` owns documents and persistence. It must not depend on Qt Widgets.
  A `Document` remembers the size and SHA-256 of the bytes it last loaded or saved.
  Saving over a file that changed since then (an agent's edit, a branch switch) asks
  to overwrite, reload or cancel (`editor/DiskChangePrompt`). Clean tabs are refreshed
  when the app becomes active. Reloads detect the encoding again unless the user chose
  one. Files above 256 MiB are refused. Text is loaded with an explicit length, so NUL
  bytes survive a save. A hibernated tab stays read-only until its file is read again,
  and Save or Save As reads it first and refuses if that fails.
- `core/src/workspace` owns the lightweight file tree and filesystem actions. It
  discovers entries only when folders are expanded and does not build an index.
  Quick Open and Search list a Git working tree with
  `git ls-files --cached --others --exclude-standard`, so `.gitignore` decides what
  they cover; other folders are walked without hidden, dependency and build folders.
  Explorer renames retarget open tabs, and trashing closes clean tabs of that path.
  Search decodes files as the editor does. Replace All is off when results reached
  the match limit.
- `modules/ai/src` owns the GPU-rendered Agent Workspace shell. QML consumes semantic
  KetJS-derived tokens from `ThemeManager`; it does not own file persistence,
  terminal processes, or editor state. `AgentSessionManager` owns the transcript
  and per-worktree lifecycle. Its `CodexAppServer` compatibility facade maps the
  existing UI lifecycle to the authenticated `DaemonClient`; only the daemon
  owns Codex JSONL transport and provider processes. Switching worktrees or
  conversations changes the visible session without stopping turns elsewhere.
  The Qt Core-only `agent/ConversationTitle.h` rule is shared with the daemon
  so both clients and durable sessions derive the same short first-request title.
- `core/src/git` owns read-only repository discovery, status/diff requests, porcelain
  parsing, and linked-worktree discovery. It never stages, commits, or changes
  the checkout. The Pro desktop shell separately owns the Agent Workspace's
  narrowly scoped `git worktree add -b` and `git worktree remove` (force only
  when the user opts in from conversation deletion of stored linked worktrees)
  processes, including bounded output and a
  timeout; this does not expand the public Git service's mutation surface.
- `core/src/terminal` owns the embedded terminal surface, VT state, scrollback, and
  process transport. `PtyProcess` isolates platform process creation from the
  renderer so a ConPTY backend can be added without changing terminal UI code.
- `core/src/app/SettingsDialog` owns the reusable settings shell and
  public appearance persistence. The Pro desktop recomposes its live theme
  and typography controls inside the non-modal, full-page Két settings
  workspace, while the hidden public host continues to own preview, validation,
  reset, and save behavior. Private chrome type embeds KÉT Sans 1.1 (the hinted
  static TTF Light/Regular/Medium/Bold faces), then falls back to the bundled Inter face and the platform UI font; the Pro shell rewrites the live QWidget
  family after ThemeManager applies, and Agent Workspace reads the same
  `InterfaceUiFont` resolver for `uiFont`. The product sidebar (`ActivityBar`)
  and Agent Workspace transcript text add a +2px bias on TypeScale sizes at
  every Interface zoom step (glyph steps stay unbiased). Transcript mono type uses the
  Settings editor font family (refreshed on appearance updates; KÉT Sans Mono when empty). Agent
  Workspace QML sizes come from `TypeScale` named steps (7 text + 2 glyph)
  merged into the theme map and scale with `appearance/interfaceFontSizePixels`
  (Medium = 14). Private QWidget chrome QSS sizes also flow through
  `TypeScale` token expansion (`{{micro}}` … `{{display}}`, `{{glyphSmall}}`,
  `{{glyphLarge}}`); temporary `px*` aliases were collapsed onto those steps.
  The CM Theme sheet is scaled via `TypeScale::scaleLiteralFontSizes` until a
  public token migration, preserving the already-resolved base QWidget size.
  Product geometry uses explicit Medium-based `chrome_geometry` bindings and QML
  `ChromeMetrics.px` under the same zoom factor. Saved snapshots publish to all
  live MainWindow instances before theme signals and propagate to the additional editor,
  diff, preview, and Agent Workspace terminal surfaces without duplicating
  persistence code.
  The private Archived page owns conversation/worktree visibility restoration;
  it stays open after restoring and can stop a still-running archived turn.
- The desktop shell owns the full Worktrees workspace. It combines linked Git
  worktrees from the public read-only Git service with private conversation and
  running-session counts, while create/archive/delete actions remain explicit
  private-shell workflows.
- Terminal creation is lazy. Hiding its panel only detaches the visible area; it
  does not stop the shell. The first implementation retains one session, while
  the planned session registry will retain one or more sessions per worktree.
- `modules/atlas/src` owns Atlas process/WebView integration and the private
  `AtlasIndex`. The index scans only the active workspace/worktree for exact
  `*.ketatlas/atlas.json` bundles; switching worktrees replaces its root rather
  than expanding into linked worktrees. Atlas descriptors retain screen IDs,
  titles, URLs, and resolved local paths for future per-screen audit, while
  KetAtlas itself reads HTML/CSS/assets on demand during audit and serve. When the
  index refreshes its watch set, unchanged watches remain registered so queued
  filesystem events survive asynchronous scan completion. A cheap metadata
  verifier polls the selected root and already indexed manifests without parsing
  or spawning Git; it schedules a normal bounded refresh when macOS delivers a
  filesystem event late or drops it under load. The active Atlas Builder applies
  the same bounded fingerprint fallback to bundle assets so preview changes do
  not depend solely on FSEvents. If the
  CLI is unavailable, `AtlasTab` owns the explicit, user-triggered npm install
  flow and executable picker. The conversation list offers an explicit 0.6.1
  update after a managed-viewer version error. Opening an Atlas never installs
  software by itself.
- A conversation header opens an Atlas list in the Agent detail tabs. The sidebar
  has no Atlas destination. `ManagedAtlasViewers` launches KetAtlas 0.6.1 in
  managed mode for explicit external-browser viewing, starts the declared native
  renderer only when the bundle has `atlas.renderer.json`, tracks running viewers
  across worktrees, and stops them on a browser close or KétPlus Stop command.
  Edit opens the existing in-app Atlas Builder in the current worktree.
- Completed assistant messages can show compact Atlas result cards from explicit
  local Markdown links to `*.ketatlas/atlas.json`. The original message remains
  the receipt source for live and replay; code blocks, raw HTML, remote URLs and
  filesystem discovery do not create receipts. Parsing is bounded to 128 K characters and
  eight distinct links per message. This attributes a link to the conversation,
  not proof that the conversation created or validated the bundle.
  The desktop composition reads manifest metadata through `AtlasIndex` (4 MiB
  maximum), fenced to the current canonical worktree, and routes explicit Edit /
  Open actions to the existing builder / managed viewer. Both actions recheck
  availability. Relative links after an execution-context change fail closed
  until historical source roots are available; the Atlas list remains usable.
  Missing or invalid manifests show an unavailable state, never a validated badge.
- Atlas creation is composed by the proprietary desktop shell because it crosses
  the AI and Atlas module boundaries. Atlas tab add/close controls use theme-colored
  Lucide SVGs; the add button occupies reserved corner space, and close controls
  are installed for both picker and builder tabs. Creation has a single AI entry point.
  `AtlasBuilderTab` embeds a compact
  `AgentWorkspaceView`, keeps a dedicated conversation in the active worktree's
  registry, and asks `AtlasIndex` to identify the first new valid bundle. The AI
  session backend adds the versioned KetAtlas output contract to the user's
  prompt; `AtlasTab` remains unaware of agent sessions.
  The builder keeps the preview canvas at full width and composes a right-inset
  AI Chat overlay capped at 400 px before and after the first preview. The chat
  remains a persistent sibling and is raised after preview geometry changes;
  native WebView stacking remains part of the opt-in native acceptance lane. Bundle-local
  file/directory watches debounce edits for 750 ms and reload only the existing
  preview browser, preserving its navigation state and the ongoing conversation.
  Partial JSON writes retain the last preview until the manifest is parseable.
  A prolonged physical manifest deletion enters explicit bundle recovery, while
  a present but invalid candidate shows that the last valid preview is retained.
  A persistent toolbar AI toggle hides the whole chat panel without stopping its
  agent or recreating the view. The preview then occupies the full available
  width. The toggle pulses while running and labels pending questions without
  automatically opening chat or stealing focus; Stop remains a separate action.
  The preview's stable bundle path plus selected flow ID and Builder conversation
  form the prompt context. `@Flow` is a preferred focus hint, never a filesystem
  isolation or authorization boundary.
- The root CMake build selects the edition (ADR 0029). Pro composes its targets
  with the `KetPlusCM::*` targets of `core/`. Product code must not include files by reaching through
  `core/`; exported include paths and targets form the boundary.

## Agent runtime

The first provider is the installed Codex CLI in `app-server --stdio` mode. The
daemon starts it on the first prompt and keeps it alive independently of the
Desktop connection. Desktop starts the daemon as a detached process when needed,
discovers sessions by canonical worktree, and attaches after reconnect. The
daemon journal replays normalized user, assistant, item, approval, turn, and
current-window occupancy (`usage.context`) events; UI updates remain incremental.

Turns use the selected read-only, workspace-write, or full-access sandbox. The
daemon's approval broker exposes user-input and approval requests to one
authoritative responder with expiry. Every provider process is a
`ProviderAdapter` (`apps/daemon/src/providers/ProviderAdapter.h`): availability,
calls, catalogs, approval replies, runtime status and capabilities. SessionHost keeps
them in one registry keyed by adapter name instead of a pointer per provider. Codex
also forwards raw app-server notifications that SessionHost projects; Claude and
Cursor translate their own streams into canonical events. Codex remains the default
provider. A Claude adapter can run beside it: the daemon supervises a Claude Agent SDK
sidecar, normalizes its stream into the same journal events, and Desktop only
selects the provider. The sidecar is packaged as JavaScript plus production
`node_modules` in `KetPlus.app/Contents/Resources/claude-sidecar` (not
`Contents/Helpers`; codesign treats every file there as nested code). It does
not ship a second Node: the daemon launches `index.mjs` with the bundled
`ketplus-browser` Node at `Contents/Resources/ketplus-browser/bin/node`.
Every build stages that tree with
`apps/daemon/providers/claude-sidecar/package-sidecar.mjs` (a build-host Node and
network access for `npm ci`, or `KETPLUS_CLAUDE_SIDECAR_DIR` pointing at a staged
tree); there is no option to leave it out, and the bundle check fails without it.
`cmake --install` copies the staged sidecar and `assets/agent-skills` after the
bundle and replaces the installed copies, so neither keeps files from the tree
the bundle was last built with (#124), as for the browser runtime (#94).

A Cursor adapter can run beside Codex and Claude when the user-installed
Cursor CLI is available. Discovery uses `PATH`, `KETPLUS_CURSOR_AGENT`,
Unix `~/.local/bin/agent`, and on native Windows
`%LOCALAPPDATA%\cursor-agent\agent.cmd` (the installer often skips PATH for
GUI processes). The daemon spawns `agent --model <id> acp` when a model other than Auto is
selected (wrapping `.cmd`/`.ps1` on
Windows), speaks ACP JSON-RPC on stdio, and normalizes `session/update` and
`session/request_permission` into the same journal events. Model ids come from
`agent models`. Effort suffixes are grouped into families; the adapter maps the
selected family plus effort back to a CLI id before `--model` and
`session/set_config_option`. After `session/new` the adapter also sends
`session/set_config_option` for the selected model. The CLI is never
bundled; authentication stays with `agent login` or `CURSOR_API_KEY` /
`CURSOR_AUTH_TOKEN`. The daemon does not call ACP `authenticate`. Override the
binary with `--cursor` or `KETPLUS_CURSOR_AGENT`.

### Provider CLI compatibility

The daemon's `providers/compat` module checks the user-installed Codex and
Cursor CLIs against `provider-compatibility.json` before each launch (ADR 0015).
It runs `--version`, caches the result by executable fingerprint, and classifies
it as `supported`, `newer`, `outdated` or `unknown`. An outdated CLI is not
started; the turn fails with the update command. Newer or unreadable versions
run and journal one `provider.runtime` notice per session. The
`provider.runtime_status` query also reports Claude's bundled SDK and Claude
Code versions, and Settings > Providers shows the result. The supported window is
releases from the last three months, reviewed monthly.

Features are gated on capability names, never version numbers. The manifest
maps each version range to a dialect with a baseline capability list; adapters
narrow it when ACP `initialize` reports a capability as `false` or a call is
answered with `-32601`. The daemon refuses steering without `turnSteer`, Cursor
skips `session/load` without `threadResume`, and Desktop's
`ProviderRuntimeStatus` hides `/compact` without `threadCompact` and disables
Fork this conversation without `threadFork`.
Unknown Codex input is handled safely (ADR 0015 §5). `SessionHost` counts
notifications it does not act on per method and reports the counts as
`ignored_notifications` on the Codex entry of `provider.runtime_status`. A known
notification missing a required id, type or delta is skipped and journaled once
per session as `provider.protocol_error` (`provider`, `method`, `missing`).
A Codex message over the 16 MiB line limit is discarded as it streams in,
without stopping the shared app-server. A skipped notification is journaled as
`provider.protocol_error` (`provider`, `method`, `oversized_bytes`); a skipped
`turn/completed` still ends its turn, a skipped response fails only its call,
and a skipped server request is answered with a JSON-RPC error.
Unknown server requests are declined with `provider.request_declined`, and
unknown item types are journaled unchanged.
`daemon.hello` negotiates a protocol minor and returns named features
(`provider.runtime_status`, `session.fork`, `transcript.window/1`,
`transcript.large_event/1`)
while the envelope major stays 0.

## Shared browser tools

`modules/browser` resolves one host-controlled MCP launch for Codex, Claude and
Cursor. The browser remains a separate stdio worker. Claude SDK receives the
server map per query; Cursor ACP receives it on session creation and loading.
The same local settings control origins and enablement for all providers;
settings changes wait for the next Claude/Cursor turn. Source readiness probes
share engine selection with the worker, and built-in skill discovery uses the
bundled manifest first. See [ADR 0011](decisions/0011-browser-tools-for-all-providers.md).

The MCP worker returns local PNG paths with capture metadata. Agent guidance
allows use/copy of those original files for an authorized task before session
cleanup; filesystem permissions still come from the provider's selected mode.
This does not grant arbitrary destination writes through the browser protocol.
See [ADR 0023](decisions/0023-browser-screenshot-files.md).

Provider `imageView.path` and `imageGeneration.savedPath` files are also copied into
the daemon's bounded, owner-only media store before their item is journaled. Their
`item.result.content` carries the same image references as MCP results, so Desktop
and existing mobile media readers do not depend on a browser's temporary capture
directory. Relative paths resolve against the conversation worktree; only regular
PNG/JPEG files within the existing media byte limit are retained. Session-scoped
media authorization, fork references and capacity eviction remain unchanged.
Older path-only history still uses its original path; an already deleted original
without a retained copy cannot be recovered by this change.

### Agent browser routing

`modules/browser/BrowserAgentInstructions` owns compact host routing guidance.
Provider adapters derive it from the same captured launch used for MCP injection,
never from caller-supplied turn metadata. Codex applies it through thread developer
instructions, Claude through a system-prompt append, and Cursor through a separate
ACP context block. The user prompt, attachments and daemon journal are unchanged.
The existing large provider transports receive only a call to this shared policy;
policy text and composition live in the browser module. See ADR 0013 for limits.
The policy allows computer use only for Safari (which the Chromium-only
`ketplus_browser` cannot drive) and for native apps that are not browsers; see
ADR 0014.

## Computer use

`apps/computer` builds `KetPlus Computer.app` (Swift), a separately signed app
nested in `KetPlus.app/Contents/Helpers`. It is the only KetPlus process that
captures the screen or posts input, so macOS attributes Screen Recording and
Accessibility to it. It also enforces per-conversation app grants, app tiers,
the control lease and the kill switch. Input runs in the background through the
accessibility tree and keyboard events sent to one process; the real pointer is
used only on request. Granted apps move to a work display, a secondary
display by default, which screenshots show. `ketplus-computer-mcp`, inside the
same bundle, is the stdio MCP front that providers launch. It forwards
`computer.v0` requests over a user-only socket. `modules/computer` owns the
opt-in settings, the host key and per-conversation tokens, the remote-turn
markers, and the routing text. Claude receives the server per query, and
Cursor receives it on session creation and loading. Codex does not receive it
yet. See [ADR 0030](decisions/0030-ketplus-computer-agent-app.md) and
[ADR 0031](decisions/0031-background-computer-use.md) and
[ADR 0032](decisions/0032-computer-use-work-display.md).

## Local daemon slice

Worktree selection is provider-specific. Codex can select the next turn's cwd during
an active turn. Claude supports selection between turns, with startup, shutdown,
pending input and live background tasks fenced by the daemon. Its next SDK query
resumes the same thread in the selected cwd, after copying the newest persisted
project transcript there; returning to a former cwd must not restore stale history.
The sidecar independently rejects a cwd change while its background tasks run.
The existing `switch_supported` and `switch_available` fields drive both clients;
no mobile binary or relay contract change is required. Cursor remains unsupported.

The independent `ketplus_daemon` executable now owns local Codex, Claude, and
Cursor adapters, normalized session events, approval arbitration, and a SQLite WAL replay
journal. Its user-restricted `QLocalSocket` protocol is defined by ADR 0002 and
tested with client disconnect/reconnect. `modules/remote` implements the Desktop
client, daemon discovery/startup, reconnect, idempotent request retry, and replay
pagination. Desktop's existing presentation model is retained behind a protocol
translation facade; raw Codex JSON-RPC is not exposed over IPC.

Remote session creation is a worktree-scoped command and does not require an
existing session key. The daemon advertises only canonical worktrees retained in
its local journal, decrypts a request to its enrolled X25519 device key, resolves
the opaque repository/worktree identity back to that catalog, and passes only
the resolved existing directory to the ordinary `session.start_turn` path. Its
authoritative session ID and generation are sealed directly to the requesting
paired device; supplied filesystem paths are never accepted from the cloud.

## Principles

1. Keep startup synchronous work minimal.
2. Load heavyweight services only when a feature needs them.
3. Keep parsing, indexing, and project search away from the UI thread.
4. Prefer process-based extensions over an unstable in-process C++ plugin ABI.
5. Measure startup time, memory, and large-file behavior before adding features.
6. Keep Git optional and read-only; terminal workflows remain the source of truth
   for operations that mutate a repository.

## Suggested milestones

1. Editing foundation: robust encodings, line endings, recent files, session
   restore, and drag-and-drop.
2. Workspace: file watcher, project search powered by ripgrep, and command
   palette. The non-indexing file tree is implemented.
3. Code intelligence: Tree-sitter and optional LSP processes.
4. Extensions: versioned JSON-RPC protocol with out-of-process plugins.

The Desktop worktree picker controls the file/Source Control root independently of conversation identity and daemon execution context. Selection never sends session.worktree.switch or navigates the active conversation. The picker uses a searchable scrolling collection with incremental reads rather than page buttons. Transcript continuations publish each page independently with adjacent read-ahead; a fenced 15-second deadline clears abandoned loading state and exposes retry without replacing existing rows.


### Claude provider credentials

[ADR 0026](decisions/0026-claude-api-key.md) defines the private, host-scoped BYOK
boundary. `ClaudeCredentials` owns storage and fixed-endpoint validation;
`LocalIpcServer` routes authenticated local credential commands without SessionHost
journaling; `ClaudeAdapter` supplies settings only to its sidecar's new SDK query.
The desktop composes the native credential form in Settings → Providers.

### Shared model-backed Két UI

Explorer and Source Control render Két row recipes through Qt item delegates, retaining lazy filesystem loading, stable selection and inline rename. Explorer creates entries through an inline Két field and only writes after confirmation. Git mutations live in `GitService`: paths use the same canonical-entry normalization as diff requests, use literal Git pathspecs, serialize operations and suppress stale completion notifications after a workspace switch. Commit acts on the existing index only; it never stages files implicitly.

`terminal/TerminalTab` now owns the shared session/view lifecycle for the standalone Terminal window and the embedded multi-tab `TerminalPanel`. Closing a tab stops only that session; hiding the panel preserves sessions. The shared terminal target exports the tab implementation, so Pro does not depend on the standalone app target.

### Pro sidebar presentation

The native sidebar composes Két NavItem, GroupHeader, ConversationRow and UpdatePill controls; the standalone Agent Workspace uses their QML counterparts. Conversation selection, context actions, collapsed repositories and preview limits keep their existing session-manager routing. Tags are rendered and filtered from supplied metadata; this UI does not invent tags or infer a finished turn's approval intent. The `awaitingReply` projection has a distinct kit state when supplied.

The sidebar reads the local daemon's `storage.size.read` capability through the existing CodexAppServer facade. The adapter accepts only read/apply operation names and requires a ready daemon advertising the exact operation. A visible window refreshes advisory status at most once a minute, with one in-flight request, a deadline and stale-response fencing. It shows the warning only from an authoritative successful response, hides stale status on disconnect, and links to Settings. This does not change retention policy or start a provider turn.

Question and approval presentation uses the shared kit question-card recipe.
Answer IDs and pending/closed state still come from the transcript broker; the
UI preserves drafts and guards repeated submission. Completed background task
receipts can be hidden locally per conversation/task incarnation; this never
deletes daemon task history or stops a running task.

The standalone Agent Workspace sidebar also uses repository → conversation
composition. It does not render a worktree rail; the execution picker and
Worktrees page own checkout selection and management. The native shell retains
its multi-repository grouping.

Pro Quick search wraps the kit CommandPalette in a transparent native popup so it
stays above the embedded QQuickView. Product filtering retains ranked name/path
matches and Atlas IDs; unavailable entries cannot activate and produce an explicit
empty-state explanation when they are the only matches. Context menus use kit
recipes and route Change worktree through the existing revisioned execution picker.

Compact conversation window (Shift+Cmd/Ctrl+M or the header icon) is desktop
presentation only: the same view, conversation and composer draft remain mounted.
It hides sidebar/details, restores prior geometry on exit and expands when opening
a file detail. This is distinct from the provider context-token compactWindow in
ADR 0021 and from Atlas Builder's restricted compactMode.

Worktree and Connections presentation consumes kit buttons, inputs/selects,
status badges and palette/profile values. The execution picker retains its
paginated QListWidget model and paints visible rows from treeItemRecipe; search,
revision fencing and acknowledgment-driven context changes retain their existing
ownership. Pairing confirmation and changed-claim rejection remain daemon-backed.
