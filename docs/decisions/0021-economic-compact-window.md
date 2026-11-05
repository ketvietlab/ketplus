# 0021: Provider-native economic compact window

Status: accepted (explicit session policy; automatic cache-aware selection remains planned)
Date: 2026-10-07

## Context

Physical context capacity and a useful working-context budget are different. A
model can accept about one million tokens while a session asks its runtime to
compact earlier. KetPlus owns the policy shared by Codex and Claude; the provider
owns compaction, its summary, and the actual threshold after model resolution.

Phone-driven sessions often have longer gaps between requests. A warm prompt
prefix can make a long conversation cheap, while a cache miss makes the same
history expensive again. Idle time alone does not prove a hit or miss: retention,
routing, model changes and prefix changes also matter. A long-running turn can
refresh the cache after its user message, so the last user-message timestamp is
not the last cache-use timestamp.

### Pricing evidence

Verified on 2026-10-07. These are direct API Standard USD rates per million
tokens, not subscription allowances, Router prices or negotiated organization
rates. Rates are documentation evidence, not constants in the daemon.

| Model | Input | Cache read | Cache write | Output | Long-context treatment |
| --- | ---: | ---: | --- | ---: | --- |
| GPT-6.1 Sol | $2 | $0.10 | $2.50 | $10 | Above 272k input tokens: input/read/write rates double, output is 1.5x for the request |
| Claude Sonnet 4.6 | $3 | $0.30 | $3.75 (5m), $6 (1h) | $15 | Claude 4.6 and later have standard rates across the full supported 1M window |

