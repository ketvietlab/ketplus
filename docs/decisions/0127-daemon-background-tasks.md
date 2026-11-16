# 0127: The daemon owns background tasks and their completion delivery

Status: accepted (contract; runtime implementation and platform verification pending)
Date: 2026-11-16

## Context

Background work needs the same lifecycle, output, Stop action and completion
delivery for every provider. The current implementation is Claude-specific:

- `apps/daemon/providers/claude-sidecar/index.mjs` keeps an SDK query open after
  its foreground result while `tasks.mjs` reports live tasks. SDK task
  notifications can start another model run inside that query. Stopping the
  foreground turn leaves background tasks running.
- `apps/daemon/src/runtime/SessionHostTasks.cpp` accepts only
  `session.task.stop` and `session.task.output`, routes them to Claude and
  checks membership in a bounded, 16-row snapshot. It maps provider loss to
  stopped display rows; that is not proof that descendants were reclaimed.
- `apps/daemon/src/storage/LocalJournalTasks.cpp` stores those provider
  snapshots for the current session generation, not durable execution intents,
  leases or a completion outbox.
- Codex uses app-server transport and Cursor uses ACP. Neither has a shared
  daemon-owned background execution API today. The shared desktop task bar
  consumes `tasks.changed` and `background_tasks` display metadata.
- `apps/daemon/src/teams/TeamCoordinatorStall.cpp` detects stalled team work
  and nudges members. It does not observe command results. Team waits are also
  separate from execution ownership (ADR 0114).

This ADR records the contract set by the team leader. The accompanying
`tests/daemon/fixtures/background-tasks-v1.json` is a behavioral contract and
fault-injection matrix, not a claim that the new runtime exists. Its vocabulary
describes constraints and the leader-defined response schema; it does not invent
a new serialized child-agent config, executor protocol or cursor encoding.
Those must reuse the existing KetPlus mechanisms and be reviewed with their
implementations.

## Decision

### 1. Authority and scope

The durable daemon job registry is the sole source of truth. V1 supports
`process` jobs and `agent` jobs managed by KetPlus. Provider adapters call the
same API; an open Claude query, a provider snapshot and the team watchdog never
own this lifecycle. The watchdog may detect a stall, but must not infer a job's
result or replay its execution.

Arbitrary detached shell work outside this API is outside managed-job scope.
Scheduling prompts (ADR 0126) and team tasks (ADR 0114) are separate records;
neither becomes a background job merely because it waits or runs automatically.

### 2. Authenticated API and start idempotency

| Agent tool | Daemon IPC operation | Responsibility |
| --- | --- | --- |
| `background_task_start` | `session.task.start` | Commit an intent; return its task identity |
| `background_task_list` | `session.task.list` | Page the complete owner registry |
| `background_task_get` | `session.task.get` | Read authoritative task metadata; paginate any collection |
| `background_task_stop` | `session.task.stop` | Request cancellation of this job only |
| `background_task_output` | `session.task.output` | Read a bounded byte-cursor output page |

Start accepts `client_request_id`, `kind`, `title`, a typed execution spec and
`timeout_ms`. A process spec supplies exactly one of an argv invocation or an
explicit shell command, plus `cwd` and an environment delta. Shell interpretation
must be explicit; argv is not silently joined into shell text. An agent spec
supplies child-turn configuration through KetPlus's managed-agent mechanism.

The managed-agent `execution` object is
`{provider?: string, model?: string, effort?: string, prompt: string, cwd?: string}`.
`prompt` is required. Omitted settings are resolved from the owner's configuration
and persisted at acceptance. Provider/model/effort must pass catalog validation;
cwd is canonical and within the existing workspace/permission boundary. Reject
`session_id`, tokens, `env`, `permissionMode` and system-prompt overrides.

Each agent job owns a separate managed child session, linked uniquely and durably
by `task_id`. Persist `child_session_id`, child generation and `parent_task_id`;
never substitute a message to an arbitrary conversation. The child is an ordinary
conversation with that ownership link, not an implicit team member or unowned
side conversation. Its one work turn and tool continuations use the existing
session/provider authorization and quota pipeline. A configured different provider
is allowed without expanding authority: intersect the owner's current policy
with workspace/provider policy. Child approvals and questions go through
ApprovalBroker/UI; neither parent approval nor child creation is a blanket grant.

