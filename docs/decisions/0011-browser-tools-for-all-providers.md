# 0011: Browser tools for Codex, Claude and Cursor

Status: accepted
Date: 2026-09-19

## Context

ADR 0008 limited automatic browser tools to Codex. ADR 0009 made the same
standalone MCP runtime available in packaged apps. Claude SDK and Cursor ACP
already support host-supplied stdio MCP servers, so they do not need a new
browser broker to use that runtime. This decision supersedes the Codex-only
scope and source-checkout prerequisites in ADR 0008.

## Decision

- `modules/browser/BrowserToolLaunch` resolves the bundled runtime first, then
  the source checkout, and applies the same local `browser-tools.json` policy
  for all providers. Disabled, damaged, invalid-origin or unresolved settings
  produce no injected server. Turn payloads cannot supply or override this map.
- Codex keeps its existing app-server configuration and idle reload behavior.
  Claude receives SDK `mcpServers` on every `turn/start`, including resumed
  queries. Cursor receives ACP `mcpServers`, `cwd` and the existing session ID
  on `session/load`, or the same server list on `session/new`. An empty successful
  ACP load response retains the requested session ID. Browser changes apply to
  Claude and Cursor on the next turn; active turns keep their captured policy.
- No per-call approval is added for the injected browser server. Claude grants
  `mcp__ketplus_browser__*` only when the daemon injected that server. Cursor
  checks its per-session injection state and the CLI's structured
  `rawInput.providerIdentifier` / `toolName`; it never trusts a display title.
  Other MCP tools retain normal permission handling. This does not change the
  selected permission mode for filesystem and command tools.
- Node and worker entrypoints are absolute. The worker resolves resources
  relative to its source, so SDK/ACP launches do not change the agent's worktree
  to the browser package directory. Each Claude query closes its workers when
  finished. Providers still own their MCP processes; no in-process linking,
  public-CM dependency or new daemon/cloud browser protocol is introduced.
- The built-in skill resolves from the bundled manifest before a source path,
  including when an installed app opens an unrelated repository. Settings uses
  the runtime's Chrome/Edge/Chrome-for-Testing selection without launching or
  downloading a browser. When automatic download is allowed, readiness means
  the runtime can attempt that download on first use, not that offline launch
  is guaranteed. Source installs need production `playwright-core` dependencies.
- The origin proxy handles socket errors before policy rejection and closes
  upstream connections when their clients disconnect. A rejected CONNECT or
  WebSocket reset must not terminate the MCP worker.

## Consequences

All three providers can use the existing 12 tools, including screenshot and
visual comparison results, under the same origin ceiling and isolated profile.
Codex's transcript image gallery remains unchanged; Claude/Cursor transcript
presentation still follows their existing adapters (full screenshot gallery
parity is not part of this transport change).

The deterministic verification uses fake providers/SDK and local browser pages.
It consumes no provider quota. Real authenticated provider turns remain opt-in.

## Protocol references

- [Claude SDK MCP configuration and allowed tools](https://code.claude.com/docs/en/agent-sdk/mcp)
- [ACP session creation and loading](https://agentclientprotocol.com/protocol/v1/session-setup)

## Alternatives considered

A new shared broker would add lifecycle and approval protocols without being
required for the current stdio worker. User/project MCP configuration files
would persist machine paths outside KetPlus settings and make disabling or
changing browser policy inconsistent, so KetPlus passes configuration directly.
