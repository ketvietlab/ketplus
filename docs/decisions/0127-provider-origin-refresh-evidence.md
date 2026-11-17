# ADR 0127: provider origin and MCP refresh evidence

Date: 2026-11-16. Partial verification; this is not acceptance of the complete
provider migration. Runtime history/MCP evidence is explicitly separated below.

## Required authority boundary

A session credential permits existing discovery/read operations. A background
start/stop additionally needs an immutable, server-bound mutation credential for
the originating session, generation, turn, effective policy and transport epoch.
The host supplies it through MCP bootstrap, never through model arguments or
prompt text. Review read/verify turns receive no mutation grant. Unknown origins
fail closed; a missing origin never inherits the current stored policy.

Revoke the previous grant before ending a turn or changing its context. An
invocation retains its original credential even when delayed across a review or
normal turn. Unaccepted requests return `stale_origin`; accepted work retains
its effective policy snapshot. No helper substitutes a new credential or retries
an old request under new authority. A new grant requires both old-transport drain
and successful setup of the replacement context.

## Claude SDK 0.3.322

The installed public `sdk.d.ts` documents `Query.setMcpServers()` with
`added`, `removed` and `errors` results. Host-owned dynamic servers can be
disconnected and re-added; project/plugin-owned servers are not all removed by an
empty map. This API's acknowledgement alone does not prove process exit.

The opt-in `background-sdk-offline.test.mjs` uses a temporary, synthetic MCP
worker and an input generator which never yields a prompt. It verifies the
background-disable settings overlay, then adds a worker, removes it, waits for
its actual PID to cease existing, and only then adds a second worker with a
different synthetic epoch. Both workers complete MCP initialization. Both tests
pass against SDK 0.3.322 without inference. An initial assertion expecting an
exit event synchronously at the removal ACK failed; the actual-exit check is
therefore deliberate. No real credential is written to the fixture log.

The sidecar now supplies the public `spawnClaudeCodeProcess` hook, preserving
SDK command, args, cwd, environment and abort signal. `process-barrier.mjs`
records the actual child exit; a close ACK, aborted signal or `killed` flag is
insufficient. Drain has a bounded wait, then kills only the owned process and
confirms exit. A failed drain retains the fence. Recreating a context retains
`resume`; the first prompt is held until SDK initialization, MCP replacement
without errors and connected agent-server status succeed. Only then does the
sidecar acknowledge the captured epoch. The MCP map is part of `launchKey`, so
credentials cannot change inside an old context.

The actual-SDK control tests also verify that the public spawn hook observes
native process exit without yielding a prompt. A fake SDK with real local child
processes separately verifies exit before replacement, unchanged resume ID,
MCP acknowledgement before prompt, auth/apiKeyHelper composition, and failed
MCP refresh without acknowledgement or model input. Process-barrier unit tests
cover forced kill/confirmed exit, late spawn rejection and refresh timeout.
These checks do not execute native Bash/subagents against a model.

## Codex app-server 0.162.1