An agent job completes only after its child turn and all descendant jobs are
terminal and no permission remains pending. Child failure is `failed`. After
crash, unknown inference is not replayed: reclaim the ownership tree, then mark
`interrupted`. Stop/cancel/timeout or invalid parent generation fences completion,
cancels child requests and reclaims every descendant job. Never kill a shared
provider process to cancel a child. Stopping the parent's foreground turn retains
the job under the normal wake fence. Nested jobs use the same registry and quotas
and cannot escape the ownership tree. Tests use fake providers.

The daemon derives `owner_session_id`, `owner_generation` and the originating
turn from the authenticated caller context, not arbitrary owner fields in the
payload. All operations validate ownership and the current generation; knowing
a task id is not authority to read output or stop it. Server-side owner/grant
checks govern Stop and output; clients never gate access by provider name.

The durable uniqueness key is
`(owner_session_id, owner_generation, client_request_id)`. Retrying the same
request returns the same `task_id` without another launch, including after a
lost response or daemon restart. Reusing that key with a different start spec
is an error. Comparison must include all execution-affecting input; it cannot
depend on a display projection or log secrets. Accepted is acknowledged only
after the intent transaction commits. Execution starts only after approval
through the existing permission system.
Validation, authentication or quota refusal before acceptance returns an API
error without creating a job or outbox event.

### 3. Durable record and identities

Each job record contains:

- `task_id`, `schema_version`, `owner_session_id`, `owner_generation`,
  `originating_turn_id`, `kind`, `title`, `state`, `revision`;
- timestamps, execution identity and the immutable execution spec/cwd;
- `exit_code`, `error`, `reason` as applicable;
- output cursor, size and truncation metadata; `completion_event_id` when terminal.

The registry also preserves the start uniqueness key and sufficient comparison
information to enforce retries. A process identity includes a verifiable birth
identity and executor lease identity. PID alone never authorizes adoption or
signalling; process group ids alone do not solve reuse either. Managed agent
identity identifies its child session/generation/turn through KetPlus's executor.

Tokens and the full inherited environment must never be stored in the journal
or exposed in UI. The environment delta is not permission to journal credentials.
Execution metadata and output projections follow existing credential and
owner-only storage boundaries. Output is untrusted data even if the job succeeds.

### 4. State machine and cancellation

The normal path is `queued -> starting -> running -> completed | failed`.
User cancellation follows `queued | starting | running -> stopping -> cancelled`.
A deadline follows `queued | starting | running -> stopping -> timed_out`.
Loss of execution that cannot be safely recovered ends as `interrupted` after
reconciliation confirms that containment has no remaining live workload.

`queued | starting -> failed` is allowed for `permission_denied`, `spawn_failed`
and `handshake_failed` only when no workload was launched or the executor and
all descendants are confirmed reclaimed. If cleanup takes time, use `stopping`
with `terminal_target=failed`; `stopping -> failed` requires cleanup confirmation.
An unverifiable identity or uncertain cleanup stays `stopping` with
`cleanup_pending`, continuing reconciliation and surfacing a warning. It must
not become terminal merely because the PID disappeared.

Every transition uses compare-and-swap on `revision`. A terminal state is
immutable. Stop is idempotent, including retries after termination. A concurrent
completion and Stop can commit only one terminal outcome; neither may overwrite
the winner or emit another completion event. Receiving a Stop request is not
proof of cancellation. `cancelled` and `timed_out` require confirmation that
the executor and all contained descendants are reclaimed. An already absent,
never-launched executor must be proven absent through its durable launch intent.
The user-selected macOS exception below replaces the absolute containment/cleanup
requirements for that platform; it does not replace durable ownership or permissions.

#### macOS native cleanup amendment (user decision D-2)

macOS process jobs run natively with `containment_mode=native_best_effort`, not
in a product VM. Supervisor, durable journal/handshake and lease remain mandatory.
Reclaim the process/group and identifiable descendants using birth identities;
never signal a PID merely because its number used to belong to the job.

