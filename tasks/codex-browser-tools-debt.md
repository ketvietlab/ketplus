# Codex browser tools – known debt (stage 0 / wave 1)

Status: accepted debt, tracked for stage 1 (see
`docs/decisions/0008-codex-browser-tools-stage-0.md`).

## No origin ceiling by default

Wave 1 turns browser tools on by default with `allowedOrigins = *`. The origin
list is no longer a guard unless the user narrows it under Settings → Browser:
an agent (or a prompt injection in a page it reads) can navigate to and send
requests to any website. The remaining guards are the isolated temporary
profile (no cookies, storage or sign-ins from the user's own browser; discarded
when the session ends) and Chromium's sandbox. There is no approval prompt.

- **Resolution options for stage 1:** per-conversation origin grants, an
  approval for first navigation to a non-loopback origin or for credentialed
  `browser.session.request`, and blocked-request reporting in the transcript.

## Credentials shared with every allowed origin

`browser.session.request` and any login performed inside the agent's browser
session share one cookie jar. If a user signs in inside that session, the agent
can use the resulting credentials against every allowed origin — with the
default `*`, any website — not only the origin where the sign-in happened.
Credentials and cookies live only in the temporary profile of that session.

- **Why it is accepted now:** sessions never reuse the user's browser profile
  and are ephemeral; a user must sign in inside the agent session first.
- **Resolution options for stage 1:**
  - scope cookie jars per origin (one browser context per origin);
  - disallow `browser.session.request` from models and perform login handshakes
    on the host side only;
  - require an approval when a session first receives a `Set-Cookie` from an
    origin other than the one being tested.
- **Owner:** browser broker (stage 1).

## Approval policy

Desktop turns run with `approvalPolicy: never`, and with the default `*` there is
no origin guard for browser actions either. Workspace conversations
(`execution_policy = ask`) run with `on-request`, but browser tools are launched
with `default_tools_approval_mode = "approve"` and the daemon auto-accepts
`ketplus_browser` tool-call elicitations, so they still never prompt (ADR 0008,
"MCP tool approvals"). Behaviour is intentionally kept for wave 1.

## Settings reload restarts

Applying a settings change restarts an idle Codex process. The restart is a
planned reload, not a provider exit: the daemon journals no `runtime.stopped`
event, keeps session states (nothing goes `offline`) and the desktop shows no
error row; sessions re-attach through `thread/resume` on their next turn. A turn
counts as active from sending `turn/start` until `turn/completed` or a final
`error` (`willRetry` false), so a reload never lands between the `turn/start`
response and `turn/started`. A busy provider still defers the restart until no
turn, call or queued call is outstanding, which can be indefinitely on a
constantly busy provider.

Launch-time source-checkout resolution uses the saved Node path, falling back to
the Node recorded in the prerequisite cache. That fallback only helps when a
cache exists, i.e. Settings → Browser has run a check at least once; default-on
users of a source checkout who never open the page get no browser. The bundled
runtime does not need Node from settings and is unaffected.

A `browser-tools.json` that exists but cannot be parsed, is not a JSON object or
exceeds 64 KiB loads with browser tools off (fail closed) and Settings → Browser
reports it; a refresh does not rewrite it, only an explicit save does.

## MCP media store follow-ups

Resolved: the daemon no longer journals inline MCP media. Image blocks are
written to the owner-only `<daemon state>/media` store and journaled as
`media_ref` blocks; audio and invalid blocks are dropped; cloud projection keeps
only tool identity, status, an argument summary, media refs and 256 characters
of text. Remaining:

- Thumbnails and a `media.fetch` command for remote/mobile clients.
- Retention is per session id (not per generation) and LRU is by write/dedupe
  time, not by desktop reads. Events journaled before this change keep their
  base64 locally until the session is deleted (cloud upload and restore project
  them away).
- `structuredContent` of MCP results is journaled unchanged; the browser facade
  does not put media there today.
- Desktop resolves refs from `DaemonClient::defaultStatePath()`. A daemon started
  with a custom state path that the desktop does not share shows placeholders.
- Pre-existing (not introduced here): live event upload sends
  `transcript.large_chunk` payloads unprojected, so a large command or MCP item
  reaches the (end-to-end encrypted) event stream raw. Restore pages are fixed;
  the upload path needs the same large-item decision and a mobile-compatible
  chunk shape.

## Tool package registry

`ToolPackageRegistry` tries `BundledRuntimeProvider` first when
`modules/browser/src/BundledBrowserRuntime.cpp` exists (CMake defines
`KETPLUS_HAS_BUNDLED_BROWSER_RUNTIME`; it is constructed with no arguments), then
the source-checkout provider. `ToolLaunch`
`workingDirectory` is passed as `mcp_servers.<name>.cwd`; `packageVersion` is not
shown in Settings. The QAPack provider (manifest, signature verification, bundled Node,
Chromium on first use) needs ADR 0009 first.

## Formatter size

`modules/ai/src/agent/AgentMessageFormatter.cpp` exceeds the 500-line review
trigger after adding browser rows; split browser/MCP formatting into its own
unit.

## Built-in skill source

The built-in `ketplus-browser` skill is read from the source checkout
(`apps/browser/.claude/skills/ketplus-browser/SKILL.md`) and its summary is
duplicated in `apps/browser/src/instructions.mjs`. Keep both in sync until the
QAPack manifest declares the skill with the tool package.
