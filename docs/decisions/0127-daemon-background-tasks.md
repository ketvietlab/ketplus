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
alone must not force every native job into a waiting state.

Cleanup errors, known surviving processes, lost identity/lease or an unreconciled
restart are the daemon's to settle, never the user's: see "Daemon-owned cleanup"
below (2026-11-25). The earlier design parked such a job in a nonterminal
`needs_attention` state for a user acknowledgement; that state, the acknowledgement
command and its UI are retired. Cleanup retries signal only identities that can still
be validated and never replay the command.

A job that never committed its handshake never launched a workload, because the daemon
sends start only after that commit. When such a job's supervisor left neither a receipt
nor a control socket (it exited before its first durable write), the daemon ends the job
as `failed(reason=handshake_failed, error=executor_never_started)` with
`cleanup_status=not_required` and releases its slot. The
supervisor persists its receipt before anything else that can fail, and a failure before
its control socket listens is recorded as `failed/handshake_failed`. A job that was already
`stopping` when this is detected ends at its `terminal_target` instead (cancelled, timed out
or interrupted), with the same evidence. A missing receipt after a committed handshake is
settled by the daemon like any other unconfirmed cleanup, and a client that has seen a
receipt keeps retrying the identities it holds. A supervisor slower than the daemon's 5.5-second recovery window to write its first
receipt (for example the first launch of a newly installed bundle while the system scans
it) is reported the same way; it launched nothing and exits once its lease expires, so the
`handshake_failed` result does not mean the executor or handshake is broken.

### 4a. Daemon-owned cleanup

Amendment, 2026-11-25. The user asked that cleanup stop being a concept they meet at all:
no warning, button, dialog or "cleanup needs attention" wording. Cleanup is the daemon's
job and the daemon finishes it.

**Decision.**

1. A job always ends at the state its command really reached (`completed`, `failed`,
   `cancelled`, `timed_out`, `interrupted`). The daemon never emits `needs_attention`,
   `reason=cleanup_unverified` or `cleanup_acknowledged`. `session.task.resolve_cleanup`
   and the `manual_guidance` field are removed from the public protocol and from the agent
   transport; the daemon answers the command as unsupported. A client older than this
   change still reads every record, because `state`, `cleanup_status` and the fields it
   used are unchanged; it simply never sees the retired state again.
2. When an executor result reports processes not yet confirmed gone (cleanup error, known
   survivor, lost identity or lease, unreconciled restart), the job moves to `stopping`
   with `terminal_target=interrupted` and the daemon keeps working for a bounded window of
   30 seconds: the executor client probes the job's process group and signals again every
   second, SIGTERM first and SIGKILL after 1.5 seconds, only to identities it re-verifies
   by pid and birth time (never a bare stored PID, never a reused PID). A later result
   that verifies the group empty ends the job normally with
   `cleanup_verification=process_group_empty`.
3. When the window elapses, or when the executor can no longer be reached at all (receipt
   missing, executor identity invalid, reconnect failed, result not committable), the
   daemon ends the job at its target state with `cleanup_status=best_effort_complete`, logs a warning and records the
   unconfirmed issue in `managed_job_diagnostics(task_id, revision, issue, recorded_at)`.
   The target state follows the real outcome: when the executor reported that the command
   ran to an exit (`completed` for exit 0, `failed` otherwise), that state is kept and the
   unconfirmed cleanup is recorded in the diagnostics only. The job is `interrupted` only
   when no real result exists (the executor was lost before reporting one), and a stop
   already in progress keeps the state it named (`cancelled`, `timed_out`).
   That table and the log are the internal diagnostics (daemon log, `ketplusctl`); the
   task record, the notices, the agent tools and the UI carry no cleanup wording.
4. **Slot and resource policy.** The terminal transition releases the execution slot and
   the job stops counting against the nonterminal cap, exactly like any ended job. A
   process the daemon could not identify may outlive its job; that residual is bounded
   per job (one diagnostics row, no retained record, no timer, no retry loop after the
   job ended) and is never signalled: the daemon kills only identities it verified, so a
   process of another user, a reused PID or an unrelated process is never touched. The
   assurance is not raised: native records stay `native_best_effort` and are never
   `cgroup_v2` or `verified` without process-group or kernel evidence.