Expose `cleanup_status=not_required | pending | verified | unverified |
best_effort_complete`. A normal root exit with every known process reclaimed and
no cleanup error ends in the normal terminal state according to the exit result,
with `best_effort_complete` and normal completion delivery. This is not `verified`
and does not promise absence of unobserved descendants. Lack of kernel containment
alone must not force every native job into an attention state.

Cleanup errors, known surviving processes, lost identity/lease or an unreconciled
restart instead produce nonterminal `needs_attention`,
`cleanup_status=unverified`, `reason=cleanup_unverified`. Preserve the root result,
exit code and output, plus manual-check guidance. Do not report clean success or
cancellation. Release the execution slot only once the known executor has stopped;
the retained attention record still counts against the 116 nonterminal cap and
is never automatically pruned, including after owner close/generation change.
Existing authenticated local administration may inspect it; do not invent a new
grant. Cleanup retries signal only identities that can still be validated and
never replay the command.

Only a user may invoke `session.task.resolve_cleanup` with `task_id` and revision.
CAS `needs_attention -> interrupted(reason=cleanup_acknowledged)` with a durable
audit whose source is `user`. Acknowledgement is a user statement, not kernel
cleanup evidence; keep the assurance level visible. Agents cannot invoke this
action on the user's behalf. Neither `needs_attention` nor acknowledgement wakes
an agent; publish a UI notice only. Permission, outbox, dedup and owner fences
remain unchanged.

### 5. Supervisor, launch and restart safety

An executor supervisor and lease protect the complete process tree. The
pre-launch handshake is durable: before any workload can execute, recovery can
identify and contact or reclaim its executor. The launch protocol must cover
crashes on both sides of spawn/register; persisting a PID after spawn leaves an
unacceptable ownership gap. A managed child agent has the same lifecycle
obligations, including work it launches through the managed mechanism.

Lease loss terminates contained work, then escalates to kill after a finite
grace period. Containment includes descendants, not just the direct QProcess.
An implementation must demonstrate the platform's containment behavior,
including escape attempts; a process group alone is not a blanket guarantee.
Except for the macOS native amendment above, if safe containment cannot be
provided on a platform, start is explicitly refused. The platform executor and its handshake are implementation gates,
not capabilities proven by this documentation.

On daemon restart, reconcile every nonterminal registry record with the
executor identity and lease. Reconnect only to a live, verifiable executor;
otherwise reclaim it and record `interrupted` only after confirming the
containment is clean. If that cannot be confirmed, keep `stopping/cleanup_pending`
and continue reconciliation and warnings. macOS follows the native amendment's
`needs_attention` path when restart cannot be reconciled. Never relaunch a command with
side effects to reconstruct its result, even when its last state was queued or
starting. Provider exit does not kill a daemon-owned process job.

Closing/deleting a session or replacing its generation cancels its jobs and
invalidates old wakes. Cancellation and reclamation must remain possible even
while the session is being removed; deleting a snapshot is insufficient. A cwd
change applies to new jobs only. An accepted job's cwd/spec remains immutable.

### 6. Transactional completion outbox

Commit the terminal record and its outbox event in the same transaction.
The event identity is derived from `(task_id, terminal_revision)`; its payload
contains `task_id`, terminal `state`, a summary and an output reference. The
notation `task_id + terminal_revision` means a stable, unambiguous compound
identity, not an invitation to concatenate ambiguous strings.

Only terminal transitions create completion wakes; progress creates none.
The dispatcher retries at least once until delivery or explicit suppression.
Accepting/enqueuing an owner turn and recording deduplication by `event_id` must
be durable together. A crash after enqueue but before the dispatcher sees the
ack therefore cannot enqueue a second turn. The receiver must use this acceptance
boundary, not a later in-memory "seen" flag.

This is at-least-once event delivery with durable acceptance deduplication.
It is not exactly-once model inference. A turn interrupted by a crash is not
automatically replayed to simulate an unobserved result.

