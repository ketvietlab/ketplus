# 0027: Conversation numbers and agent-to-agent messaging

Status: accepted
Date: 2026-11-01

See also: ADR 0114 (agent teams) adds roles, tasks, digests and role addressing on
top of these messages; nothing here changes.

## Context

Claude, Codex and Cursor conversations run under one daemon (ADR 0002), but an
agent cannot address another conversation. A user who wants one conversation to
consult another copies text by hand, and provider-native subagents stay inside
one provider. The daemon already owns every conversation's journal, turn
lifecycle and steering, injects host MCP servers for all providers (ADR 0011)
and delivers host routing instructions (ADR 0013). It can broker messages
between conversations without a new process or a cloud dependency.

Product scope decided for this feature:

- Numbers and messaging are local to one daemon. There is no cross-host
  addressing and no relay routing.
- A message to an idle conversation starts a turn automatically.
- Automatic turns are not restricted by the receiver's execution policy,
  including bypass.
- Users can reference conversations by number in the composer.

## Decision

### Conversation numbers

- Every conversation has a host-unique number, displayed as `#4821`. The UUID
  session ID remains the identity; the number is an alias stored in
  `conversation_numbers(number, session_id, generation, assigned_at, released_at)`.
- Numbers are four digits, `1000`–`9999`, allocated uniformly at random among
  free values rather than sequentially, so a mistyped number usually names no
  conversation. Protocols carry numbers as strings.
- New conversations and forks receive a number when created; a fork is a new
  session and never inherits its source's number. Every daemon start backfills
  conversations that have none, so journals written before numbers existed are
  numbered the first time an upgraded daemon opens them.
- Provider handoff, model changes and worktree switches keep the number. It
  names the conversation, not a provider thread.
- Permanent deletion releases the number into a 30-day quarantine in the same
  transaction. When more than half of the four-digit range is assigned or
  quarantined, new conversations receive five-digit numbers. An assigned number
  never changes.
- `session.list`, `session.start_turn` and `session.fork` results expose
  `number`. Desktop shows `#4821` at the start of every sidebar conversation row
  and before the title in the conversation header.

### Broker

A new daemon module, `apps/daemon/src/agents/`, owns `AgentBroker` (lookup,
listing, delivery and the loop guard), the provider instructions and launch
configuration, and the MCP server. Number allocation and durable messages live
in the journal (`LocalJournalAgents.cpp`). The broker reaches `SessionHost` only
through the narrow `AgentHostPort` start-turn, steer and idle-transition hooks.

`agent_messages` stores the message ID, `chain_id`, `hop`, sender and receiver
session/generation/number, kind (`request`, `reply`, `notice`), `reply_to`, a
UTF-8 body of at most 32 KiB, state (`queued`, `delivered`, `answered`, `held`,
`expired`, `failed`), the delivering turn, and creation/expiry times.

Delivery depends on the receiver:

- **Idle** (no open turn, background task, pending approval or pending
  question): the daemon starts a turn with the receiver's own model, mode and
  execution policy. Its provider input is a host block naming the sender's
  number, title and provider, followed by the message. `user.message` remains
  the turn boundary for turn sequences, windows, activity groups and handoff,
  so the journal records the message text there with `origin: "agent"` and the
  `agent_messages` it delivered. Clients label that row with its sender instead
  of showing it as the user's text; older clients show the plain text. An agent
  message never becomes a conversation title.
- **Busy**: the message is queued. Queued messages are delivered together, in
  order, when the receiver becomes idle, or prepended as one host block to the
  next turn the user starts, whichever comes first.
- **`urgent`**: delivered through `session.steer` when the receiver's provider
  supports steering into its active turn; otherwise queued. The journaled
  `turn.steered` carries the same `origin` fields.
- **Provider failure**: the message becomes `failed` and the sender's next turn
  receives the error.

`agents_send` returns the message ID and the receiver's number, title, provider
and delivery state (`starting`, `queued` or `steered`) so the sender can verify
the address.