5. A restart re-runs the same path: recovery reconnects to each nonterminal job, repeats
   the probe and signals for another window, and ends the job the same way. The window is
   per daemon start, so a job cannot keep a slot across restarts.
6. **Migration.** When the registry opens, every record an earlier daemon left in
   `needs_attention` ends as `interrupted` (`reason=daemon_upgrade`,
   `cleanup_status=best_effort_complete`), releases its slot, queues a suppressed
   completion (no wake for the migration), drops `manual_guidance`, and keeps its former
   issue as a `migrated_` diagnostics row. Its remote job record, if any, follows. Pending
   attention notices are superseded. The step is idempotent.
7. A remote job whose runner stays away for the 20-minute window, whose runner is removed,
   or whose result lacks cleanup confirmation, ends the same way: the runner's own result
   decides the state (`interrupted/runner_unreachable` or `runner_removed` without one),
   the missing confirmation is a diagnostic. The daemon cannot signal a remote machine, so
   there is no retry for it.

**Internal vocabulary kept.** The executor's own receipt file and its client still call the
unconfirmed result `needs_attention` with `reason=cleanup_unverified`. That is the private
protocol between the daemon and its supervisor process, never stored in the registry and
never sent to a client; the runtime translates it as above. The runner's result vocabulary
is unchanged (`cleanup_unverified` is its own diagnostic code).

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
suspend the event loop that sends leases to other healthy executors. The Linux
lease remains five seconds. Admission commits only after the capability result
and rechecks generation/quota/idempotency at that point; internal synchronous Host
start dispatch is rejected before beginning admission.

Linux records use `containment_mode=cgroup_v2`. `cleanup_status=verified` requires
an identity-matching domain with `populated=0`, or verified service reclamation/
host reboot during recovery. Otherwise the record stays `stopping`,
`cleanup_pending=true`, and retains its slot. This boundary contains the workload's
own migration authority; it is not a sandbox against an independently acting
same-UID host process or requests deliberately made to external services.

The macOS native lease is 15 seconds. Lease expiry accounted for 39 of 67
unverified-cleanup receipts (58%) in one local sample. The longer lease tolerates
short heartbeat gaps above five seconds and can delay detection of a genuinely
lost heartbeat by up to ten additional seconds. This is a temporary measure
while the cause of delayed heartbeats is investigated; the target remains five
seconds on every platform. Executor receipts record
the largest observed heartbeat gap in milliseconds when it exceeds two seconds.

#### Native cleanup is verified by the OS, not guessed (macOS)

Every native job runs in its own session and process group (`setsid`). The
supervisor records the root's PID, process group and process birth time before the
root executes; the birth time keeps a reused PID from being mistaken for the job.

Losing the daemon is a reason to stop the job, not evidence of a leak. When the
heartbeat lapses, the control channel closes, or the supervisor itself is lost, the
job is stopped and the OS is asked whether any live (non-zombie) process remains in
the recorded group (`proc_listpgrppids`) and whether every recorded incarnation is
gone. The answer decides the outcome:

- Group empty and every recorded identity gone: cleanup is **verified**
  (`cleanup_status=verified`, `cleanup_verification=process_group_empty`). The job
  ends `interrupted` with no blocked slot; the daemon only logs it. This is process-group evidence, not the
  kernel containment of Linux (`fullScopeVerified` stays false).
- A live process remains, the group or an identity cannot be listed or inspected
  completely (another UID, truncated listing), the recorded PID now has a different
  birth time, or the receipt is missing: the daemon retries for its window and then ends
  the job with a diagnostic (§4a). A reused PID is never signalled.

The ledger accepts this result by evidence kind, not by trusting the status word. A
native result carrying `cleanup_status=verified` is committed only with
`cleanup_verification=process_group_empty` (`groupVerified`) or kernel verification;
`verified` alone on a native job is still refused (`invalid_cleanup_evidence`). The task
record keeps `cleanup_verification=process_group_empty` and a native `containment_mode`,
so the weaker assurance is never raised to `cgroup_v2`. A result that could not be
verified is retried, then ended with a diagnostic (§4a).