T-2 owns the enqueue/accept API with `event_id`. Commit the deduplication entry
and accepted-turn record together before dispatching to a provider. Persist
`accepted` and `dispatching` so recovery never dispatches an inference whose
outcome is unknown. The provider/agent integration lane consumes this boundary;
message-only routing cannot replace it.

### 7. Owner wake policy

| Owner condition | Completion handling |
| --- | --- |
| Idle, eligible owner | Accept a follow-up turn for that owner |
| Busy in a turn | Enqueue after that turn; never urgent-steer into it |
| Waiting on permission or user decision | Keep pending; do not answer for the user |
| User Stop or pause | Keep pending; only explicit Resume releases it |
| Closed/deleted owner or replaced generation | Suppress with a recorded reason |
| Job cancelled by Stop or session close | Suppress; cancellation must not restart the agent |
| Native cleanup needs attention or is user-acknowledged | UI notice only; no auto-wake |
| Permission denied after job acceptance | `failed` with `reason=permission_denied`; suppress completion with the same reason |

Delivery is rechecked at acceptance and dispatch, including generation and Stop
fences. A follow-up includes `task_id`, result metadata and output references;
any output included in context is clearly untrusted task data, not instructions.
No pending hook may clear pause, fabricate permission or switch its owner.
Permission denial never auto-wakes or retries to ask for permission again.
Spawn/handshake failures after acceptance use the normal terminal/outbox path,
without automatically retrying the command.

Stopping a foreground agent turn preserves daemon process jobs under the existing
user behavior, but blocks their auto-wakes until explicit Resume. Stopping a job
cancels only that job. These controls must not be conflated.

### 8. Queries, output, retention and admission limits

List/get read the complete registry and paginate collections. List returns
`{tasks, next_cursor, revision, active_count}`. Cursors are opaque; snapshot
revision is used for gap detection/refetch. A 16-row UI
projection does not determine whether a job exists, is active, is eligible for
cleanup or may be stopped. Snapshots include `revision` and `active_count`.
An event gap or reconnect requires refetch, rather than reconstructing membership
from a partial event stream.

Output uses an opaque cursor representing a byte position, returns at most 8 KiB
per call and returns `{output, next_cursor, total_bytes, truncated}`. Clients
must pass cursors back unchanged, not parse or increment them. The spool is
bounded to 10 MiB per job with a tail
ring; draining child output never blocks because the spool is full. Readers must
learn when requested bytes have fallen out of retention, rather than interpreting
the surviving tail as a complete transcript. Byte pagination must tolerate
UTF-8 characters crossing a page boundary. UI decoding is not cursor arithmetic.

Terminal metadata is retained for seven days. Nonterminal records and pending
outbox events must not be pruned. Retention cannot break a pending event's
reference or its durable acceptance deduplication.

Default admission limits are four execution slots per root owner session/generation
and sixteen globally. Queued jobs do not consume slots. When slots are full,
accept into the durable queue if its one-hundred-job capacity permits; reject
with `resource_exhausted` when the queue is full. Independently cap all nonterminal
jobs, including parked agents, at 116. Rejected admissions create no job/outbox.

Starting/running/stopping work retains its slot until cleanup. An agent waiting
only for children/permission may park and release its slot only after confirming
that no inference is running. It retains its job, ownership and timeout; resuming
inference must reacquire a slot. Descendants count against the same root owner.
Claim slots atomically in FIFO order among eligible jobs, skipping owners at
capacity to avoid head-of-line blocking. Reconcile reservations after crash.
Admission checks and intent creation are atomic; accepted-request retries consume
neither another queue entry nor a slot. Tests must cover a parent waiting on a
child at the root quota so parked parents cannot deadlock the queue.

### 9. Provider migration and desktop contract

Claude must disable or block native `run_in_background` and background subagents
with a verified SDK option, hook or capability. A prompt-only prohibition does
not satisfy the contract. The current `canUseTool` callback and review hooks
are integration points, not evidence that every native background path is
blocked (including permissive modes and foreground-to-background transitions).
If the pinned SDK cannot enforce this, the provider lane must report the blocker
to the leader; migration is not complete.