A request with `expect_reply` is answered by an explicit
`agents_send(reply_to=…)`. If the woken turn ends without one, the daemon sends
that turn's final assistant message as the reply. Replies follow the same
delivery rules, so an idle sender is woken. Unanswered requests expire after 24
hours and the sender is notified. Turns started by a message use reply-oriented
response guidance without the progress matrix. The guidance depends on what
was delivered. When any delivered message has `expect_reply`, including a reply
that asks something back, the final message becomes the reply. Otherwise
nothing is forwarded, so the guidance and each delivered
reply tell the agent to send any follow-up the sender asked for (a fix to
re-review, a result) with `agents_send(reply_to=…)`. Otherwise, an agent that
ends with a message addressed to the sender leaves it unsent in its own
transcript.

### Loop guard

Automatic delivery spends provider quota without a user prompt, so these limits
are mandatory:

- A user-started turn begins a new chain at hop 0. A message sent during a
  message-started turn inherits that message's `chain_id` with `hop + 1`. The
  daemon derives both from the turn; agents cannot set them.
- Automatic turns stop at hop 6, after 30 automatic turns in one chain, or after
  20 automatic turns per receiver per hour.
- Over a limit, the message is `held`. Both conversations show a notice card,
  and held messages are delivered with the receiver's next user-started turn.
- A conversation cannot message itself.
- A sender with 20 undelivered (`queued` or `held`) messages to one receiver
  gets `receiver_backlog` until the receiver takes some. Other senders are not
  affected.
- One turn takes the oldest queued messages whose serialized prompt block and
  `agent_messages` entry (escapes and sender title included) fit 240 KiB,
  leaving room for the rest of the 256 KiB journal event. The rest wait for the
  next turn. `agents.send` refuses with `message_too_large` a message that could
  never fit on its own; one accepted earlier that does not fit is marked
  `failed` alone instead of failing its batch.

### Explicit Desktop release of held messages (#130)

The UI implementation stack adds two authenticated **local Desktop** operations:
`session.agent_messages.read` and `session.agent_messages.release`. Both take the
receiving session/generation and `payload.message_ids` (1–100 unique IDs for
release). They are separately advertised in `daemon.hello` capabilities. An old
daemon leaves the UI with an update/status error, never an emulated user prompt.
Agent-token connections and cloud-control clients cannot invoke these operations;
the `agents.*` MCP surface does not expose them. Generation checks run before a
cached command result can be returned.

Desktop renders an incoming held notice with sender/reason, a one-message scope,
and **Receive and continue**. Reading it only queries state. Clicking is the user
authorization and states that an agent turn/provider quota may follow. Pending
clicks are fenced, failures can refresh/retry, and a conversation switch rejects
late responses. Multiple IDs are supported by the protocol, but the current UI
explicitly releases one visible message per action.

The journal atomically validates the whole selection, rejects stale/expired or
already-delivered messages and marks only those IDs queued with
`delivery=user_authorized:<command-id>`. The existing command result cache makes
transport retries idempotent. The broker delivers one authorized batch separately
from unrelated queued messages, bypassing hop/chain/hourly guards for that batch
only. A busy receiver waits until its normal turn finishes, even for an urgent
message. New automatic messages retain their existing limits and reply semantics.
A new ordinary user turn can still consume pending held/authorized messages.

Before invoking the provider, the broker persists a `releasing` claim and the
expected turn sequence. The journaled input carries `user_release_id` and the
original attributed message IDs. After a restart, that marker proves delivery:
claimed inputs already journaled are not started again; a claim with no input is
failed and requires another explicit retry. A journal read failure never grants
permission to repeat the turn. Authorization, progress and outcome are separate
`agent_message.state` notices, rather than forged user prose. No installed daemon
is replaced by this implementation work; packaging/version distribution remains
a later phase.

### Sender identity and trust

