# ADR 0127: provider history and MCP validation without inference

Date: 2026-11-16. Evidence only; this does not close the provider acceptance gate
or change the authority contract in [ADR 0127](0127-daemon-background-tasks.md).

## Findings

Codex 0.162.1 has a documented, real no-inference history injection method. An
owned app-server accepted synthetic items, wrote them to its own rollout, and
resumed the original thread in a replacement process with a different MCP
inventory. However, its presentation-history APIs did not return the injected
marker. This establishes more than the previous empty-thread control, but does
not establish restoration of a non-empty user/model conversation or model recall.

No supported offline synthetic-history import recipe was established for Cursor
2026.11.11-043ef28. Its isolated, offline MCP control could not complete session
setup. Copilot was not found in the executable search locations checked. None
of these results proves that a provider lacks history or MCP support.

| Evidence | Observed result | Limit |
| --- | --- | --- |
| Codex `thread/inject_items` | ACK and marker in provider-written rollout | Injected prompt context is not a completed model turn |
| Codex same/replacement-process `thread/resume` | Success, original ID preserved | History responses did not contain the marker |
| Codex per-thread MCP replacement | Connected worker/tool for epoch `one`, then `two`; old tool absent | Synthetic MCP endpoint, not production authorization |
| Cursor isolated injected stdio MCP | `session/new` failed with `-32603`; worker not started | Offline API sentinel rejected setup requests; not an unsupported-capability verdict |
| Cursor configured MCP discovery | `mcp list-tools` exited 1; no fixture tool/worker | Configuration-file discovery does not establish ACP injection or replacement |
| Copilot discovery | No executable found on PATH or checked default candidates | No installed-version capability or history test |

## Inputs and scope

The provider-origin evidence and probes from the provider migration change were
read first: `docs/decisions/0127-provider-origin-refresh-evidence.md`,
`tests/providers/codex/refresh-offline.py`, and
`tests/providers/cursor/refresh-offline.py`. These files belong to that change;
this document does not modify them. Coordination requested the existing recipe
from its developer and shared the new Codex control findings.

Those earlier controls created empty sessions. Codex returned `-32600`,
`no rollout found`, on both same-process and replacement-process resume. Cursor
returned `-32602`, `Invalid params`, on both loads. The controls did not isolate
a cross-process failure. They were not rerun here, and are not evidence against
non-empty history restoration. Scripted adapters with synthetic persisted
history test the host/adapter contract, not the installed provider's persistence.

This investigation ran on macOS 26.2 arm64 with installed `codex-cli 0.162.1`
and Cursor `2026.11.11-043ef28`. Version/help/schema commands and owned control
processes were used. No user transcript, existing session ID, provider database,
model turn, login, or remote MCP server was used. No provider runtime or adapter
code was changed. Temporary profiles contained only newly generated fixtures.

## Codex: supported injection, with a narrower history result