The supervisor records who stopped it. Its SIGTERM/SIGINT handler (`sigaction`,
`SA_SIGINFO`) only stores the signal number and the kernel-reported sender PID and UID
in async-signal-safe atomics. The first signal wins by compare-exchange; `sa_mask` is
per thread, so it is not relied on to serialise handlers. The main loop adds
`stop_signal`, `stop_sender_pid`, `stop_sender_uid` and, when the sender can be
inspected, `stop_sender_birth` to the receipt and logs them next to
`stop_cause=supervisor_termination_requested`.

`process_identity_lost` means the recorded process may still be alive beyond our
control. The OS reporting the PID gone, or the PID held by another process with a
different birth time (a reused PID), proves the original exited and raises no alarm.
An unchanged birth, or a PID that cannot be read but still answers `kill(pid, 0)`,
is retried within the window and then ends the job with a diagnostic (§4a).

On macOS the probe reads the PID through libproc. For another user's process libproc
either refuses the read (the probe reports unknown and falls back to `kill(pid, 0)`) or
returns it with a different owner (the probe reports foreign, with its birth). The
probe test accepts either result. This change does not narrow that case; the daemon's
own retry of it is a separate follow-up.

The record keeps `cleanup_verification` only while its `cleanup_status` is `verified`;
any other status removes it.

A process that called `setsid` leaves the group, so the group alone cannot see it.
The supervisor therefore keeps tracking descendants by parentage, signals every
recorded incarnation, and verifies the group only after they are gone. A child that
detached before it was ever observed is not detectable and remains a best-effort
limit, as documented above.

Detection uses the closed channel. The supervisor holds the control connection; EOF/
HUP on either end is noticed immediately. The supervisor never stops the job
because the channel closed: a restarted or upgraded daemon reconnects to the live
executor, and only the lease bounds that wait (a stop after a closed channel
records `stop_cause=lease_expired_after_channel_closed`). The daemon starts
recovery at once when it sees EOF and the supervisor process is gone, then waits
`NativeJobRecoverySettleMs` (3.5 s) for signalled processes to exit before it
declares a loss unverified. The heartbeat and its lease (above) remain
the fallback for a hung peer, whose connection stays open but silent
(`stop_cause=lease_expired`). The Linux executor shares the channel detection; its
cleanup evidence is still the cgroup.

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
and continue reconciliation and warnings. macOS follows the native amendment's bounded
retry and then ends the job with a diagnostic (§4a) when restart cannot be reconciled.
Never relaunch a command with
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

Only terminal transitions create completion wakes; progress creates none. The owner
notices of §7a are separate events with their own rows, not completions.
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
| Team paused or automatic turn budget exhausted | Keep pending and record one UI notice per event ID with the reason; Resume releases eligible wakes |
| Closed/deleted owner or replaced generation | Suppress with a recorded reason |
| Job cancelled by Stop or session close | Suppress; cancellation must not restart the agent |
| Approved job still queued at its notice threshold | One owner notice naming why (§7a) |
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

### 7a. Owner notices of nonterminal jobs

Amendment, 2026-11-23. Agents guessed how long to wait for jobs. Only terminal states
woke the owner, so a job that sat queued behind its root's full slots told it nothing
until the job's timeout, if ever. Team members also
set `team_wait` timers for builds that a managed job would have reported by itself.

**Decision.** A nonterminal job queues an owner notice when it waits in the queue, at most
once (the `needs_attention` notice of the first design is retired, §4a):

| Notice | When | Event identity |
| --- | --- | --- |
| `queued` | The job is still `queued`, its permission approved, `queuedNoticeAfterMs(timeout)` after its creation: half its timeout, at most two minutes | `(task_id, "queued")` |

The queued notice names `queue_reason`: `root_slot_limit` with `slot_limit` and the
`slot_holders` (task id, title, state) of its root, `waiting_for_runner` for a job that
runs on a runner, or `awaiting_admission` otherwise. A job waiting for its permission
request gets none: the request is the user's and the UI shows it.

