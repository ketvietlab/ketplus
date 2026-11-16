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

## Final provider gaps after the real-adapter control

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
