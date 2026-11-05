# 0013: Automatic agent browser routing

Status: accepted, amended by [ADR 0014](0014-computer-use-for-safari-and-native-apps.md)
Date: 2026-09-19

## Context

Registering a browser MCP server makes tools callable, but does not reliably tell
an agent to choose them over its provider's browser or shell automation. The full
browser skill is opt-in, and MCP initialize instructions may not be sufficient
for the agent's initial tool selection.

## Decision

`modules/browser/BrowserAgentInstructions` supplies compact routing policy from
host-owned provider state. Real-browser work uses `ketplus_browser`; missing or
failed tools, unsupported operations and origin denials are reported without an
automatic switch to another browser or ad-hoc automation. Unrelated coding and
native desktop UI tasks remain unaffected.

- Codex receives `developerInstructions` on `thread/start` and `thread/resume`,
  in both direct and daemon transports. Caller-provided developer instructions
  are preserved. Availability follows the running process's captured launch
  arguments, including while an idle reload is pending, rather than newer settings
  that the process has not received. The instructions persist for subsequent
  turns; reloads resume threads with the new policy.
- Claude receives host-generated `browserInstructions` beside `mcpServers` on
  each query, including resume. The SDK appends them to its `claude_code` system
  prompt. Caller-supplied policy metadata is overwritten at the daemon boundary.
- Cursor ACP has no system-prompt field. Each prompt carries a separate leading
  text block derived from the session's injected MCP state. User text and image
  blocks remain separate and unchanged. KetPlus does not journal the host block
  as user-authored conversation text.
- Disabled, invalid or unresolved browser configurations produce explicit
  unavailable guidance, replacing stale instructions when a session resumes.
- MCP initialize instructions and the built-in skill describe the same routing
  expectation. Agents need not select the full skill before using the browser.

The host policy, MCP initialize instructions and skill require agents to track and
close every browser session they create when browser work ends, before their final
response, including failed, blocked and abandoned work. Agents preserve needed
evidence before closure deletes captures, verify closure and report unclosed IDs
if cleanup fails. Only an explicit user request permits retaining a session;
unrelated user/agent sessions and shared browser processes must remain untouched.
The host backs this with cleanup when a turn ends, since a model can forget:

- Claude: the sidecar closes each query, which ends its MCP workers and every
  session they opened.
- Codex keeps a thread's MCP servers across turns. When a turn that called a
  `ketplus_browser` tool completes, the daemon calls `browser_close_all_sessions`
  on that thread through `mcpServer/tool/call`. Turns that never used the browser
  do not start the server just to close nothing.
- Cursor ACP gives clients no tool call. Each conversation's server receives
  `KETPLUS_BROWSER_TURN_END_FILE`, a file under the artifacts directory. When a turn
  ends, the daemon writes the end time to that file, and the server closes the
  sessions created before that time. A session a following turn already opened
  is kept.

So a session never carries into a later turn inside KetPlus, even at the user's
request; the retention exception applies only outside KetPlus.

This adds model instructions, not a security enforcement layer. It does not
remove other provider tools or change their permission modes. The existing
browser origin proxy remains the enforcement boundary for KetPlus browser calls.

## Verification

Fake providers exercise direct/daemon Codex thread setup and captured availability,
Claude/Cursor new and resumed turns, disabled/damaged/missing runtimes, preservation
of user text/images, and rejection of caller-supplied routing metadata. A fake SDK
checks Claude's actual system-prompt options. MCP integration checks routing
and mandatory cleanup instructions at initialize. Provider tests also verify
cleanup guidance reaches new and resumed sessions. No authenticated model call is
required; deterministic coverage proves instruction delivery, not guaranteed model
compliance. Turn-end cleanup is covered by the MCP suite (the close-all tool and
the turn-end file keeping later sessions), a fake Codex thread (called only after
turns that used the browser) and a fake Cursor agent (the per-conversation file
receives the turn's end time).