**Mechanism.** Notices are rows of `managed_job_notices`, written in the transition's
transaction or by the runtime's pump through one idempotent ledger call (queued). `INSERT OR IGNORE` on the event identity makes a repeated write harmless,
including after a restart. They are not added to the completion outbox: that table keeps
one event per task, which its `completion` projection, retention and child-continuation
checks rely on. From acceptance on, a notice follows the completion path unchanged: the
same durable acceptance into `managed_job_wakes`, owner generation and Stop/pause/decision
fences, idle dispatch, and the logical `turn_id=background-wake:<event_id>`. A notice is
stale once its job has moved on (started or ended): dispatch suppresses it
with `superseded`, and a terminal transition suppresses the job's pending notices and
their accepted wakes in its own transaction, so retention is never held by them. The
wake's origin kind is `background_task_notice`. Outbox delivery to wake acceptance
can retry at least once under the same event ID. The unique durable wake ID deduplicates
acceptance across restart. Once dispatch has been claimed, recovery interrupts an
uncertain dispatch rather than risking a second provider turn.

A job's wake leaves a `team_wait` the member set: the wait names something KetPlus does
not track, such as a deployment, and nothing ties it to the job. The team watchdog counts
a member that owns a queued, starting, running or stopping job as at work, so ending a
turn on a job is a valid way to wait. While only such jobs hold the team, the watchdog
looks again each stall period: a job the user stops ends as cancelled and wakes no one,
and no turn's end would start the watch again. Agent and team instructions say to
start long builds and tests as jobs and end the turn rather than guess minutes;
`team_wait` stays for what KetPlus does not track, such as a deployment or another
conversation's answer.

**Not covered.** A running job that makes no progress gets no notice. The daemon has no
reliable progress signal (output growth is not one), so the job's own timeout remains its
bound.

**Alternatives.** Reusing the completion outbox for notices needs a table rebuild to drop
its one-row-per-task constraint and changes every reader of that projection. A periodic
"still running" heartbeat would bring back the guessing this removes. Ending every
`team_wait` on a job's wake would save a check turn when the member set the wait for that
job, but it drops a reminder set for unrelated work; `team_wait` with a background task id
could tie a wait to one job if the extra turn turns out to matter.

### 8. Queries, output, retention and admission limits

List/get read the complete registry and paginate collections. List returns
`{tasks, next_cursor, revision, active_count}`. Cursors are opaque; snapshot
revision is used for gap detection/refetch. A 16-row UI
projection does not determine whether a job exists, is active, is eligible for
cleanup or may be stopped. Snapshots include `revision` and `active_count`.
`tasks.changed` snapshots and every session catalog row (`background_task_summary`)
also carry an owner summary over the whole registry:
`{live_jobs, queued_jobs, job_title}`, just `{live_jobs: 0}` when nothing is live. It
counts the owner's queued through stopping jobs, except a job stopping toward
`cancelled`, whose completion wake is suppressed, and names one job. Clients show whether an ended turn still waits on a job from this
summary, never from the bounded rows.
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

Default admission limits are four execution slots per root owner session/generation.
There is no machine-wide slot limit: an earlier global limit of sixteen let attention
records that held their slots stop every new job on the machine while each waited for a
user. Queued jobs do not consume slots. When slots are full,
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
Codex, Cursor and other ACP adapters. The bound conversation token preserves
discovery/read scope; it must not authorize start/stop by itself. Mutation needs
a separate origin capability bound to session, generation, turn, policy and
transport epoch, supplied by the host through MCP bootstrap. Old invocation
credentials are immutable, revoked at turn end, and never rebound to current
policy. Review turns receive no mutation capability. New authority requires a
verified old-context drain and replacement-context acknowledgement. See the
[provider refresh evidence and remaining gaps](0127-provider-origin-refresh-evidence.md).
The MCP layer forwards the exact execution spec/request key and opaque cursors;
the registry owns deduplication, authorization, output and completion delivery.
It exposes no cleanup operation to an agent: there is none (§4a). A new schema
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
deletion; agent and remote credentials do not gain this administration grant.

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