The checked-in 0.154.0 protocol surface includes `thread/unsubscribe`,
`thread/closed`, and `thread/resume` with per-thread config. Historical
[official API documentation](https://github.com/openai/codex/blob/21f7032dbb5ff6b1535abb44642aedca49f7a4fb/codex-rs/app-server/README.md)
describes closure after the final subscriber leaves. That schema is not proof
of closure for another installed version. A control-only probe against installed
0.162.1 initialized an owned process and created an ephemeral empty thread;
`thread/unsubscribe` returned `unsubscribed` but no `thread/closed` notification
arrived within five seconds. Its ACK cannot be a credential barrier.

The leader therefore selected an app-server per conversation. The router stops
and confirms exit of only that owned process before initializing a replacement.
It resumes the exact prior thread ID, preserves supplied model/effort/cwd/sandbox/
approval configuration, and checks the thread's `ketplus_agents` tools inventory
before acknowledging the new epoch. It does not fall back to `thread/start`.
Setup, history-load and MCP-discovery timeouts return
`mutation_refresh_unavailable`; late callbacks and uniquely routed approvals
cannot enter a replacement context. Parent catalog-process failure does not
fence separately owned conversations or discard their pending approvals.

The fake app-server fixture verifies two conversations with synthetic persisted
history, old-process exit before replacement resume, unchanged thread ID,
independent busy workers, stale approval rejection despite reused wire IDs,
scoped crash, and missing history/load/MCP timeout without epoch acknowledgement.
This is adapter evidence, not real-provider history acceptance.

Reproduce the real, control-only probe with an installed CLI:

```sh
python3 tests/providers/codex/refresh-offline.py --executable codex
```

On 0.162.1, temporary `thread/start` and the synthetic MCP tools inventory succeed
without `turn/start`. Both same-process and cross-process resume of that empty
thread return `-32600`, `no rollout found for thread id [fixture]`. The probe
confirms owned app-server exit and separately checks the synthetic MCP workers'
exit. Empty threads are not usable evidence of non-empty history persistence.
These earlier empty controls did not establish real history/resume verification.
They did not read or fabricate existing user history/provider storage, and sent
no model request. The later T-8 injection and production-adapter control below
provide a narrower, positive persistence result without creating completed turns.
The schema generated by installed 0.162.1 also labels `thread/resume.history`
`[UNSTABLE] FOR CODEX CLOUD - DO NOT USE`; that field is not used as an artificial
history-persistence control. The product resumes only its original thread ID.

### Production adapter verification after T-8

[T-8's reviewed evidence](0127-provider-history-validation.md) established the
official `thread/inject_items` recipe. `CodexOfflineRefreshTest.cpp` repeats its
control sequence through the production `CodexAdapter`, including its dedicated
session router, actual process stop, MCP discovery and epoch acknowledgement.
It is not a second standalone JSON-RPC harness.

The explicit opt-in test uses installed 0.162.1 with an empty scratch workspace,
a disposable child-only provider profile, and no inherited credentials. The
profile selects a loopback model sentinel which rejects every request; its
observed request count is zero. A synthetic MCP discovery peer receives the
production `AgentToolLaunch` bootstrap shape, exposes common tool names plus an
epoch marker, and implements no mutation/workload. No tool or model turn is called.
The fixture does not substitute for the separate Host/IPC authorization tests.

The observed production-adapter run passes: injected synthetic context appears
in the rollout written by the provider; same-process and replacement-process
resume return the original ID; MCP inventory changes from `t3_epoch_one` to
`t3_epoch_two` with the old tool absent. The replacement launch observes zero live
prior MCP workers before starting the new provider, without adding a harness
wait to hide an adapter ordering race. Both owned provider processes and all
fixture workers exit, and the disposable profile is removed. The test reads
only newly generated owned rollout JSONL files, matching both the original
`session_meta.id` and the marker in a `response_item`, and refusing paths outside
that profile.

`thread/read(includeTurns:true)` does not show the marker before or after
replacement. A persisted raw Responses item is not a completed user/model turn;
neither rendered non-empty conversation restoration nor model recall is claimed.
The explicit installed-CLI CTest passes 1/1 (1.71 s); the default focused
matrix passes with this test body skipped before launch. This closes the
production-adapter control/MCP verification gap, while the
completed-turn history gate remains open. Cursor/Copilot mutation stays gated.

## ACP: dedicated processes and history preservation

The [ACP session setup specification](https://github.com/agentclientprotocol/agent-client-protocol/blob/main/docs/protocol/v1/session-setup.mdx)
defines optional `sessionCapabilities.close`. `loadSession` alone advertises
history load, not old-context shutdown. A load response must not be treated as a
drain acknowledgement. The leader selected a dedicated process per conversation
when close is absent: revoke, stop and confirm exit of that process, initialize
its replacement, load the original session, and acknowledge the fresh epoch
before mutation becomes available. A shared provider process cannot be restarted
to rotate one conversation's credential.

Dedicated transports also have an explicit conversation-delete boundary. The
host first requires an idle conversation with no live managed/native jobs,
revokes authority, and asks each adapter to release only that session's owned
process. Failed exit confirmation retains the local conversation for retry;
shared catalog processes and other conversations are not stopped. Provider-owned
history deletion remains the existing separate cleanup operation. Fixtures cover
busy-neighbor survival and the host's refusal to delete before release succeeds.

`AgentMutationOrigin` separately tests the host authority primitive: pending
credentials cannot mutate before an epoch acknowledgement, review receives no
secret, owner/generation mismatch and unknown origins are refused, revocation
survives delayed/duplicate invocation, and accepted snapshots retain policy
through later review/normal turns and reconnect. These are authority-unit tests. `ManagedTaskOriginHostTest` separately checks
Host admission using trusted fixture epoch activation: provider-labelled
Claude/Codex/Cursor/Copilot review read/verify origins cannot start process or
child work or stop jobs with permissive stored policy, including idle/drain,
spoofed credentials, delayed cross-turn requests and a queued catalog response.
A normal authorized child retains its accepted `ask` policy after the parent
moves to review/full. The fixture's direct activation tests the host fence;
provider transport setup is covered separately by the adapter/sidecar fixtures.
Socket IPC tests retain read-token discovery/output while denying mutation and
agent cleanup acknowledgement.

`AcpSessionIsolation.cpp` implements the adapter part when the host supplies a
transport epoch. It creates independent concrete Cursor/Copilot workers,
preserves the supplied resume ID, forwards scoped events/permissions, and rejects
load failure or missing history-load capability instead of silently creating a
new session. Replies use unique routes so an old permission ID cannot answer a
request in a replacement process. Startup/load have bounded timeouts. Worker
failure is scoped to that conversation instead of emitting provider-wide stop.
The Host activates mutation only for the exact pending epoch acknowledged by
the adapter; stale setup callbacks cannot activate a newer turn.

The offline Qt fixture verifies two concurrent conversations, actual old-process
exit before replacement load, unchanged history ID and synthetic persisted
history, distinct epochs/PIDs, load rejection/timeout without fallback, and
failure of one worker while the other remains active, and rejection of a late
reply for a replaced worker even when the wire request ID is reused. This proves the adapter
against the fixture. It does **not** prove any installed provider's history or
MCP implementation.

### Cursor control results and validation gap

The production Cursor adapter does not advertise managed mutation support. The
Host still provisions a fresh read/discovery epoch, but issues no mutation
credential even after an ACP load ACK. The MCP helper omits start/stop from
`tools/list` without a credential; direct start/stop attempts return
`mutation_refresh_unavailable` from the daemon. Foreground behavior and authorized
registry reads remain available. This is a temporary safety gate, not completion
of Cursor support or evidence that its non-empty history cannot be restored.

Installed CLI: `2026.11.11-043ef28`. `initialize` returns protocol 1,
`loadSession:true`, MCP HTTP/SSE capabilities, and `sessionCapabilities:{list:{}}`;
it does not advertise session close. A temporary empty session is created
successfully with a canonical cwd and `mcpServers:[]`. Loading that ID returns
`-32602`, `Invalid params`, both in the same process and after confirmed exit
and replacement initialization. No `session/prompt`, authentication request,
tool invocation, user history read, or provider DB edit occurs.

Reproduce only with an already installed CLI:

```sh
python3 tests/providers/cursor/refresh-offline.py --executable agent
```

The [official Cursor ACP documentation](https://cursor.com/docs/cli/acp) describes
session loading and configuration-file MCP setup, but does not establish an
old-transport drain guarantee or version-specific empty-session persistence.
Because the same-process control also fails, these results do not isolate a
cross-process regression. They do not establish successful restoration of
non-empty history or injected MCP bootstrap either. The smallest outstanding
evidence is a provider-supported no-inference fixture/history setup or an
official version-specific capability test which verifies load and the injected
MCP context. Do not reuse real user transcripts or fabricate provider storage to
obtain that evidence.

### Copilot validation gap

No Copilot CLI is installed in this environment. The concrete adapter builds
with the dedicated-process changes, and the generic ACP fixture covers the
control sequence. Real advertised capabilities, cross-process history load and
MCP replacement remain unverified. No quota-consuming substitute was run.
Copilot has the same production mutation gate as Cursor; a fake ACP test cannot
enable it. Enabling either adapter requires reviewed provider/version-specific
history and injected-MCP evidence, rather than an environment override or a load
ACK alone. Existing conversation history is kept when load fails; refresh returns
a capability error without fallback `session/new` or an automatic retry loop.

The independent regression suite also covers an unconfirmed Claude close after
timeout/kill request, a late Host epoch callback after review-to-normal turnover,
and read-only Codex MCP discovery without hidden mutation tools. A normal Codex
bootstrap with a mutation credential still requires start/stop inventory before
its epoch ACK. These checks preserve the distinction between an unavailable
mutation capability and usable foreground/read transports.

## Verification checkpoint

The source checkpoint `905142e2` includes the latest integration baseline and
was rebuilt with the headless daemon preset on macOS arm64. No app bundle was
deployed and no provider model turn was submitted.

| Check | Result | Evidence boundary |
| --- | --- | --- |
| Post-integration Host, IPC, origin, Codex isolation, ACP isolation, messaging CTest matrix | 6/6 pass, 60.33 s | Real local fixture processes and temporary journals |
| Narrow MCP/origin CTest target | 2/2 pass | Strict schemas, immutable invocation credential, authority state machine |
| Sidecar and background contract Node suite | 95 pass, 3 opt-in SDK skips | Fake SDK/native hook and lifecycle fixtures; no model |
| Explicit installed SDK 0.3.322 plus process-barrier control tests | 6/6 pass | Native query process/MCP control only; no prompt yielded |
| Cursor/Copilot adapters and turn-acceptance matrix before integration refresh | 6/6 pass | Concrete adapters with scripted providers, not installed-provider history |
| Claude adapter/worktree, daemon lifecycle, Codex isolation/items, messaging regression matrix before integration refresh | 6/6 pass | Includes scoped browser config and permission behavior |

A failed first version of the new ACP stale-response fixture expected an
untranslated response instead of the existing ACP permission outcome. A failed
first delete-release fixture reused a message ID and therefore received its
cached failure on retry. Corrected fixtures pass; neither failure is counted as
a successful run. The Claude close-throw regression also verifies that an SDK
exception cannot bypass actual subprocess drain.

The acceptance gate still requires completed-turn history evidence for Codex,
real history/injected-MCP evidence for Cursor, and an available Copilot runtime
with history and injected-MCP verification. Passing synthetic adapter history is
insufficient to close that gate. The task remains blocked on this evidence rather than
silently resetting provider history or reducing the supported-provider goal.

The subsequent capability-gate regression run passes 7/7 CTest targets (59.85 s):
Host/IPC, Host origins, Codex isolation, ACP isolation, MCP and messaging. The
narrow MCP/origin target passes 2/2; the sidecar/background-contract suite passes
96 tests with 3 explicit SDK opt-in skips. The newly added unconfirmed-close
fixture uses a synthetic process record and starts no workload. No installed
provider probe was repeated for this change, and no model was called.

## Provider gaps after the real-adapter control (before D-3)

| Provider | Proven scope | Remaining boundary | Smallest next access/probe |
| --- | --- | --- | --- |
| Claude SDK 0.3.322 | Installed SDK background-control rejection, settings precedence, real query/MCP process exit; sidecar fixtures cover native-tool hooks, resume/auth composition and managed delegation | Model-driven native Bash/subagent execution and a completed-turn resume were not run | Existing offline recipe needs no new auth/quota. If end-to-end model behavior is required, leader must bound and obtain permission for a synthetic turn in an isolated authenticated profile |
| Codex 0.162.1 | Production adapter plus installed CLI: public injection, provider-written raw context, same-ID same/replacement-process resume, real MCP epoch replacement, actual old-worker exit and zero sentinel requests | Injected context does not establish completed-turn presentation history or recall | A provider-supported offline completed-turn fixture/import, otherwise one authorized synthetic text-only turn in an isolated authenticated profile; a recall turn is separate if required |
| Cursor 2026.11.11-043ef28 | Advertised ACP load capability; scripted adapter isolation/fences; reviewed T-8 isolated setup reached an offline sentinel and could not initialize its injected MCP worker | Real injected MCP and non-empty history restoration remain unverified; empty controls do not establish lack of support | Isolated test authentication/control-plane access to initialize ACP/MCP without prompting first; only then, if no offline history recipe exists, explicitly authorize one synthetic text-only turn |
| Copilot | Concrete adapter builds and fake ACP lifecycle/authority fixtures | No installed CLI version, real initialization/MCP or history evidence | Supply a pinned installed CLI and isolated test profile; record control capabilities/MCP first, then request separate quota permission only if materializing history requires a model turn |

Authentication permission does not authorize model quota. No provider installation,
user transcript, manual provider-DB edit, automatic history reset or repeated
empty-session probe is part of this follow-up. The reviewed T-8 document records
the independent research and proposed user-decision boundary.

## D-3 bounded validation: environment and authentication checkpoint

On 2026-11-17, the user approved the proposed bounded follow-up. Its budget is
at most one synthetic text-only model turn per Codex, Cursor and Copilot, three
in total, with one developer holding the team budget. It excludes recall turns
and automatic retries. A request whose delivery is uncertain consumes that
provider's allowance. Authentication uses the official provider integration;
approval does not authorize copying a personal profile or transcript. Synthetic
session data and cwd are separate from the authentication integration.
A pinned Copilot CLI may be prepared in a test-only directory. Control/MCP
verification must succeed before any model turn is sent. This decision changes
the earlier access boundary, not the provider acceptance criteria.

The initial checkpoint consumed **Codex 0/1, Cursor 0/1, Copilot 0/1** live model
turns. No login flow was started and no credential was copied. Clean
child environments retain executable/locale essentials only, use separate homes
and provider configuration directories, and do not inherit authentication or
user configuration. Codex additionally forces file credential storage in its
test profile: `login status` exits 1 and reports not logged in. Cursor `status`
exits 0 but reports not authenticated. Those isolated profiles lacked auth;
this did not assess the existing KetPlus authentication integration. See the
corrected follow-up below. Official references describe
[Codex file storage and login](https://learn.chatgpt.com/docs/auth) and
[Cursor login](https://cursor.com/docs/cli/reference/authentication).

Copilot CLI **1.0.112** was installed into a separate local npm prefix, with no
global installation replacement. Registry integrity was
`sha512-OieokPh6qDlniqP4LqTSSuw+y87b3g0CPgFOVB3w6U523aEAfruniP/DWca7IaFq5FMtgwTi6JdfOBCBEtNsew==`.
Its installed help documents `COPILOT_OFFLINE` with a local model-provider
endpoint; [GitHub's ACP reference](https://docs.github.com/en/copilot/reference/copilot-cli-reference/acp-server)
also documents BYOK sessions without GitHub login. The control uses offline mode,
an always-rejecting loopback endpoint, isolated home/profile/cache directories,
disabled update/telemetry/built-in MCP, and no authentication variables.

The real CLI returns `loadSession:true` and `sessionCapabilities.close` from
`initialize`. `session/new` accepts one injected stdio MCP server and creates a
session, but its worker does not spawn or discover tools during the bounded
five-second observation after the control response.
The informational `/mcp list` command also returns successfully without causing
worker startup; GitHub documents informational ACP slash commands as non-model
operations. The local endpoint observes **zero requests**, the provider exits,
and no worker remains. The reproducible control
`tests/providers/copilot/refresh-control.py` therefore exits **2** for missing
MCP discovery. This bounded request-injection observation does not establish
that the provider cannot support injected MCP.

A separate launch-configuration control uses the documented
`--additional-mcp-config` route instead of request injection. An initial fixture
aborted on the vendor's `server/discover` preflight. That result was a fixture
defect, not provider readiness evidence. Returning JSON-RPC method-not-found
for the unsupported preflight allows standard MCP initialization to follow.
The corrected launch control **passes (exit 0)**: its informational output names
the fixture, its worker completes `initialize` and `tools/list`, zero endpoint
requests occur, and the provider and worker exit without leftover processes.
The corrected request-injection control still observes no worker and exits 2.

The Copilot adapter now supplies the managed agent server in an immutable
process-launch configuration file owned by its dedicated conversation worker.
The file is created with `NewOnly` and explicit owner read/write permissions
(`0600` on POSIX); argv contains only its
`@file` reference, and deletion follows worker release. The common server is
omitted from session-level injection to avoid registering it twice. Other MCP
servers keep their existing routing. Platform launch construction preserves
Windows wrapper quoting, rejecting unsafe command-wrapper/configuration paths.
Fixtures verify distinct epoch files, old-file deletion, credential exclusion
from argv/logs and unchanged mutation gating. Authentication/model/cwd policy
is not replaced by a test configuration. Creation-time permissions use
[Qt's ACL-aware file-open API](https://doc.qt.io/qt-6/qfile.html#open-1), rather
than relying on a later `setPermissions` call for Windows. Windows command
shapes are exercised on macOS; native Windows launch/ACL validation was not run.

The control is a standalone protocol measurement, not a production-adapter
history/epoch proof. Copilot's previously missing executable gap is closed for
this pinned test environment; the standalone process-launch MCP control also
passes. Completed history and real production-adapter refresh remain unverified.
Cursor/Copilot mutation gates stay
in place. The initial suggested next step was login into those profiles; that
test-environment choice is superseded by the official-auth follow-up below.
The approved budget has not been increased.

The final build and focused Host-origin/ACP-isolation/Copilot-adapter/tool/catalog/
CLI CTest matrix pass **6/6 (59.65 s)**; Python compilation and diff checks pass.
The initial matrix exposed a 500 ms interpreter-startup deadline in the isolation
fixture, with different startup cases timing out across runs. Initialization now
has its own 3 s fixture budget; the intentional `session/load` timeout remains
500 ms and production deadlines are unchanged. The corrected real launch-MCP
control passes without inference. These checks do not complete the real history
or mutation-support acceptance criteria.

## Official authentication route and bounded probe follow-up

The clean-HOME login checkpoint above was a test-environment mismatch. The user
confirmed that all three providers already work through KetPlus. The follow-up
therefore keeps the inherited provider environment and official credential-store
integration used by the production adapters, and separates only the new
synthetic conversation and cwd. It does not copy credentials, inspect previous
transcripts, modify default auth/configuration, or restart a shared process.
Codex `login status` and Cursor `status` confirm authentication through that
route. Copilot's ACP initialize/new control succeeds; that alone is not a model
entitlement check.

Available cheap named models are checked before delivery: Codex advertises
`gpt-6-luna` with low effort; Cursor advertises `claude-haiku-5-5-low` without
thinking/fast mode. The [Codex model guide](https://learn.chatgpt.com/docs/models)
describes Luna as an efficient model; the
[Cursor pricing table](https://cursor.com/docs/models-and-pricing) lists Haiku
5.5 at $0.10 input / $0.50 output per million tokens at this checkpoint. These
are explicit selections, with no default or more expensive fallback.

Copilot CLI 1.0.112 does not advertise a model selector in the authenticated-route
ACP session response. The documented informational `/model` command reports that
its picker is interactive-only. On the newly created synthetic session,
`/model --session claude-haiku-5.5` returns **only Auto mode is available on your
plan**. No model prompt is sent. This is an explicit-model entitlement boundary,
not evidence that the provider is broken or that another login is required.
The [official CLI reference](https://docs.github.com/en/copilot/reference/copilot-cli-reference/cli-command-reference)
documents the session-scoped model command. The existing strict adapter model
selection also refuses an unadvertised named model before prompting. Auto is
not used to bypass the user's cheap explicit-model constraint; Copilot mutation
remains gated pending a supported, verifiable selection and real history test.

The opt-in production-adapter probes keep their external decision ledger across
runs and reserve before actual model delivery. Cursor's observer also checks
the real model ACK and MCP discovery before passing any prompt. It intercepts
the replacement transport's prompt after the provider's real load/model reply,
so history replay can be inspected without a second/recall turn. That observer
does not invent history, write provider storage, or replace a load ACK. The
bounded tests skip by default and are excluded from quota-free regression runs.

### Bounded result ledger and remaining provider gaps

The real Codex production-adapter test **passes 1/1 (16.34 s)** at source
checkpoint `58644f64`, with installed CLI **0.162.1**, explicit
**gpt-6-luna / low**, and one synthetic text-only turn. The observer's earlier
missing `agents_whoami` fixture entry caused the pre-delivery readiness fence to
timeout; the corrected fixture passes before reserving the allowance. Completed
`agentMessage` history containing the synthetic marker is visible through
`thread/read` after same-process resume and after dedicated-process same-ID
resume. Epoch-two tools replace epoch one; the previous MCP processes are dead
at replacement launch. All recorded owned provider/MCP PIDs exit. No tool call,
recall turn, fallback model, or second model turn occurs. This verifies rendered
completed history, not model recall of that history.

The team ledger is now **Codex 1/1, Cursor 0/1, Copilot 0/1**, total **1/3**.
Codex must not be rerun with that ledger. No allowance is refunded for a failed
assertion or uncertain delivery.

| Provider/version | Verified through the supported authentication route | Remaining gap / smallest next step |
| --- | --- | --- |
| Codex 0.162.1 | Actual adapter, available named cheap model, one completed turn, same-ID history before/after process replacement, MCP epoch rotation and process exit | No further model validation is required for these assertions; model recall was intentionally excluded |
| Cursor 2026.11.11-043ef28 | Status authenticated; actual adapter initialize/new succeeds and ACKs named Haiku selection; request-injected MCP receives initialize/initialized | MCP does not issue `tools/list` before the observer's prompt fence, including a standard list-changed notification control. A direct no-model new-session control succeeds in 7.83 s, separating the initial startup timeout from an auth failure. The official project-scoped `agent mcp list-tools` control refuses the unapproved synthetic server before launching it. Establish a supported approval/discovery path without modifying default user settings, then use the still-unused single allowance for real completed-history/same-ID refresh. Mutation remains gated. |
| Copilot CLI 1.0.112 | Inherited-route ACP initialize/new; existing standalone launch-MCP discovery; session-scoped model control returns the explicit entitlement diagnostic | Current plan permits Auto only and advertises no named-model selector. A supported cheap named-model entitlement or an explicit revised model constraint is needed before the unused allowance can be sent. Then verify actual-adapter completed-history/MCP replacement; mutation remains gated. |

Cursor control prompts are intercepted, not delivered to the provider. One
initial control timeout briefly outlived the observer's three-second cleanup
check; a subsequent PID check confirms every recorded owned process exited.
The corrected protocol control exits its owned provider and worker. These are
test-observer observations, not proof that a real background executor leaked.
The additional CLI project fixture contains only synthetic configuration and
does not approve a server or change global settings. The official
[Cursor MCP command reference](https://cursor.com/docs/cli/mcp) documents the
`list-tools` route. No repeated empty-session load probes, provider-storage
edits, user-history reads, or additional provider installations were used.

### Reviewable Cursor discovery approval proposal

This proposal has **not** been approved or executed. It authorizes no model
prompt. The existing team ledger stays Codex 1/1, Cursor 0/1, Copilot 0/1.

`tests/providers/cursor/discovery-proposal.py --prepare --root <new-private-root>`
creates a review manifest, a project-scoped `.cursor/mcp.json`, an ACP
`session-new.json`, and exact commands in `REVIEW.txt`. It never calls an approval
API. The fixed fixture source checksum is recorded, and serving refuses a source
change after review. Its unique server identity starts with
`ketplus_t3_cursor_discovery_`; the prepared manifest supplies the complete name,
canonical cwd, executable, args and empty environment. Do not approve the real
`ketplus_agents` server, a wildcard or another server in its place.

The sole exposed tool is **t3_probe_status**. It returns the constant
`synthetic-ready`, accepts no input, and declares read-only/non-destructive/
idempotent/closed-world annotations. It exposes no shell, file, URL, real daemon,
credential, background mutation or cleanup operation. Project tool policy denies
all MCP/native tool calls; discovery itself needs no tool-execution permission.
Server audit records contain only method names and owned PIDs. An independent
stdio self-test verifies the single-tool inventory, static result, rejection of
`background_task_start`, private manifest permissions and matching CLI/ACP launch
tuples; it does not launch a provider or approve a connection.

The supported [configuration override](https://cursor.com/docs/cli/reference/configuration)
`CURSOR_CONFIG_DIR` is kept separate from inherited HOME/auth. Installed CLI
2026.11.11-043ef28 also resolves project MCP approval state through its data root
(`CURSOR_DATA_DIR`), rather than the config root. This data-root behavior and the
approval-key binding to canonical workspace/server definition were checked in
the distributed CLI implementation; they are version-specific observations,
not an undocumented approval-file API for us to write. With **both** roots set
to private proposal directories, official `agent status` still reports
authenticated. The no-model `mcp list-tools` control returns the expected
unapproved-server error, launches no fixture, and creates no approval entry.

After the user reviews the manifest and approves the exact operation, the
official [MCP commands](https://cursor.com/docs/cli/mcp) are:

```sh
cd "$fixture_root/work"
# Confirm the reviewed installed version before executing an approval.
agent --version
CURSOR_CONFIG_DIR="$fixture_root/config" CURSOR_DATA_DIR="$fixture_root/data" \
  agent mcp enable "$exact_server_identity"
CURSOR_CONFIG_DIR="$fixture_root/config" CURSOR_DATA_DIR="$fixture_root/data" \
  agent mcp list-tools "$exact_server_identity"
# Revoke loading through the official CLI:
CURSOR_CONFIG_DIR="$fixture_root/config" CURSOR_DATA_DIR="$fixture_root/data" \
  agent mcp disable "$exact_server_identity"
```

`enable` persists a workspace/server-specific approval in the isolated data
root's project state, not for one session. The expected location is
`<data>/projects/<workspace-key>/mcp-approvals.json`; the actual write boundary
remains to be checked after user approval. `disable` prevents loading but need
not erase the old approval entry. After owned processes exit, deleting only the
entire proposal root removes its definition and isolated approval/disabled state.
No default user setting, personal approval entry or authentication store is
edited. Do not use `--approve-mcps`, `--trust`, global tool allowlists or manual
approval-hash insertion as a substitute.

CLI project discovery and ACP request injection are **separate controls**. The
prepared ACP payload uses the identical canonical cwd, name, command, args and
empty env as the project fixture; it does not assume that matching declarations
transfer approval or force eager discovery. After approved CLI discovery, test
only initialize/new with that ACP payload, and then the actual adapter's
intercepted-prompt control. If the provider still defers `tools/list`, stop and
report the unresolved boundary. A passing CLI control alone cannot enable
Cursor mutation or release a model prompt. The existing pre-model fence stays
in place throughout this proposal.
