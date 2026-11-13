# 0120: GitHub Copilot through a shared ACP adapter

Status: proposed
Date: 2026-11-13

Extends [ADR 0011](0011-browser-tools-for-all-providers.md) and
[ADR 0013](0013-browser-agent-routing.md) to Copilot. This records the target
contract; it does not claim the implementation or authenticated smoke test has
landed.

## Context

Cursor's adapter currently owns both provider-specific behavior and reusable
ACP transport, session, transcript and approval handling. Adding another copy
would duplicate the framing and lifecycle fixes already protecting Cursor.
GitHub documents an ACP server in Copilot CLI [G1], so Copilot can use that same
boundary while keeping its executable discovery, authentication and models
independent of Cursor.

The adapter belongs to the Pro daemon in `apps/daemon/src/providers/`. No
provider code moves into `core/`, and Copilot CLI remains user-installed.

## Decision

### Share ACP behavior, retain provider policy

Extract an ACP connection/adapter layer used by both `CursorAdapter` and
`CopilotAdapter`. It owns newline-delimited JSON-RPC over stdio, bounded
stdout/stderr, typed response correlation, initialization, negotiated
capabilities, session mapping, prompt/update normalization, cancellation and
process-generation cleanup. Session setup and permission translation also use
this layer. Provider hooks supply executable/arguments, model discovery and
selection, error classification, and any provider-specific tool metadata.
Cursor labels, paths, `agent models` parsing and login/plan heuristics stay in
the Cursor implementation. Existing Cursor behavior and regression assertions
must survive the extraction.

Register Copilot under the stable adapter id `copilot`, displayed as GitHub
Copilot. Start a supervised child process with:

```text
copilot --acp --stdio
```

GitHub documents this exact stdio invocation [G1]. Do not open a TCP listener or
use prompt mode (`-p`) as a transport. Negotiate the supported ACP version;
optional features require advertised support, rather than inheriting Cursor's
capabilities. In particular, call `session/load` only after `loadSession: true`;
a successful empty result retains the requested session id [A1].

### Executable discovery

The daemon accepts `--copilot <path>` and `KETPLUS_COPILOT`. Resolve in this order:

1. Explicit daemon option.
2. Environment override.
3. `copilot` on `PATH`, including native executable/script extensions on Windows.
4. The platform candidates below, in their listed order.

| Platform | Candidates after `PATH` |
| --- | --- |
| macOS | `~/.local/bin/copilot`, `/opt/homebrew/bin/copilot`, `/usr/local/bin/copilot` |
| Linux | `~/.local/bin/copilot`, `/home/linuxbrew/.linuxbrew/bin/copilot`, `/usr/local/bin/copilot`, `/usr/bin/copilot` |
| Windows | `%APPDATA%\npm\copilot.cmd`, `%APPDATA%\npm\copilot.ps1`, `%LOCALAPPDATA%\Microsoft\WinGet\Links\copilot.exe` |

This ordering and these fallbacks are KetPlus policy, not a promise about every
installer. GitHub documents npm, Homebrew, WinGet and script installation [G3];
custom npm prefixes, package managers and Windows link locations may differ.
Use the override for those installs. Validate file type and runnability; an
invalid explicit override produces an actionable path error rather than silently
launching a different CLI. Never search a conversation's worktree or download a
CLI automatically. Use the existing Windows argument-safe `.cmd`/`.ps1` launcher
where required; the logical arguments remain `--acp --stdio`.

If discovery fails, show: “GitHub Copilot CLI was not found. Install Copilot CLI
using the GitHub installation instructions, then retry. For a custom install,
set the daemon's --copilot path.” Link to [the installation guide][G3].

### Authentication and failures

Authentication remains owned by Copilot CLI. GitHub documents `copilot login`
and token precedence `COPILOT_GITHUB_TOKEN`, `GH_TOKEN`, `GITHUB_TOKEN` [G2].
KetPlus neither imports tokens into its journal nor starts an OAuth flow or ACP
`authenticate` request. An installed binary is not proof of a signed-in account.
GitHub also documents BYOK sessions without GitHub login [G1]; absence of a
GitHub token alone must therefore never reject a session.