Codex and Cursor call the common MCP tools. Existing Claude jobs during upgrade
are drained or stopped only through their legacy path until terminal. They do
not become new registry executions or emit duplicate completion wakes. New jobs
must use the registry. Legacy snapshots must remain distinguishable from registry
records during this transition; legacy provider loss is not adopted as proof of
new executor containment.

Every provider uses the same desktop schema: queued/starting/running/stopping,
all terminal states, timing, output, Stop, failure reason and completion delivery
`pending | delivered | suppressed`. The new API uses `state`; the existing
`background_tasks` projection retains `status` as a compatibility alias of the
same state, never a separately inferred lifecycle. Each job exposes
`completion: null` before terminal, then
`completion: {event_id, status, reason}`. Here `delivered` means a wake was
durably accepted/enqueued, not that inference finished. Completion delivery
changes belong to the outbox projection and do not change a terminal job state.
Offline clients may view their snapshot but
cannot mutate it. Transcript receipts name the task and its result. Switching
conversations must never mix owner generations or accept stale output replies.

The fixture includes small response examples for these exact projection fields.
They are partial job projections, not replacements for the complete record above;
example opaque cursor/event strings do not prescribe their encoding.

### 10. Ownership and implementation sequence

| Lane | Owned work |
| --- | --- |
| Runtime/storage (Developer 1) | Registry, revisions, intent/terminal/outbox transactions, quotas, spool, supervisor/lease, restart/reclamation |
| Providers/agents/MCP (Developer 2) | Authenticated tools, provider restrictions and legacy drain, completion dispatcher and durable turn acceptance/dedup |
| Model/QML (Developer 3) | Shared task projection, paging/output/Stop, offline behavior, owner fencing and transcript receipts |
| Shared coordination | SessionHost and IPC wiring, feature negotiation, docs, tests and CMake; coordinate dependent commits before consuming a new contract |

Do not add the advertised API until its handlers are registered. The old
`session.tasks/1` snapshot contract is not evidence of support for new starts or
durable lifecycle guarantees. This ADR does not change its advertised behavior.
No implementation is imported from another repository. No Lite `core/` change,
hosted-service dependency or new bundled dependency is required by this contract.

### 11. Verification gates

The fixture assigns stable scenario ids for retry/spec conflict, permission,
launch-boundary crashes, identity reuse, containment/descendants, lease expiry,
pre-launch permission/spawn/handshake failure and uncertain cleanup,
timeouts, stop/completion races, restart around terminal/outbox and turn acceptance,
busy/permission/decision/pause fences, stale generations, quota races, output
overflow, more than sixteen jobs, provider loss and Claude legacy drain/blocking.
Executors, providers and journals used by these tests must be fake or isolated;
no test consumes provider quota.

The contract fixture check validates design consistency only. Runtime, storage,
provider and UI implementations must add fault-injection tests at their real
boundaries. In particular a fixture mentioning containment or SDK blocking is
not evidence that either works. Independent review must check feasibility as
well as consistency before downstream implementation is accepted.

## Consequences

Jobs and their completion survive provider/UI disconnects without using a
provider query as the lifetime anchor. This requires a durable execution and
delivery boundary, platform containment evidence and a migration window for
Claude's old jobs. Unsupported execution platforms fail explicitly.

The previous query-liveness and worktree/approval assumptions remain valid for
legacy Claude jobs during drain (ADRs 0021, 0026 and 0027). Registry jobs instead
use immutable cwd and the wake fences above. Their implementation must update
those integration points without dropping legacy cleanup or inventing results.

## Alternatives considered

- Retain provider snapshots as authority: cannot guarantee recovery, full
  membership, stable identity or durable completion delivery.
- Let the team watchdog rerun commands: cannot distinguish an absent reply from
  a completed side effect and cannot own process containment.
- Keep a Claude query open for all providers: neither a provider-neutral API nor
  a durable executor boundary, and duplicates Claude's native completion path.
- Claim exactly-once execution/inference: a crash can leave an already executed
  side effect or model request with an unknown result. Durable deduplication
  prevents duplicate acceptance, not that fundamental ambiguity.