The [official app-server documentation](https://learn.chatgpt.com/docs/app-server#inject-items-into-a-thread)
describes `thread/inject_items` as appending raw Responses items without starting
a user turn, persisting them to the rollout for later model requests. Its resume
documentation also describes per-thread MCP discovery through
`mcpServerStatus/list` and required-server initialization checks. These are API
contracts; the observations below test the installed binary separately.

Schema generated locally with the installed binary includes
`ThreadInjectItemsParams` with required `threadId` and `items`. In contrast,
`ThreadResumeParams.history` is explicitly marked
`[UNSTABLE] FOR CODEX CLOUD - DO NOT USE`; it was not used. Resume `path` is also
unstable and can override the requested identity, so the probe resumed by ID
only. External-agent session migration in the documentation does not provide an
arbitrary synthetic transcript import format, and was not used to manufacture
provider storage or discover existing histories.

### Control recipe

Generate the exact-version schema without starting a model:

```sh
codex --version
codex app-server generate-json-schema --experimental --out <temporary-schema-dir>
```

Use a disposable directory containing separate `profile` and empty `work`
directories. Launch each owned `codex app-server` with a child-only environment:
retain executable/library/locale essentials, point `CODEX_HOME` at `profile`,
and omit inherited provider credentials. Do not modify the shell's home or the
user's profile. The [configuration documentation](https://learn.chatgpt.com/docs/config-file/config-advanced)
describes custom model providers. Here a custom provider used a loopback HTTP
sentinel returning 503 for every request, with no authentication configured.
This is a deny endpoint, not a fake successful model response.

Initialize with `capabilities.experimentalApi: true`, then send `initialized`.
Create a persistent thread using these parameters; substitute only disposable
absolute paths, the local sentinel port, and the Python executable:

```json
{
  "cwd": "<temporary-work-dir>",
  "approvalPolicy": "never",
  "sandbox": "read-only",
  "config": {
    "model_provider": "t8-offline",
    "model_providers": {
      "t8-offline": {
        "name": "T8 offline sentinel",
        "base_url": "http://127.0.0.1:<sentinel-port>/v1",
        "wire_api": "responses"
      }
    },
    "mcp_servers": {
      "ketplus_agents": {
        "command": "<python-executable>",
        "args": ["<temporary-worker.py>"],
        "env": {"T8_EPOCH": "one", "T8_PIDS": "<temporary-pid-log>"}
      }
    }
  }
}
```

The local worker is a synthetic MCP peer. It logs its PID, responds to
`initialize` with tools capability and `serverInfo.version` equal to `T8_EPOCH`,
and responds to `tools/list` with just `t8_epoch_one` or `t8_epoch_two`, with an
empty object input schema. It exposes no workload or mutation implementation
and has a 45-second lifetime fallback. Actual PID exit, not EOF alone, is checked.
No tool is called.

After discovery of `t8_epoch_one`, append one deliberately synthetic item:

```json
{
  "method": "thread/inject_items",
  "id": 3,
  "params": {
    "threadId": "<new-thread-id>",
    "items": [{
      "type": "message",
      "role": "assistant",
      "content": [{"type": "output_text", "text": "T8 synthetic context alpha"}]
    }]
  }
}
```

The remaining sequence is:

1. Read `thread/read` with `includeTurns:true`, `thread/items/list`, and
   `thread/turns/list` for this owned ID only; check for the marker, not merely
   successful responses.
2. Resume this same ID in the first process with the original start parameters.
3. Terminate and wait for the owned app-server; wait for all its logged MCP PIDs
   to cease existing before creating a replacement.
4. Initialize the replacement with the same disposable profile, then resume the
   original ID with the same cwd/config, changing only `T8_EPOCH` to `two`.
   Do not call `thread/start` as a fallback.
5. Inspect `mcpServerStatus/list` with that `threadId`: require connected server
   version/tool `two`, and absence of the old tool. Repeat the history reads.
6. Stop and wait for the replacement and its workers. Inspect only the rollout
   written inside this disposable profile for the marker, then remove the
   temporary profile. Check the sentinel request count and remaining PIDs.

The control harness allowlisted initialization, start, injection, resume,
read/history-list and MCP-status methods. It never sent `turn/start`, review,
compaction, prompt, or a tool invocation. The sentinel recorded **zero** requests.

### Observations and remaining boundary

On the corrected harness, injection, same-process resume, and replacement resume
all succeeded. Both resume responses preserved the original ID. The first MCP
worker reported connected with tool `t8_epoch_one`; the replacement reported
`t8_epoch_two`, with the old tool absent. Old app-server and MCP worker exit was
confirmed before replacement. Final remaining owned-provider/worker count was
zero, and the disposable profile was removed.

The marker was present in the provider-written JSONL rollout after replacement;
the harness never wrote or edited that rollout. However, successful
`thread/read`, `thread/items/list`, and `thread/turns/list` responses did not
contain it before replacement; `thread/read` and `thread/items/list` still did
not contain it afterwards. Successful resume plus a persisted response item is
therefore a partial result, not a demonstrated round trip of a completed turn.

A separate control selecting experimental `historyMode:"legacy"` also resumed
the same ID and refreshed MCP, and retained the marker in the owned rollout.
It did not make the marker visible through `thread/read`; item pagination
returned `-32601`. Selecting legacy history did not close the gap. This is
evidence about that explicit mode, not a proposal to change the product default.

Initial harness attempts had an incorrectly escaped Python worker and failed MCP
handshake. They are not counted as successful MCP observations; the corrected
worker completed initialization and tool discovery. No model was used in either
attempt.

## Cursor: capability, authentication and injection are separate

The [official ACP guide](https://cursor.com/docs/cli/acp) documents `session/new`,
`session/load`, authentication, and project/user configuration-file MCP. The
[CLI MCP guide](https://cursor.com/docs/cli/mcp) documents `mcp list-tools`.
The [configuration reference](https://cursor.com/docs/cli/reference/configuration)
documents `CURSOR_CONFIG_DIR` for an isolated CLI configuration directory.
The inspected help/reference did not establish a supported synthetic transcript
import or a no-inference method for appending conversation messages.

The [ACP session-setup specification](https://agentclientprotocol.com/protocol/v1/session-setup)
defines injected `mcpServers` on new/load and history replay via `session/update`
when loading a conversation. A capability advertisement is not a successful
provider implementation test, nor does it establish old-transport drain.

The new Cursor control used a temporary work/config directory,
`CURSOR_CONFIG_DIR`, no inherited credentials, and `--endpoint` pointing only at
a loopback sentinel returning 503. It did not authenticate or send
`session/prompt`. `initialize` advertised `loadSession:true`, HTTP/SSE MCP,
and `sessionCapabilities:{list:{}}`, matching the earlier observation.

`session/new` with one injected stdio worker returned `-32603`. During the
three-second observation no worker spawned, initialized, or listed tools.
Separately, a project `.cursor/mcp.json` defining only that worker was tested
with `--trust --approve-mcps mcp list-tools t8_probe`; it exited 1 without
discovering the synthetic tool. Ten API requests were rejected by the local
sentinel across these controls; none reached a remote provider or successful
inference backend. The owned Cursor process exited and no fixture worker
remained. Temporary files were removed.

These failures under denied API access cannot distinguish authentication,
required control-plane access, lazy MCP initialization, and other setup
requirements. In particular, they do not show that stdio injection is ignored
or unsupported. No history load was repeated: another empty-session load would
not answer the outstanding non-empty-history question.

## Copilot: environment unavailable, documentation is not runtime evidence

`copilot` was absent from PATH and the standard executable candidates checked by
the macOS adapter resolver. No `gh` Copilot extension was listed either; such an
extension would not establish availability of the standalone ACP executable.
No package was installed or account authenticated during this investigation.

The [official ACP reference](https://docs.github.com/en/copilot/reference/copilot-cli-reference/acp-server)
documents `copilot --acp --stdio` and session-level MCP parameters. Its public
preview status and current documentation cannot establish the capabilities of
an uninstalled version. The [CLI reference](https://docs.github.com/en/copilot/reference/copilot-cli-reference/cli-command-reference)
documents explicit resume IDs and `--additional-mcp-config`; the latter augments
configuration rather than proving existing servers are drained. No supported
arbitrary persisted-transcript import was established from the reviewed
reference. Memory import is not a conversation-history import.

## Smallest follow-up for a user decision

Keep provider acceptance open. Do not replace the original history ID with a
new empty session, fabricate provider databases, or count a fake adapter as a
real-provider result. The leader can request either a provider-supported offline
completed-turn fixture/import recipe or authorization for the bounded steps
below; this document does not grant that authorization.

| Provider | Next bounded probe | Additional access needed |
| --- | --- | --- |
| Codex | Use this injection recipe for the control/MCP portion. For completed-turn history, create one synthetic text-only turn, then perform same/replacement-process resume and compare the exact marker/history. If model-visible recall of injected context is required, request one separate minimal echo turn. | Explicit quota permission for each model turn; isolated authenticated test profile |
| Cursor | Establish an isolated authenticated control session and prove injected MCP initialization/tool discovery first. Only if no supported offline history recipe exists, authorize one synthetic text-only turn to materialize history, then load the original ID and compare replay before any additional prompt. | Test authentication/control-plane access; explicit quota permission before the first prompt |
| Copilot | Supply an installed, pinned CLI and isolated test profile. First record initialize capabilities and injected MCP control without prompting; then apply the same non-empty-history gate. | Runtime/environment setup; authentication if required; separate explicit quota permission if materializing a turn is necessary |

For every follow-up, use an empty scratch workspace and harmless constant text,
deny native shell/subagent and external-tool work, and retain only redacted
synthetic evidence. Capture the original ID, ordered history/replay markers,
actual old-process/MCP exit before replacement, fresh MCP inventory/epoch, and
absence of fallback creation. If tool initialization requires a model turn,
stop at that boundary and request a separately bounded tool-discovery turn.
Do not treat authentication permission as permission to spend model quota.

This evidence document requires only documentation checks and public-tree
hygiene; no application build, installation or model-based smoke is claimed.
