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

### D-6 Copilot Auto follow-up

The user subsequently allowed **one Copilot Auto turn**. This is the existing
Copilot 0/1 allowance from D-3, not an additional turn; Codex stays consumed at
1/1 and Cursor's cheap named-model constraint is unchanged. Authentication keeps
the existing official inherited route. No additional login, plan change or
default configuration change is needed.

The new opt-in `CopilotBoundedRefreshTest` uses the production concrete adapter,
its private launch-MCP bootstrap and dedicated process lifecycle. The observer
sets `--model auto` only on its owned test process. Before delivery it requires
real MCP discovery and the official `/model --session auto` configuration
response. CLI 1.0.112 acknowledges that command through
`config_option_update` followed by a successful response, without prose; the
observer accepts that actual structured ACK. It reserves the existing allowance
before forwarding a valid synthetic marker and intercepts the refresh prompt.
Any explicitly reported model ID is recorded; Auto is not a guessed underlying
model name.

The CLI's empty `--available-tools` value is deliberately avoided: a
[first-party CLI issue](https://github.com/github/copilot-cli/issues/5306)
reports that it exposes all tools on 1.0.105. The probe instead uses an explicit
unmatched tool name and denies native permission requests. It also checks for
both ACP tool-call notifications and attempted fixture tool calls. This report
does not assume the older-version issue is fixed in 1.0.112. Control failures
occur before any model reservation/delivery. The synthetic-marker gate also
caught the new ACP tests using the Codex `input` shape rather than ACP's host
`prompt` field; both ACP fixtures now use the actual adapter input shape.

The actual production-adapter Auto probe then **passes 1/1 (10.08 s)** at
`fa350ec2179f`, installed CLI **1.0.112**. The observer records exactly one
forwarded synthetic model prompt, no ACP/native/MCP tool call, a successful
real `session/load` with the original ID, and replay of the completed assistant
marker before a recall prompt can reach the provider. Epoch-two MCP discovery
succeeds; both the prior native provider and MCP PIDs are already dead at
replacement launch. The observer checks and fails on a still-live prior native
process instead of waiting and hiding a closure race. All owned recorded PIDs
exit. The inspected ACP model fields disclose no underlying model ID, so the
record is **Auto; underlying model not reported**, not an inferred model name.

The Copilot implementation gate is therefore enabled. Every real invocation
still needs its exact transport epoch ACK, successful same-ID restoration when
history exists, and the daemon's immutable-origin/policy checks. Failed load
cannot fall back to new history. This installed-provider evidence covers
1.0.112 on the validation host; it does not claim native Windows execution or
model recall. Cursor remains gated and its approval proposal is unchanged.

The persistent whole-team ledger is now **Codex 1/1, Cursor 0/1, Copilot 1/1**,
total **2/3**. Neither consumed provider may be rerun with the current allowance.
The compiled opt-in targets skip before provider launch by default, **3/3**
checks pass (1.31 s), and the Python fixture/mock-ledger tests pass **2/2** with
both Cursor and Copilot branches, without provider execution. The final
checkpoint control also passes (5.90 s) before live delivery. Only the unused
Cursor allowance remains, subject to its readiness and cheap named-model
constraints.

### Cursor discovery after the user's scoped approval

The user enabled the exact static discovery proposal. This supersedes earlier
notes saying approval is pending. Read-only inspection finds one matching
project approval in the isolated data directory. The fixture source checksum
still matches the reviewed manifest; its name, command, arguments, empty
environment and canonical project cwd are unchanged. No approval hash was copied,
default configuration widened, or additional enable operation performed.
The official `agent mcp list-tools <proposal-identity>` now exits successfully and
lists `t3_probe_status`. Both recorded fixture processes exit; no model prompt is
sent. This establishes project-scoped discovery, not request-injected discovery.

A new no-model ACP control uses that same approved tuple in `session/new`.
Initialize/new both succeed. Two fixture processes initialize, but only one
issues `tools/list` within eight seconds. Both processes are confirmed gone
after the owned provider exits. The production `CursorAdapter` control then
reproduces the boundary: explicit cheap Haiku selection is acknowledged, two
fixture processes initialize, and only one lists tools. Its observer therefore
rejects the prompt before native delivery: **control fails in 22.91 s, model
requests zero**. The observer deliberately requires discovery from each newly
spawned instance so a project instance cannot attest an injected instance.
The approved proposal option is restricted to control mode; this static server
cannot substitute for the mutation bootstrap in a quota-bearing test.

This result neither proves that an eventual model turn cannot discover tools nor
proves that session injection is ready before delivery. The
[official ACP reference](https://cursor.com/docs/cli/acp) documents initialize,
session creation and prompts; the
[MCP reference](https://cursor.com/docs/cli/mcp) documents project discovery.
No supported pre-model request-injected discovery operation has been established
by these controls. Project discovery alone cannot release the delivery fence.
The outstanding boundary is therefore a supported control for the injected
context, followed by the still-unused single completed-history/refresh probe.
There is no additional login or repeated user-approval request.

Cursor remains mutation-gated. The whole-team ledger stays **Codex 1/1,
Cursor 0/1, Copilot 1/1 (2/3 total)**. No completed Cursor history, model recall,
or all-provider acceptance is claimed. All owned PIDs from the actual-adapter
control are confirmed exited. Neither consumed provider is rerun.

The quota-free fixture regressions pass **3/3 (1.126 s)**, including refusal to
credit project discovery to an injected process. With Copilot support enabled,
Host tests now check that its pending credential is unusable before the exact
epoch ACK and is revoked for review; Cursor still gets no mutation credential.
The Host regression passes **1/1 (3.16 s)** after replacing its obsolete
Copilot-is-unverified assertion. The concrete Copilot adapter and two-session ACP
isolation tests also pass (39.16 s and 2.81 s); no provider model is invoked by
these fixture tests.

### Amended Cursor text-only validation order

The design now separates transport attestation from eager tool discovery.
Cursor's production mutation gate stays disabled throughout this probe. Before
its one still-unused synthetic text prompt, a private observer must bind a
host-generated nonce/transport epoch to the exact injected worker PID and
unchanged approved launch tuple. It must observe that instance's ordered
initialize request, successful response, and initialized notification. A spawn
or session/new ACK alone is insufficient. Cheap-model ACK and confirmed prior
owned provider/MCP exit remain required. The persistent ledger is reserved before
the sole native model delivery; there is no recall, retry, or second native
prompt after same-ID restoration.

The proposed test observer uses a process-local Node preload in only the owned
Cursor test process and its children. It intercepts child-process launch/stdio
without changing the approved fixture source, server declaration or project
approval. A private Unix socket authenticates the observer OS peer PID plus its
nonce, epoch and launch digest. Private bootstrap files are mode 0600 under a
0700 directory and are deleted after closure. No nonce, secret epoch, raw
protocol body or credential is written to evidence logs. Main ACP process/thread
attribution is checked separately from project connection-pool processes or
threads; the no-model control must establish that distinction on the installed
CLI before this mechanism can release a prompt.

Native permission requests are denied. The observer blocks any MCP tools/call
before bytes reach the fixture; the fixture still exposes only its approved
static read-only inventory. If tools/list occurs lazily after prompt delivery,
only a successful inventory response from that exact injected PID/epoch counts.
The replacement transport must independently attest and restore the original
history ID. Text/history success alone cannot enable production mutation:
injected inventory, closure and history evidence still require review. Missing
attribution or handshake stops before model delivery; missing inventory after
a successful text turn leaves the gate disabled.

The first no-model attestation control rejects readiness (22.14 s): the native
CLI creates both project and injected workers in its main process/thread.
Parent PID alone therefore cannot attribute injection. The observer's captured
launch callsite distinguishes them: only the injected spawn passes the ACP
resource loop that constructs clients for request-supplied `mcpServers`.
The revised test observer pins `8512.index.js` to SHA-256
`d70d62e5f06260a477583ae6a174c537c6e8d08ea9be24c96c9a0e7ec4702725`
and the injected constructor at line 1, column 70274 of CLI
2026.11.11-043ef28. It checks the module checksum before use; unknown code cannot
inherit this classification. This is version-specific test instrumentation,
not a new provider API or production capability. Project spawns sharing the
same command and parent PID are expressly excluded. Neither exploratory
control forwards a model prompt or changes approval scope.

The revised pinned production-adapter attestation control **passes 1/1
(12.49 s)**. Exactly one worker is attributed to the injected constructor and
completes its ordered handshake, authenticated over the private OS-peer-bound
channel. Its inventory is still absent before the intercepted prompt, while
project inventory is excluded. Cheap-model ACK succeeds; rejected/forbidden
events and forwarded model prompts are zero. All owned recorded PIDs exit.
Five quota-free fixture tests pass (1.917 s), including a real Node/Python
handshake, nonce/epoch/tuple/peer spoof rejection, same-parent project exclusion,
and blocking tools/call before it reaches the Python fixture. The ledger remains
2/3 before the authorized Cursor text-only delivery; production mutation remains
disabled.

### Cursor one-shot result and exhausted allowance

The final checkpoint no-model attestation control passes (14.13 s). At
`59a52152b447dcd9967f12323138dc0b9abd46b2`, the sole authorized Cursor
text-only prompt is then forwarded after exact-instance attestation, cheap-model
ACK and ledger reservation. Requested model is `claude-haiku-5-5-low`; native
configuration ACK reports `claude-haiku-5-5`. The exact injected worker lists
its approved single read-only tool after prompt delivery. Project inventory is
not credited. No native permission request, ACP tool call, fixture tools/call,
rejected observer event or changed approval is observed. All owned recorded
processes exit.

The turn nevertheless takes the native failure path before the test can verify
completion: **live test fails in 15.44 s**. There is no second transport/load
phase and no completed-history/refresh claim. The checkpoint observer did not
retain the native prompt error code, so the cause is undetermined; the scoped
worker log's LSP warnings do not establish causation. Forwarding an ACP prompt
also does not prove successful delivery to the model backend. The request is
counted consumed, including that uncertainty, without refund or retry.

The whole-team ledger is now **Codex 1/1, Cursor 1/1, Copilot 1/1: 3/3 consumed**.
Cursor mutation stays disabled. Follow-up diagnostics may load only the already
recorded synthetic ID using the official API, with all native prompts
intercepted; they cannot spend another model turn. Future probe responses record
only bounded numeric error codes and known stop-reason enums, never raw errors.
The observer's replacement guard also now handles both common-worker PID rows
and preload rows carrying the actual worker in `child_pid`. A quota-free
regression proves that a live recorded MCP child prevents provider startup.
This correction does not turn the failed one-transport run into closure evidence.

One subsequent **no-model** official load addresses only that ledger-recorded
synthetic ID and canonical cwd. The production adapter forwards initialize and
`session/load`; the provider refuses load with **-32602** (test fails in 9.75 s).
It sends neither session/new nor a native prompt, performs no fallback, and
does not scan existing history. All recorded owned PIDs exit. This does not
identify the preceding prompt failure's cause or establish completed history.
The allowance stays 3/3; no further native model request or load retry is made.

Final quota-free fixture checks pass **6/6 (1.718 s)**, including both supported
MCP PID-row formats and rejection before provider startup when an observed
worker is still alive. The three default opt-in bodies skip before provider
launch and pass **3/3 (0.16 s)**. C++ formatting, Python AST, Node syntax and
diff checks pass. Remaining validation is Cursor's native prompt failure and
completed-history/same-ID refresh after a successful turn. Further inference
requires a new explicit allowance; the current ledger cannot be reused.

### Offline review corrections after the exhausted Cursor probe

Independent review at `bd0ad70d25b5a023b0dc44127c24aaa38ba9d26e`
found two reliability defects in the test-only attestation preflight:

- **F1:** Sequence numbers were allocated in the socket-connect callback.
  Reordering delivery could make an invalid observed handshake appear ordered.
  The preload now allocates the sequence and serializes an immutable record
  synchronously at `send()` entry. Receiver checks use source sequence, reject
  duplicate stages and never derive order from delivery. A Node networking mock
  delivers the invalid observation order `spawn, initialize_result,
  initialize_sent, initialized_sent` as apparently valid stage order; the
  receiver rejects it. The valid observation order still passes when delivered
  out of order. A separate assertion mutates caller fields after observation
  and verifies the serialized event and trusted identity remain unchanged.
- **F2:** Historical handshake success remained true after the exact injected
  worker exited, and the wrapper used that fact as current readiness. The
  snapshot now exposes historical `attested`/`inventory` separately from `ready`
  and sticky `revoked`. The exact worker's exit, close, child error, stdout
  EOF/close/error or stdin close/error revokes readiness. Older handshake
  delivery cannot resurrect it. Unrelated-instance exits do not revoke it.
  The prompt path checks `ready` during the wait and again immediately before
  the barrier/reservation. Fake-wrapper regressions revoke it before the wait,
  after the wait and at that final check, with model selection already ACKed:
  no reservation, no forwarding and a zero-count mock ledger in every case.
  The real Node/Python fixture also verifies terminal events from the preload
  revoke readiness while historical handshake/inventory remain available.

The wrapper now records bounded diagnostics for the request phase, numeric RPC
error/exit status and allowlisted reason. Offline fake-provider cases distinguish
RPC rejection, stdout EOF/process exit, timeout, and assistant plan/usage text
patterns. Raw RPC messages and Auto configuration output are no longer logged;
synthetic secret/transcript sentinels are checked absent from evidence. A
30-second pending-request diagnostic and the explicit 10-second Auto control
timeout never retry or refund a reservation. Assistant pattern matching is only
an observation, not proof of entitlement failure. No historical native error
payload can be recovered by these changes.

These fixes do **not** establish the cause of the earlier Cursor prompt failure,
completed history or same-ID refresh. No provider executable, model request,
native load retry, auth/config/approval change or live probe is used in this
correction pass. The allowance remains **3/3 consumed** and Cursor mutation
remains **DENY**. All-provider acceptance remains blocked on the previously
documented Cursor runtime gap; offline review of these corrections is separate.

Focused verification for this correction passes: **9/9** attestation/diagnostic
regression methods (6.381 s), including nested lifecycle/barrier subcases;
**6/6** existing proposal/allowance methods (2.013 s); and **3/3** default opt-in
CTest bodies, skipped before provider launch (0.28 s). Python AST, both Node
syntax checks and `git diff --check` pass. No C++ source changed, so no native
rebuild or broad runtime matrix is used as evidence for this test-only patch.

### D-7 additional Cursor allowance

After the offline corrections received independent **no findings** at
`a372f7301417968b332c1c2bbcb3883fba9dce60`, the user separately authorized
**one additional Cursor turn (D-7)**. This is not a refund or reset of D-3.
The additional private ledger is bound by SHA-256 to the unchanged consumed
D-3 ledger, with the same owner, Cursor-only scope, total/per-provider maximum
one, and no recall or automatic retry. Reservation is durable before forwarding;
an ambiguous timeout remains consumed. Mock tests reject altered parent data,
wrong decision/owner/provider, widened scope, recall/retry and a reused allowance.

The test retains the same approved synthetic cwd/server identity and official
inherited auth route, explicitly selects `claude-haiku-5-5-low`, and requires
native cheap-model ACK plus exact injected-worker current readiness before
delivery. No tools or mutation grants are issued. The adapter's same-ID
replacement check now reads current `ready`, rather than historical `attested`.
No-model control precedes the only additional text-only prompt; failure stops
without automatic model retry. The replacement transport may restore history
and readiness but cannot forward another prompt. Cursor production mutation
remains **DENY** pending sufficient evidence and independent review.

Before the additional native run, quota-free checks pass **8/8** proposal/budget
methods (3.078 s) and **9/9** attestation/diagnostic methods (8.380 s).
The installed Cursor module and the approved synthetic fixture retain their
pinned hashes and exact canonical launch identity. The
[official Cursor pricing table](https://cursor.com/docs/models-and-pricing),
checked again for this allowance, lists Haiku 5.5 at $0.10 input/$0.50 output
per million tokens; explicit native model selection must still ACK before any
prompt. No automatic model fallback is permitted.

The focused Cursor test target rebuild succeeds (existing unrelated compiler
warnings remain); its default opt-in body passes/skips before any provider
launch (0.86 s). Changed C++ formatting, Python AST and diff checks pass.

The actual-adapter no-model control at
`cb8d4de09bd4552084f4dd6075cbcb8a0a8ecc2a` **passes in 12.612 s**:
initialize/new, explicit native Haiku ACK and exact injected-worker readiness
succeed. No native prompt is forwarded and D-7 remains unused at that barrier.
All recorded owned processes exit before the separate live invocation.

The sole additional text-only prompt is then reserved and forwarded once at
that same head. The native ACP response has **no RPC error**, reports
`stopReason: end_turn`, and the injected instance supplies its approved
inventory lazily. Nevertheless the adapter test **fails in 14.114 s** because
its canonical event path sets the failure flag. No native permission request,
tool-call observation, rejected observer event or second transport occurs.
The first failure prevents entry into the same-ID load/refresh phase. All
recorded owned PIDs are confirmed gone. Native response success does not
establish completed assistant history or successful backend inference.

The native RPC diagnostics now distinguish this from RPC rejection, EOF or
timeout. They do not retain the original canonical `runtime.error` payload or
assistant text. The existing adapter can classify assistant plan/usage text as
failure even after an `end_turn` response; process/protocol failure is another
failure path. These source paths are **candidates, not proof** of the observed
cause. No plan, usage, auth, model mapping, LSP or earlier F1/F2 cause is assigned
from the available data. The isolated data directory contains scoped project
metadata, approvals and worker logs, not completed-history proof. No personal
transcript is read to fill that gap.

This run also exposes a postmortem observer limitation: when the owned provider
is terminated during cleanup, its Node child terminal callbacks may not reach
the observer. The pre-fix final snapshot retained `ready: true` although the
owned PIDs had exited. That historical artifact is preserved unchanged. The
subsequent **offline** correction explicitly revokes readiness on provider
channel EOF/error and owned exit, without fabricating a child-exit observation.
Regressions prove the historical handshake/inventory survive, readiness cannot
resurrect, and provider-transport loss before the prompt barrier neither reserves
nor forwards. No native rerun validates this correction yet.

The test additionally gains passive canonical diagnostics that log only
allowlisted adapter phase/reason enums (including plan/usage pattern observations
and known completion states). A quota-free C++ fixture verifies no raw message,
unknown status, identifier or transcript field escapes classification. This
cannot recover the D-7 canonical payload retrospectively and does not identify
its cause. It requires no further provider call.

**D-3 remains 3/3 consumed and byte-for-byte unchanged; the separate D-7 Cursor
allowance is 1/1 consumed.** Neither ledger may be reset, retried or refunded.
No recall, second native prompt, load retry, auth/default-config/approval change,
fallback model or production mutation enable occurs. Cursor remains **DENY**;
completed-history/same-ID refresh and the actual failure cause remain blocked.

Post-run offline verification passes **10/10** attestation/diagnostic methods
(13.688 s), including provider-transport loss at each barrier checkpoint, and
**8/8** proposal/budget methods (3.416 s). The focused Cursor target rebuilds;
default CTest passes the always-offline canonical classifier and skips native
validation before launch (one CTest target, 0.56 s). C++ formatting, Python AST
and diff checks pass. Final cleanup independently confirms all four recorded
D-7 PIDs gone and zero observer temporary directories. These offline checks
do not rerun or upgrade the failed native D-7 result.

The offline correlation then exercises the production adapter with the existing
fake ACP provider: its `always-upgrade` branch deliberately emits assistant plan
text followed by a successful `end_turn` RPC result. The strengthened regression
asserts canonical `turn.completed: failed` and no completed assistant item.
Together with the Auto plan-refusal and team-usage error cases, **three focused
adapter fixtures pass** (2.730 s; five Qt passes including setup/cleanup). This
proves the adapter's conditional refusal path without weakening it. It still
does not establish which payload occurred in D-7. A bounded read of the existing
scoped worker log finds no entry associated with that owned synthetic ID, so it
cannot supply the missing canonical reason. No further native prompt is sent.

### Separate ten-prompt Cursor validation budget

The user subsequently supplies a direct additional allowance of at most ten
Cursor prompts. This does not reset the exhausted D-3 or D-7 ledgers. Its
private audit record retains the original instruction reference as
`direct_user_instruction`, without inventing a new decision answer. The new
Cursor-only ledger binds both consumed parents and their existing hash chain;
only the assigned owner may reserve each new invocation. A monotonically
numbered journal records purpose, source head and model in a durable atomic
reservation before forwarding. Ambiguous delivery stays consumed. No Codex/Copilot allowance,
auth/configuration/approval change or production mutation enable is added.

The initial additional probe has one purpose: capture the bounded canonical
failure classification missing from the D-7 artifact, using the same explicit
Haiku low model and approved synthetic launch identity. No-model readiness
control precedes delivery. Failure stops for metadata classification rather
than triggering consecutive retries. Completion permits the existing same-ID
load/refresh verification with its replacement prompt intercepted. Any future
authorized follow-up must be counted separately, and validation stops before
the maximum if the required evidence is already sufficient.

Preparation checks pass without a native model request: four additional-budget
unit cases, nine proposal/adapter fixture cases and ten attestation regressions.
The focused Cursor test target rebuilds, and its default CTest invocation passes
the classifier while skipping the opt-in native case. Live results must be
recorded separately; these preparation checks do not consume or attest a model
turn.

The first additional run at source head
`047c4da369ab87300fc892ffc67f3b651d55aa2a` consumes **1/10**. Its preceding
no-model control passes with zero forwarded prompts and confirmed owned-process
cleanup. The live adapter accepts the same explicit Haiku low model and sends
one synthetic text prompt. The injected instance supplies inventory. Native
prompt RPC returns a response without a numeric error, but the canonical adapter
reports `adapter_runtime: plan_limit_pattern`, followed by
`adapter_turn: failed`. Thus this artifact identifies the plan-wording refusal
path that an RPC success alone could not distinguish. It does not establish the
account's exact entitlement, the affected set of models, or the cause of the
earlier D-7 run.

The run stops after that failure: no consecutive retry, history load, recall or
second prompt is attempted. All recorded owned processes exit. The private
ordinal retains the outcome and summary hash; both consumed parent ledger hashes
remain unchanged. Nine additional Cursor prompts remain unspent. Completed
history and same-ID refresh remain unverified for Cursor, and its production
mutation gate remains denied. The next verification must address the classified
refusal before another deliberate model request; fresh login, broader approval
or an automatic model fallback is not implied by this result.

### Read-only refusal controls and assistant-text false-positive regression

An offline probe linked to the actual daemon classifier reproduces a generic
false-positive: ordinary explanatory prose quoting billing wording matches the
broad plan substring detector. The synthetic marker and unrelated transport
failure do not match. This establishes a classifier limitation, not that the
first additional live run was a false-positive. That run intentionally retained
only the aggregate classification; neither its QTest log nor a bounded read of
its scoped worker log supplies the native response associated with its own ID.
Its exact native P1–P4 match therefore cannot be recovered retrospectively.

Assistant-content refusal handling now requires a recognized refusal lead,
optionally preceded by `ActionRequiredError:`. Streaming prefixes remain held
until disambiguated. Ordinary explanatory prose quoting the four plan phrases
and a usage-limit phrase completes normally; six known plan/usage refusals,
including split chunks, remain failures without an assistant answer. Explicit
RPC errors retain the existing broad classification and display behavior. This
is a text heuristic, not a provider entitlement authority; new refusal wording
still needs independent validation.

The private native observer additionally records only allowlisted rule IDs
P1–P6/U1–U2 and a `plan`, `model` or `quota` category from its owned prompt stream.
Split chunks, duplicate suppression and request completion boundaries are
covered offline. Raw response text stays unrecorded. Rule observations do not
establish backend delivery, account entitlement or the cause of an earlier run.

Read-only CLI `models` and `about --format json` controls on version
`2026.11.11-043ef28` report an account catalog containing Haiku and Composer 2.5,
and a Team tier label. They expose no usable remaining quota or per-model grant.
The official [CLI parameter reference](https://cursor.com/docs/cli/reference/parameters)
documents these controls. The [current model and pricing page](https://cursor.com/docs/models-and-pricing)
lists non-fast Composer 2.5 at $0.50 input/$2.50 output per million tokens and
distinguishes the Cursor-model pool from third-party model credit. This makes
Composer a concrete cheap named candidate, but catalog presence and a tier label
alone do not prove this CLI session's entitlement or remaining pool. No second
additional prompt is sent during these controls or offline fixes; the budget
remains **1/10**, and Cursor mutation remains denied pending review and runtime
evidence.

Focused validation of this offline change passes: both C++ targets rebuild;
the Cursor adapter suite and bounded-refresh classifier/default native skip pass
(2/2 CTest targets, 27.32 seconds). Python attestation/diagnostics passes 11/11,
proposal fixtures 9/9 and additional-budget units 4/4. AST, C++ formatting and
diff checks pass. No native model or history request is part of those checks.