Classify an explicit authentication-required ACP failure or a narrowly matched
CLI diagnostic as signed out. Historical upstream reports show `-32000` with
the message “Authentication required” [G4]; the number alone is insufficient,
since it is a generic server error. Keep permission denials, subscription/policy
restrictions, network failures and usage limits distinct. Drain stderr and
enforce startup/request deadlines so an exited or waiting process cannot leave
the turn pending forever. Preserve a bounded, sanitized provider explanation
without exposing credentials or replacing it with raw JSON.

The user-facing authentication guidance is: “GitHub Copilot CLI needs sign-in.
Run `copilot login` in a terminal on the machine running the KetPlus daemon,
then retry. For headless use, configure a supported Copilot token in that
daemon's environment.” A retry starts a fresh process after an authentication
failure. This also avoids retaining stale in-process credentials reported
upstream [G5]. These error signatures are compatibility evidence, not a claim
that the historical bugs remain in the current CLI.

### Model catalog and selection

Use the session's advertised ACP catalog, not Cursor's `agent models` command
or a hard-coded list from a documentation page. Prefer a model select option
in `configOptions`, retain its advertised option id/values/current selection,
and update through `session/set_config_option`. ACP defines these selectors and
configuration updates [A2]. Support the older `models.availableModels` /
`currentModelId` shape and `session/set_model` only when that CLI actually
offers it; this compatibility path is an assumption pending a Copilot fixture.
Do not invent reasoning levels or reuse Cursor's effort suffix rules.

Opening the picker may perform a bounded initialize/session-setup catalog
probe, but must never send `session/prompt`. Coalesce concurrent probes, drain
both channels, enforce output/time limits, and clean up the temporary process.
If no machine-readable catalog is available, expose only the provider default
and a catalog-unavailable explanation. GitHub documents `--model` and the
interactive `/model` command [G2]; neither proves the availability of a
`copilot models` subprocess API. Exact Copilot ACP catalog shapes and the
permission needed to discover them remain unverified (see Verification).

### Permission requests

Translate `session/request_permission` through KetPlus's existing authoritative
approval broker. Bind each request to its adapter, process generation, ACP
session, active turn and request id. Preserve the offered `optionId` values;
approval selects the offered `allow_once` option, refusal selects
`reject_once`, and cancellation or expiry returns a cancelled outcome. A missing
applicable option also cancels the request. Never fabricate a successful choice
or silently promote a one-time approval to an always rule. ACP defines the selected and
cancelled outcomes and the permission-option kinds [A3]. Late answers cannot
authorize a replacement turn or process.

Copilot mode ids and tool identity fields must be provider policy. Do not send
Cursor's mode ids without negotiation or claim that approval brokering alone
enforces a filesystem sandbox. Until a permission mode's semantics are verified,
report it unsupported rather than quietly weakening the selected policy. Do
not add blanket `--allow-all*` flags to make browser tools work.

### KetPlus browser MCP and routing

Reuse `BrowserToolLaunch` and its host-owned settings. Pass the resolved stdio
server in ACP `mcpServers` with the conversation cwd on `session/new` and on
supported `session/load` [A1, G1]. Capture injection state only after successful
session setup; invalid, disabled or unresolved settings inject nothing. Refresh
changed settings at the next turn/session setup, preserving active-turn policy.
Do not write user/project MCP files or trust an MCP map from a turn payload.

Extend ADR 0011's browser permission exemption only when the session received
the host server and Copilot supplies verified structured server/tool identity.
Cursor's `rawInput.providerIdentifier` is not assumed to be Copilot's schema.
Display titles or a familiar tool-name prefix alone are insufficient. Missing
or ambiguous identity uses the normal approval broker; it does not authorize
all MCP tools. The exact Copilot identity representation is unverified and
needs a fixture before the exemption is enabled.