- The daemon mints a random 256-bit capability token per conversation
  generation for the life of the daemon process and passes it in the
  environment of that conversation's `ketplus_agents` MCP server. Codex and
  Cursor receive that configuration over their JSON-RPC connection. The Claude
  Agent SDK passes `mcpServers` to the CLI as `--mcp-config` JSON, so on macOS
  the token is visible in that process's arguments to local users. Accepted
  (#69): another user cannot reach the owner-only socket inside `~/Library`,
  and a process of the same user can already read the daemon credential. The
  server
  presents the token in `daemon.hello` as `agent_token`; the daemon derives the
  sender from it, and tool arguments never name the sender. Tokens are not
  journaled, deletion revokes them, and providers are restarted with the daemon.
- The MCP server is a mode of the daemon executable
  (`ketplus_daemon --mcp-agents`), so messaging does not depend on Node. It opens
  a short authenticated connection per tool call: providers keep one server per
  thread, and idle servers must not hold the daemon's limited local connection
  slots. It uses the endpoint's absolute path, not one resolved from the
  provider's environment.
- Token-authenticated connections have a narrow scope: the four `agents.*`
  operations below. Any other operation fails with `forbidden`; they cannot
  answer approvals or questions, change configuration, or read another
  conversation's transcript, and session events are never broadcast to them.
- Any process running as the same user with shell access can read the
  owner-only daemon token. The scoped token prevents spoofing through the tools
  and accidental misuse; it is not a defense against a hostile local process.
- The host block marks the content as coming from another agent and states that
  it does not override user or system instructions. Per the product decision,
  the receiver's execution policy is not narrowed. A bypass conversation woken
  by a peer runs with bypass. This means content a peer has read (web pages,
  files, tool output) can influence a bypass conversation with no user in the
  loop. This risk is accepted.

### Agent tools

`ketplus_agents` exposes:

- `agents_whoami`: own number, title, provider, model and worktree.
- `agents_list({query?, limit?})`: non-deleted conversations on this host with
  number, title, provider/model, state (idle, running, waiting), repository,
  worktree and last activity. The caller is included and flagged.
- `agents_send({to, text, expect_reply?, reply_to?, urgent?})`.
- `agents_inbox({message_id?, limit?})`: read-only list of recent messages the
  caller sent or received with their states (bodies truncated to 2,000 bytes),
  or one message in full by `message_id`.

Injection follows ADR 0011 and ADR 0013 and receives the same approval treatment
as `ketplus_browser`:

- **Claude**: `mcpServers` on every query beside `ketplus_browser`. Routing
  instructions are appended to the system prompt. `mcp__ketplus_agents__*` is
  granted only when the daemon injected the server.
- **Cursor**: ACP `mcpServers` on `session/new` and `session/load`. Routing goes
  in the leading host text block, and the same `providerIdentifier` check
  applies.
- **Codex**: per-thread `config` on `thread/start`, `thread/resume` and
  `thread/fork`, with dotted keys `mcp_servers.ketplus_agents.{command,args,env}`.
  Routing goes in `developerInstructions`. A metadata-only probe against Codex
  0.162.1, with no model turn, verified the following. Two threads in one
  app-server launched separate server processes, each with its own `env`, and a
  process-level server stayed present beside them. A nested `mcp_servers` object
  in the same field replaced the process-level table, so it must not be used:
  it would drop `ketplus_browser`. A second probe confirmed that
  `default_tools_approval_mode` and the compact window are accepted together in
  per-thread config. The daemon also approves a `ketplus_agents` MCP
  elicitation for a thread it injected. Per-thread config on `thread/fork`
  could not be probed without a model turn (Codex has no rollout to fork); the
  fork's own config is sent there and again on the fork's first resume.

### User references

- `@` remains file mentions, except `@agent/`. Typing `#` or `@agent/` in the
  composer offers conversations by number and title; choosing one inserts
  `#4821` or `@agent/4821`, as the user started it.
- On submit and steering, `#NNNN` and `@agent/NNNN` tokens that name a live conversation on this
  host are listed for the agent in a preamble with number, title, provider and
  state, in the same way as referenced paths. Unresolved tokens stay plain text.
- A reference never sends a message by itself; the agent calls `agents_send`
  when the user asks it to. `#1234` can also be an issue or pull-request number,
  so the preamble says references are for contacting conversations on request.
  `agents_send` and the team tools accept a number in any of these forms.
- Delivered agent messages appear as an outlined bubble labelled
  "From @agent/1093 · title · provider", as the Qt mock draws it (earlier
  builds wrote "Message from …" or "Reply from …").

### Transcript and clients

- Delivery is the `user.message` (or `turn.steered`) described above. A new
  `agent_message.state` event tells the sender that a message was sent (`sent`,
  with the receiver's number, title and provider, the delivery state and a
  500-character excerpt of the text) or held, failed
  or expired, and tells the receiver that its final answer was sent as the reply
  (`replied`) or that a message is held. The broker emits `sent` and `replied`
  itself, because each provider presents its MCP call differently. Desktop
  renders the event as an "Agent message" note; the sender's `agents_send` call
  also stays visible as its MCP tool row.
- While a message-started turn runs, the broker keeps its sender in memory.
  `session.list` adds it as `agent_turn`, and Desktop ends that conversation's
  sidebar row with "from #4821" until the turn ends.
- The relay and mobile receive these events through the existing encrypted
  projection as additive types, and older clients ignore them. The phone gets
  each conversation's number, and the sender of a running message-started turn
  (`agentTurn`), in its sealed session metadata beside the branch, so the relay
  never reads either. Mobile shows numbers, agent-message bubbles and notes, the
  woken row's sender, and its own `#` picker, as the desktop does.
- Minor capability `agents.messaging/1` advertises the feature;
  `session.list` adds `number`.

### Out of scope

Cross-host addressing, spawning new conversations through a tool,
per-conversation receive settings, and addressing provider subagents.

## Consequences

- Agents on different providers can coordinate without the user copying text,
  and a number survives provider handoff.
- Automatic turns consume the receiver's provider quota without a prompt. The
  loop guard bounds this cost but does not eliminate it.
- Bypass conversations can act on peer content without review (accepted above).
- Journal migration adds `conversation_numbers` and `agent_messages` and backfills
  numbers. The deletion transaction also releases the number, revokes the token
  and expires pending messages.

Verification uses fake providers and consumes no quota. It covers:

- delivery to idle and busy receivers, batched queues and the automatic reply
  from the final message;
- `urgent` steering of a running turn;
- the hop, chain and hourly limits holding messages, release with the
  receiver's next user turn (including `#NNNN` references) and the 24-hour
  request expiry;
- numbering, restart stability, legacy-journal backfill and the deletion
  quarantine;
- forged tokens, invalid numbers, sizes and replies, and denial of
  out-of-scope IPC operations without event broadcasts;
- the injected server for each provider, the Claude sidecar allowance and the
  MCP tool mapping;
- `sent` and `replied` notices and the running automatic turn's sender in
  `session.list` and the sealed metadata;
- the Desktop projection of agent messages and notices, and the sidebar label.

Codex per-thread configuration is checked by a metadata-only probe, as in
ADR 0021. An authorized provider run on 2026-11-01 used an isolated 0.1.49
daemon (its own data directory and socket, not signed in to the relay) with
Claude Sonnet through the sidecar and Codex `gpt-5.6-luna` on Codex 0.162.1,
and confirmed the following with no approval prompts:

- Codex called `agents_send` with `expect_reply` and woke the idle Claude
  conversation. Claude's final answer returned as the reply and woke Codex.
- Claude loaded `mcp__ketplus_agents__agents_send` through its tool search and
  sent a notice that woke Codex.
- Both transcripts recorded the `sent` and `replied` notices.
- After a daemon restart, the resumed Codex thread called `agents_whoami` and
  received its own number.
- A fork received a new number, and its first turn's `agents_whoami` returned
  that number.

The fork's first turn resumes the forked thread with the fork's own config, so
Codex honouring per-thread config on `thread/fork` alone is still not isolated.
Cursor was not part of the run.

## Alternatives considered

- **Shared filesystem mailbox:** no sender identity, no wake-up and no
  transcript presentation.
- **Provider-native subagents:** limited to one provider.
- **Cloud broker:** unnecessary for a single host, and it adds latency and
  privacy exposure.
- **Synchronous tool that waits for the reply:** provider MCP timeouts differ,
  receiving turns can run for minutes, and mutual waits deadlock.
- **Shell CLI instead of MCP:** identity would rely on per-conversation shell
  environment, which a shared Codex app-server cannot vary. MCP reuses the
  existing injection paths.
- **`@` for conversations:** conflicts with file mentions of a folder named
  `4821`.
- **Sequential numbers:** an adjacent typo would usually reach a real
  conversation.
