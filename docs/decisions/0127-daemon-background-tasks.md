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

Signalling binds a kernel target before validating its recorded birth: a pidfd
on Linux, or a task audit token on macOS. The macOS token is obtained through a
task-name port, validated against the recorded PID/birth, then passed to
`proc_signal_with_audittoken`; the kernel checks the PID version when signalling.
Failure to obtain that identity fails closed. Group enumeration supplies discovery
evidence only; a check followed by a PID/process-group `kill` is not sufficient.
See Apple's [libproc wrapper](https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/libproc/libproc.c)
and [kernel implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/proc_info.c).

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

Linux requires a delegated cgroup v2 **domain** per job, `cgroup.kill`, and
`cgroup.events`/`populated`. Persist boot/subtree identity and contain the trusted
launcher before any workload exec; the supervisor stays outside the subtree.
Reclaim with `cgroup.kill` and confirm `populated=0` before terminal commit.
Delegation to the workload's own UID alone does not prevent migration. A
supported deployment must also supply tested unit/namespace isolation that
prevents migration outside the job boundary. Missing capabilities return
`containment_unavailable` before accepted, with no PGID fallback. The daemon
must not install privilege changes, mount cgroups or change system ownership to
make admission succeed. This does not claim to contain deliberate requests to
external services. See the [kernel cgroup v2 documentation](https://docs.kernel.org/admin-guide/cgroup-v2.html)
for delegation, migration and populated semantics.

The reference Linux path uses the non-root daemon user's existing systemd user
manager. Each supervisor is MainPID of a separate transient user service with
`Delegate=yes`, `KillMode=control-group`, `TimeoutStopSec=3s`, and `Restart=no`.
The daemon commits the deterministic unit/lease and boot identity before requesting
the service. The supervisor creates a fresh `job` domain, persists its boot/device/
inode and unit invocation identity, and places a trusted blocked launcher there
before releasing its durable pre-exec gate. The supervisor itself stays outside
that domain; the user manager reclaims the complete service on supervisor death.

The trusted launcher enters user, mount, PID and cgroup namespaces, makes mounts
private, mounts namespace-local proc/cgroup views, and closes every descriptor
above stdio before applying the workload's environment or executing its program.
This prevents a workload environment such as `LD_PRELOAD` from running before
isolation and prevents inherited migration descriptors or namespace handles from
crossing exec. The namespace's UID 0 maps only to the existing daemon UID.
The daemon's inherited environment travels only in the authenticated, bounded
private launch channel; it is applied with the explicit delta after isolation.
It is not stored in the service unit, receipt, registry, or UI. Thus a user
service's different default environment does not silently replace the daemon's
environment. Oversized private launch data fails before a supervisor is spawned.
`nsdelegate` and successful namespace setup are required by an admission probe;
an unavailable user bus or denied namespace policy returns
`containment_unavailable`, without creating a job. Administrators supply these
capabilities; neither the daemon nor installer lowers host namespace policy.

Daemon-side admission and user-manager RPCs are asynchronous, including supervisor
start, receipt certification and recovery queries. A stalled manager must not
suspend the event loop that sends leases to other healthy executors. The existing
five-second lease is unchanged. Admission commits only after the capability result
and rechecks generation/quota/idempotency at that point; internal synchronous Host
start dispatch is rejected before beginning admission.

Linux records use `containment_mode=cgroup_v2`. `cleanup_status=verified` requires
an identity-matching domain with `populated=0`, or verified service reclamation/
host reboot during recovery. Otherwise the record stays `stopping`,
`cleanup_pending=true`, and retains its slot. The native macOS acknowledgement
action does not resolve this Linux state. This boundary contains the workload's
own migration authority; it is not a sandbox against an independently acting
same-UID host process or requests deliberately made to external services.

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

Terminal metadata and output are retained for seven days. Prune only settled
terminal records: retain pending outbox, accepted/dispatching wake and live
ownership references. Atomically replace pruned metadata with a tombstone
`{owner_session_id, owner_generation, client_request_id, spec_hash, task_id,
expired_at}` and the accepted defaults snapshot needed to normalize retries.
Same-key/same-spec retries return `ok=false`, `error_code=task_expired`, the
original `task_id` and `expired=true`; changed specs return
`idempotency_conflict`. Neither path launches execution. Canonical hashing
resolves omitted fields against the original defaults, including after owner
configuration changes. Retain the requested-to-resolved cwd mapping so a retry
does not follow a later symlink retarget or deletion. Tombstones have no independent TTL while that generation
can accept requests; GC is permitted only with permanent generation revocation.
Keep nonterminal records and pending outbox events. Retention cannot break a pending event's
reference or its durable acceptance deduplication. Keep the compact child link
as an ownership fence after parent expiry, so an expired worker cannot become a
new quota root simply because its metadata aged out.

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

The shared `ketplus_agents` MCP server exposes the five tools above to Claude,
Codex, Cursor and other ACP adapters using the already bound conversation token.
The MCP layer forwards the exact execution spec/request key and opaque cursors;
the registry owns deduplication, authorization, output and completion delivery.
It never exposes `session.task.resolve_cleanup` as an agent tool. A new schema
does not grant execution or replace the existing daemon permission broker.

Claude SDK 0.3.322 is configured through `background-policy.mjs`: both the child
environment and the higher-priority SDK `settings.env` overlay pin
`CLAUDE_CODE_DISABLE_BACKGROUND_TASKS=1` and
`CLAUDE_CODE_DISABLE_MCP_TASK_BACKGROUND=1`. Environment alone was insufficient:
an offline test demonstrated that project settings could otherwise turn the
first flag off. The enforced settings preserve credential configuration and
review cache variables. Native `Agent`, its legacy `Task` spelling and `Workflow`
are removed with `disallowedTools`; native delegation, including foreground and
resume/remote variants, uses a managed child instead. A fail-blocking
`PreToolUse` hook denies those entries and any `run_in_background` request;
the same denial runs before `canUseTool`'s permissive-mode exemptions. Other
hooks compose with this guard, and a neutral guard result grants no permission.

This delegation restriction is specific to native entries that can own agent
work, not all foreground tools. In the pinned SDK's `sdk-tools.d.ts`, Agent's
`run_in_background` defaults to true. Its `sdk.d.ts` task-start contract says a
resumed subagent is always registered in the background, and its background
control supports moving foreground agents into the background. Workflow accepts
scripts using `agent()/parallel()/pipeline()` and a `resumeFromRunId`; there is
no separately verified, non-delegating foreground-only Workflow entry here.
Normal foreground Bash, Read, Edit and other permission-checked tools keep their
existing behavior. The leader-approved migration routes the entire native
Agent/Task/Workflow entry to managed children rather than leaving those paths
with query-owned lifetime.

The equivalent managed request is `kind=agent` with
`execution_spec={prompt, provider?, model?, effort?, cwd?}`. Native prompt,
model and effort intent map to those fields and cwd stays within the owner's
workspace. This is **not** full native API equivalence: `subagent_type`, forked
context, remote/native isolation, named-agent messaging and Workflow script/result
caches have no one-to-one mapping. Explain the intended work in the managed
child's prompt; do not silently discard those distinctions. Where appropriate,
decompose a workflow into independent managed children and use their terminal
results for dependent steps, rather than pretending to resume a native script
cache. Existing authorized managed-child creation, child approvals and ownership
remain T-2's implementation; the MCP adapter forwards their typed spec unchanged.

The SDK's background-disable flag covers its automatic foreground-to-background
path, while the host hook independently blocks explicit requests. Offline
fixtures verify the hook/callback across normal, full-access and review launches,
including resumed queries. The opt-in real-SDK test verifies control rejection
and hook failure support with adversarial project settings, without yielding any
prompt. It does not claim a live model exercised every Bash timeout or subagent
variant. The policy is tied to the pinned SDK; an upgrade must reverify it.

Codex and Cursor call the common MCP tools. Existing Claude jobs during upgrade
are drained or stopped only through their legacy path until terminal. They do
not become new registry executions or emit duplicate completion wakes. New jobs
must use the registry. Legacy snapshots must remain distinguishable from registry
records during this transition; legacy provider loss is not adopted as proof of
new executor containment.

The old sidecar task collector and SDK stop/output controls remain only for
legacy drain. They do not call registry start or the durable completion outbox.
New managed completion goes through T-2's transactional owner-wake boundary,
independent of a kept-open Claude query. The team watchdog remains a stall
detector/recheck mechanism; it never interprets process output, infers a job's
terminal state or creates a replacement execution. Foreground Stop and pending
permissions retain T-2's existing wake fences.

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

## Implementation checkpoint for T-2

The native pre-exec barrier records the forked root birth identity durably before
releasing it to exec; gate EOF aborts a child whose supervisor died before release.
This also covers commands that exit before Qt emits its started notification.

The storage/runtime implementation now includes the transactional registry,
permission-gated native macOS supervisor, owner-bound IPC operations, independent
managed child conversations, durable completion acceptance and wake dispatch.
`background_tasks` remains a 16-row compatibility projection; its active records
come first, while list/get/output/stop use the complete registry. Authenticated
local administration can inspect a retained owner/generation after conversation
deletion and acknowledge cleanup with revision/audit checks; agent and remote
credentials do not gain this administration grant.

An accepted wake contains a durable logical `turn_id=background-wake:<event_id>`.
A live dispatcher may defer a claimed wake only before invoking a provider.
Recovered `dispatching` records become interrupted and are never replayed;
`accepted` records remain eligible after restart. Managed child terminal commit
waits for descendant jobs and their accepted/pending completion continuations.
A parked child must reacquire a quota slot before another inference starts.
When reacquisition succeeds, its accepted wake is claimed and dispatched in the
same poll; an idle-child poll cannot park that slot again before dispatch.

Native macOS executor and fake-provider Host/IPC tests are runtime evidence;
provider migration and shared UI acceptance remain separate gates. Linux process
admission probes the reference user-service/namespace capabilities and fails
closed when they are missing. On the Ubuntu 24.04 arm64 reference guest, the
production helper, registry, fake-provider Host, IPC and restart acceptance suites
pass together; Runtime-component staging also passes runtime discovery and a
capability probe executed by the staged helper. See the dated verification record
in [testing](../testing.md#background-job-runtime-verification-record).
This evidence does not establish support on every Linux distribution or replace
the required deployment capability probe. The separate
[T-7 evidence](0127-linux-containment-evidence.md) establishes environment
primitives, not executor acceptance. Its guest-only namespace-policy bootstrap
is not a product installer step or a product VM dependency.