Sources: [GPT-6.1 Sol model pricing](https://developers.openai.com/api/docs/models/gpt-6.1-sol),
[OpenAI pricing](https://developers.openai.com/api/docs/pricing),
and [Anthropic pricing](https://platform.claude.com/docs/en/about-claude/pricing).

OpenAI GPT-5.6 and later document a 30-minute minimum lifetime refreshed on
reuse, 1.25x writes, and model-specific discounted reads. Earlier models have
different `in_memory`/`24h` retention behavior. Anthropic documents 5-minute and
1-hour writes, refreshed by cache reuse. These API policies do not establish
which retention a CLI, gateway or subscription actually uses. See
[OpenAI prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching)
and [Anthropic prompt caching](https://platform.claude.com/docs/en/build-with-claude/prompt-caching).

### Cost model

Let `L` be the reusable old prefix, `S` the compacted prefix, `N` the expected
future reuses, `r` the cache-read rate, `w` the write rate, and `C` the native
compaction call's complete cost (input, cache operations and output). Ignoring
unchanged new input and output, an approximate warm comparison is:

```text
keep:    N * L * r
compact: C + S * w + (N - 1) * S * r
```

If the next request must rewrite an expired prefix, replace the first term in
`keep` with `L * w`; uncached content uses its ordinary input rate. Apply any
long-context tier to the entire eligible request. This is a planning estimate,
not a billing calculator: partial hits and retained stable prefixes change it.

Cold-cache compaction can have a better break-even point, but still pays to read
the old history for the native compaction call. One more turn is not guaranteed
to save money. A 272k request is also not a guaranteed price ceiling: the next
prompt, tool definitions/results, hidden context and provider counting need
headroom. We do not make 272k a universal automatic default.

## Decision

### Contract and default

Use `compactWindow`, in tokens, for the requested native auto-compaction window.
Do not use `maxContext` or change physical model capacity.

- A JSON integer from 100,000 through 1,000,000 selects a window. This portable
  range matches Claude's CLI range; the provider further clamps to its model.
- An omitted field inherits the persisted request; a new session inherits the
  provider's configuration. `null` removes KetPlus's override and inherits saved
  CLI/user settings or environment overrides that the provider normally honors.
- Strings, suffixes, fractions, booleans, zero and out-of-range values fail with
  `invalid_compact_window` before creating/queuing a turn.
- Cursor has no mapped knob. A non-null requested window there fails with
  `unsupported_compact_window`; clear it explicitly when handing off to Cursor.
- Persist the request in the private SQLite journal (`compact_window`, zero for
  inheritance). It survives daemon restarts, provider handoffs and native forks.
  Existing journals migrate to inheritance without changing models or history.
- Session configuration/list/events expose `effective.compactWindow` as the
  **requested** policy. It is not a provider-resolved threshold or usage total.

The existing daemon command is `session.start_turn`, for both new and resumed
conversations. It accepts `payload.options.compactWindow`; do not invent separate
daemon `session.start`/`session.resume` methods. An idle session can also change
it through revision-fenced `session.configuration.apply`. Minor-1 clients can
discover `session.compact_window/1` in the negotiated feature list.

The default is provider-owned behavior. Until payer and actual runtime cache
retention are known, elapsed time cannot justify an automatic economic window.
Explicit requests work independently of those future inputs.

### Manual controls

Desktop Qt and iOS SwiftUI reuse their native model-selection
surface for Auto, 272k, 500k, 1M and Custom. Auto sends null to clear the explicit
policy; omission retains compatibility with older daemons. Custom accepts whole
token counts in the portable range and cannot exceed a reported model capacity.
Unknown capacity is labeled rather than inferred from a model name.

Model, reasoning and compact-window choices form one draft. Apply carries the
revision captured when the picker opened; Cancel discards the draft. A stale
revision keeps the draft visible, reads the authoritative configuration and
uses its new fence only on explicit Retry. Active turns, background work and
disconnection prevent policy changes. UI changes take effect on the next turn;
they do not trigger a separate compaction request.

The iOS integration uses its existing authenticated control-v2 transport.
Android implementation is explicitly deferred and its legacy preview transport
is unchanged.

### Provider mapping

| Provider | Mapping | Preserved behavior |
| --- | --- | --- |
| Codex | `config.model_auto_compact_token_limit = compactWindow`, `config.model_auto_compact_token_limit_scope = "total"` on `thread/start`, `thread/resume`, `thread/fork` | No `model_context_window` override; model capacity is unchanged. Scope counts full active context rather than growth after an inherited prefix. |
| Claude | SDK `extraArgs: { autocompact: String(compactWindow) }` on each new/resumed query | No `options.env`, no replacement environment, no `CLAUDE_CODE_MAX_CONTEXT_TOKENS`, no saved settings mutation. |

Codex re-enters `thread/resume` before the next turn when the requested window
differs from the loaded thread's request. An unchanged policy reuses the thread.
Removing an override resumes without KetPlus's compact config.

Claude query reuse includes the window in its launch identity. Changing it
requires a fresh query. The daemon and sidecar reject that change while live or
stopping background tasks remain, preserving the query that owns those tools.
An inherited `CLAUDE_CODE_AUTO_COMPACT_WINDOW` has higher priority even than the
CLI flag: the requested window is not necessarily the resolved threshold.

These mappings follow the [Codex configuration reference](https://learn.chatgpt.com/docs/config-file/config-reference),
[Codex app-server lifecycle](https://learn.chatgpt.com/docs/app-server), and
[Claude auto-compact configuration](https://code.claude.com/docs/en/model-config#context-window-and-auto-compaction).
The Claude SDK pin is 0.3.289; the input investigation used Codex CLI 0.160.0.
Our deterministic tests verify wiring without invoking either model. A native
Codex 0.160.0 metadata-only probe accepted start and successive resumes requesting
272k and 500k after flushing an empty rollout through archive/unarchive. It sent
no `turn/start`; its generated rollout was archived and removed afterward. This
checks request acceptance, not the unreported resolved threshold or compaction.

### Cache-aware selection policy (planned)

Evaluate only before a new turn, when no foreground turn, approval or background
tool is active. Required inputs are the payer (direct API/BYOK, Router,
subscription, organization policy or unknown), actual provider/model/endpoint
retention, last provider request/cache-use time, current context occupancy and
native threshold, and expected next-request headroom.

1. An explicit session window or organization policy takes precedence.
2. For unknown payer, retention, occupancy or time, retain provider behavior.
3. For a plausibly warm prefix, retain the broad window and avoid invalidating
   cache merely because the conversation is long.
4. After a likely expiry, choose a model/payer-specific economic window only
   when estimated future savings justify compaction cost and quality risk.
5. Record the decision reason and uncertainty; a time-based estimate must never
   be presented as an observed cache miss. Subscription limits are not API USD.

The current protocol implements the explicit request, not this automatic
scheduler. It does not infer a payer from the device, hard-code a shared TTL,
call a summarizer, or trigger an additional compaction request.

### Boundaries and observation

KetPlus changes the policy at idle turn boundaries. The knobs above do **not**
guarantee that native auto-compaction only happens at a turn boundary: a provider
can compact during its own request/tool loop. Codex's post-turn threshold knob
does not establish a cross-provider boundary-only guarantee. That stricter mode
needs a verified provider capability before it can be offered.

Keep provider `modelContextWindow` separate from `compactWindow`. Claude's
`autoCompactThreshold` is retained in normalized `usage.context.tokenUsage` when
reported. Codex does not currently return the resolved auto-compact threshold;
leave it unknown. Existing native `contextCompaction` transcript items remain the
source of compaction markers. The manual controls expose the requested window,
reported physical capacity and blocked/retry reasons. Automatic cache/payer
selection and its decision-reason indicators remain subsequent work.

Plans, todos and durable decisions should live in workspace files that providers
can re-read after compaction. Do not silently synthesize a second memory system
or replace native compaction with KetPlus transcript summarization.

## Consequences

The policy stays private, provider-neutral and opt-in. Public CM, model identity,
licensing and provider credentials are unaffected. Explicit requests can improve
economics but may reduce remembered detail, may be capped by the model, and may
lose to higher-priority provider configuration. We do not promise cost savings.

The existing large `SessionHost.cpp` receives only lifecycle/configuration glue;
shared validation and Codex mapping live in `CompactWindowPolicy.*`. The existing
large `LocalJournal.cpp` receives only its column migration and session read/write
bindings. Moving unrelated lifecycle/storage code would obscure this change.

## Alternatives considered

- Override physical capacity: misrepresents the model and uses Claude's custom
  gateway capacity variable for the wrong purpose.
- A global 272k limit or a five-minute timeout: ignores price tiers, actual
  retention, cache writes, payer and quality.
- A Claude environment override: risks replacing the entire SDK environment and
  has provider setting-precedence concerns. Use the per-launch CLI flag.
- A custom summarizer or forced mid-tool compaction: duplicates provider logic,
  consumes extra quota and breaks the native lifecycle boundary.