Apply ADR 0013's ACP routing: prepend a separate host-owned context block based
on captured injection state, preserve user text/images, and keep the block out
of user-authored journal text. Send explicit unavailable guidance when needed.
Reuse the per-conversation `KETPLUS_BROWSER_TURN_END_FILE` signal and write it
when a turn ends, fails or is cancelled, so the worker closes leftover browser
sessions. Keep the origin proxy, absolute worker paths and isolated profiles;
Copilot owns its child MCP workers. Routing instructions guide model behavior;
the existing browser runtime enforces origins.

## Verification and compatibility limits

On 2026-11-13, no `copilot` was available on the shell's `PATH`. A temporary npm
execution of the official `@github/copilot` package reported GitHub Copilot CLI
**1.0.110** using `--version`. Its `--help` listed `--acp`, `--model`, `login`,
and additional MCP configuration. `--stdio` was not listed in that top-level
help; its use is based on GitHub's explicit ACP documentation [G1]. Only version
and help were executed: no prompt, login, authenticated session or model-quota
test was run. GitHub describes ACP as public preview [G1].

Before declaring implementation complete, deterministic fake-provider tests must
cover discovery precedence and Windows wrappers; exact launch arguments;
initialize/new/load/prompt/update/cancel; auth failure and fresh retry;
catalog success/absence/malformed/timeout cases; approval selection, rejection,
expiry and stale generation; injected versus spoofed browser identity; new and
resumed routing; disabled settings; and turn-end cleanup. Cursor's existing
catalog/framing/lifecycle tests must pass without weakened assertions.

The assumptions above require sanitized fixtures from the supported CLI or an
explicit opt-in live smoke test. Until then, negotiated capabilities and
conservative fallbacks prevent catalog or metadata drift from granting broader
permissions. Fake-provider tests prove transport and policy handling, not
Copilot's live behavior or model compliance. Platform candidate paths, resumed
MCP replacement and worker shutdown also need platform/runtime validation;
none was demonstrated by the version/help check.

## Consequences

ACP fixes benefit Cursor and Copilot through one implementation while the daemon
and Desktop retain the existing provider-neutral event and approval contracts.
Users install and authenticate Copilot on the daemon host. No CLI, dependency or
credential store is bundled by this decision.

The extraction has regression risk because Cursor already handles malformed
frames, bounded output and late responses. Splitting by behavior and retaining
its focused tests is required. Copilot's preview protocol and provider-owned
catalog/tool metadata may change independently; unknown features degrade
explicitly instead of borrowing Cursor-specific assumptions.

## Alternatives considered

- Copy CursorAdapter: duplicates protocol/security fixes and mixes provider
  policy into two transports.
- Use an SDK sidecar or `-p` subprocess: adds another transport and dependency
  when the CLI already exposes the required ACP boundary.
- Persist MCP configuration or grant all tools: makes local browser policy
  inconsistent and expands authority beyond the injected server.

## Sources

GitHub and ACP references were consulted on 2026-11-13. They are mutable;
version/help evidence above is narrower than a protocol conformance check.

- [G1: GitHub Copilot CLI ACP server][G1]
- [G2: GitHub Copilot CLI command reference][G2]
- [G3: Installing GitHub Copilot CLI][G3]
- [G4: Copilot CLI issue #4016, historical ACP authentication failure][G4]
- [G5: Copilot CLI issue #3902, historical stale authentication state][G5]
- [A1: ACP v1 session setup][A1]
- [A2: ACP v1 session configuration options][A2]
- [A3: ACP v1 tool calls and permission requests][A3]

[G1]: https://docs.github.com/en/copilot/reference/copilot-cli-reference/acp-server
[G2]: https://docs.github.com/en/copilot/reference/copilot-cli-reference/cli-command-reference
[G3]: https://docs.github.com/en/copilot/how-tos/copilot-cli/set-up-copilot-cli/install-copilot-cli
[G4]: https://github.com/github/copilot-cli/issues/4016
[G5]: https://github.com/github/copilot-cli/issues/3902
[A1]: https://agentclientprotocol.com/protocol/v1/session-setup
[A2]: https://agentclientprotocol.com/protocol/v1/session-config-options
[A3]: https://agentclientprotocol.com/protocol/v1/tool-calls
