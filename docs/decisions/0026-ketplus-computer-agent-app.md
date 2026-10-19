# 0026: KetPlus Computer agent app for native desktop control

Status: accepted
Date: 2026-10-19

## Context

ADR 0014 lets agents use computer use for Safari and native desktop
applications, but KetPlus supplies no such tool. The Claude sidecar injects only
`ketplus_browser` and loads no user MCP configuration. Claude Code's built-in
`computer-use` server requires an interactive CLI session and a claude.ai login,
so the Agent SDK cannot reach it. The Claude API `computer_toolset_20260801`
requires an API key and a client-side driver. Cursor and Codex have no
host-neutral equivalent.

macOS grants Accessibility and Screen Recording to the responsible process. A
capture or input call made inside an MCP server spawned by a provider CLI, the
Claude sidecar or the daemon is attributed to that parent chain. The system
prompt then names the wrong application, and a provider or daemon upgrade can
reset the grant.

## Decision

- **A separate app owns the desktop.** `KetPlus Computer.app` (bundle ID
  `dev.ketplus.computer`) is a signed Swift application with its own source
  tree under `apps/computer`. Only it calls ScreenCaptureKit, CGEvent and
  AXUIElement, and only it holds the TCC grants. It ships nested in
  `KetPlus.app/Contents/Helpers`, is signed with the same team identity, and
  is updated with KetPlus (ADR 0019). LaunchServices starts it so that it is
  its own responsible process. It shares no code with other Ketsuite
  products.
- **A thin MCP front.** Providers launch `ketplus-computer-mcp`, a stdio MCP
  server inside the app bundle. It makes no capture or input call. It forwards
  each tool call over an authenticated local socket owned by the agent app,
  together with a per-conversation token that the daemon places in the
  server's launch environment. Requests without a valid token are refused.
  `modules/computer` resolves and injects the server the same way
  `modules/browser` does for ADR 0011: Claude `mcpServers`, Cursor ACP
  `mcpServers` and Codex app-server configuration, from local settings, never
  from turn payloads. A disabled or missing app produces no server.
- **The toolset follows the API's tool names.** Member tools use the
  `computer_toolset_20260801` names and inputs (`screenshot`, `zoom`,
  `left_click`, `type`, `key`, `scroll`, `left_click_drag`, `wait` and the
  rest). A later API-key driver can then reuse the agent app. KetPlus adds
  `list_apps`, `request_access`, and `observe`/`act`. `observe` returns an
  accessibility snapshot with node references, and a stale reference fails, as
  in `browser.v0`. Coordinates refer to the returned image; the agent app scales
  to display points.
- **The agent app enforces policy.** Model guidance cannot override it:
  - Access is granted per application and per conversation, in a native
    approval panel that the agent app draws. The panel cannot appear in a
    screenshot.
  - Tiers: Safari can be fully controlled for the Safari cases in ADR 0014.
    Other browsers are view-only, and Chromium work routes to
    `ketplus_browser`. Terminals and IDEs accept clicks only.
    Finder and System Settings show an elevated warning. Keychain Access,
    password managers and a user deny list are refused.
  - Capture is filtered to the windows of granted applications. KetPlus
    windows and the approval panel are always excluded.
  - `type` and `key` are refused while secure event input is active or a secure
    text field has focus.
  - A global Esc event tap cancels the current batch. A visible indicator shows
    while control is active. One conversation holds the control lock at a
    time.
  - Every action is journaled to the conversation with before and after
    thumbnails.
- **No remote-initiated control in the first release.** The daemon withholds
  the token for turns started from KetPlus App or another remote client,
  because no local user can press Esc.
- **Provider permission integration.** The agent app is the enforcement point.
  The Claude sidecar may therefore allow `mcp__ketplus_computer__*` only when
  the daemon injected the server, and Cursor checks the structured provider
  identifier, mirroring ADR 0011. Other tools keep their permission handling.
- **macOS first.** Windows (UI Automation, SendInput, Windows.Graphics.Capture)
  needs its own decision and is out of scope here.

## Consequences

Swift becomes a build language for one target. CMake builds it with Ninja, and
signing covers the nested app. Developers grant the two TCC permissions once to
`KetPlus Computer`, independently of provider CLIs and daemon restarts. The
agent app and the MCP front share a versioned local protocol, `computer.v0`,
which must be specified and tested before the tools are injected. ADR 0014's
routing text changes from "provider computer use" to `ketplus_computer`.

Deterministic tests use a fake capture/input backend and a Qt fixture
application. Real TCC grants cannot run in CI and stay a manual check. No test
consumes provider quota. Prompt injection from on-screen content is reduced by
the per-app grants and the window filter, but is not eliminated.

Implementation started with a spike: a signed agent app with `screenshot` and
`left_click`, driven by a provider's MCP client. The spike had to show the TCC
prompt naming `KetPlus Computer`, the grant surviving relaunch and upgrade, and
the latency and token cost of each step. It met these criteria, so the decision
was accepted. The results follow.

## Spike results (2026-10-19, macOS 26.2, `apps/computer`)

- System Settings lists "KetPlus Computer" as its own Screen Recording entry.
  When the MCP front starts the agent with `open -g -a`, the agent's parent is
  launchd (`parentPid` 1).
- The agent was signed with Developer ID (designated requirement: bundle ID
  plus team). The Screen Recording grant stayed valid after the CDHash changed
  and after the agent was relaunched.
- Capture excluded a full-screen KetPlus window (`dev.ketplus.editor`). The
  menu bar still shows KetPlus's menus while KetPlus is frontmost, so P1 must
  also crop or filter the menu bar.
- A 1372x887 PNG is about 0.5 MB and about 1.6k image tokens. A screenshot
  takes about 180–200 ms with the agent running and about 450–530 ms when the
  call also launches it.
- Claude Code 2.1.278 ran `claude -p` with `--mcp-config` and
  `--allowedTools mcp__ketplus_computer__screenshot`, received the 1372x887
  image and described the visible windows. It reported that no KetPlus
  transcript was visible, and the run cost US$0.06. When Claude's process tree
  started the stopped agent, the agent still ran under launchd and kept its
  Screen Recording grant.
- The agent read the Accessibility grant only after a relaunch. The
  `scripts/fixture-target.swift` window received both a scripted `left_click`
  (77 ms) and one that Claude chose on its own: it located the button in the
  screenshot and clicked image pixel (686, 281). The run took 5 turns and cost
  US$0.21.
- Not yet verified: injection through the daemon and Claude sidecar, multiple
  displays, and any periodic Screen Recording re-prompt.

## Alternatives considered

- Reusing KetViewer's capture and input code. KetViewer is a separate commercial
  product with its own release cycle, and coupling the two would tie KetPlus
  releases to it.
- An MCP server that captures in-process. It is simpler, but TCC would attribute
  access to whichever provider CLI or daemon launched it.
- Claude Code's built-in `computer-use` server. It is unavailable through the
  Agent SDK and to Cursor and Codex.
- Only the API `computer_toolset_20260801` path. It needs an API key or Max/Team
  API credits and still needs a local driver. It stays a later option on top of
  the same agent app.
- Third-party agents or community MCP servers such as OpenClaw. Their default
  security posture and skill supply chain do not meet the daemon's trust
  boundary.
