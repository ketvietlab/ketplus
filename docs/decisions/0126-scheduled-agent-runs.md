# 0126: The daemon owns scheduled agent runs

Status: accepted
Date: 2026-11-13

## Context

A schedule sends a prompt to an agent at a time or on a recurrence, including while
the desktop window is closed. The desktop and, later, agents need one API for that.
The approved mock (pull request #681, `mockups/qt/evidence/schedules/`) creates a
schedule only from New conversation and New team. The Schedules window edits and
manages; it does not create.

0124 and 0125 were already accepted, so this decision is 0126.

## Decision

1. **The daemon owns scheduling.** It is the only process that stores a schedule,
   decides the next fire time and delivers the prompt. Desktop and agents talk to
   it through the `schedules/1` feature. That name is not advertised in
   `daemon.hello` until the handlers that answer `schedule.*` are registered;
   advertising it earlier would make every request `unsupported`. Phone and cloud
   sync are out of scope for v1: nothing in this feature is projected to the relay.

2. **Two records.** Field names on the wire are snake_case.

   - `Schedule`: `id`, `name`, `enabled`, `prompt` (text only; attachments are out
     of scope), `target`, `rule`, `busy_policy` (`queue` | `skip`), `missed_policy`
     (`run_once` | `skip`), `created_by` (`user` or an agent conversation id),
     `created_at`, `updated_at`, `next_run_at`, `last_run_at`, and `status` when it
     is `target_deleted`. `status` is omitted while empty. It is server-owned: a
     client that sends it is `unknown_field`.
   - `ScheduleRun`: `id`, `schedule_id`, `scheduled_for`, `started_at`, `finished_at`,
     `status` (`queued` | `running` | `succeeded` | `failed` | `skipped` | `missed`),
     `session_id`, `turn_id` (the conversation turn, when the run has one),
     `error_code`, `error_message`.

   The C++ types and their JSON live in `modules/remote/src/Schedule.h`
   (`ketplus::schedule`). The daemon and the desktop both link `ketplus_remote`
   and include `Schedule.h`. The last 50 runs of a schedule are kept
   (`maximumRetainedRuns`).

3. **Four targets.**

   - `{kind: "conversation", session_id}` — the prompt goes into that conversation.
   - `{kind: "new", workspace, provider, model, effort, isolated_worktree, title_template}`
     — each run creates a conversation. `workspace` is the repository path.
     Empty provider, model or effort means the same defaults as a normal new
     conversation. `isolated_worktree` is that conversation's execution worktree.
   - `{kind: "team", team_id}` — the prompt is a work order for the team's leader.
     Any recurrence rule is allowed.
   - `{kind: "team_start", team_id}` — starts the team. No prompt, the rule's
     `kind` is `once`, and `missed_policy` is `run_once`.

   `schedule.list` filters by `target_kind`, `session_id` and `team_id`. The
   filters combine. `session_id` selects conversation targets; `team_id` selects
   `team` and `team_start` targets.

4. **The rule is an opaque JSON object string.** The recurrence engine owns the
   schema. v1 kinds are `once` (a local date-time), `daily` and `weekdays` (a
   local time), `weekly` (weekdays and a local time) and `interval` (every N
   hours, 1-24, from an anchor). Every rule carries an IANA timezone. This
   protocol checks that `rule` is a JSON object string, and that a team start's
   object has `"kind": "once"`. The engine rejects a rule it cannot schedule.

   Daylight saving time, implemented by the engine: a local time skipped by the
   spring-forward gap fires at the first valid instant after the gap. A local
   time repeated by the fall-back overlap fires once, on the first occurrence.

5. **Busy and missed.** `queue` waits when the target conversation is already in
   a turn; `skip` drops that fire. `run_once` catches up a single fire the daemon
   missed while it was off or asleep; `skip` drops missed fires. A team start
   always uses `run_once`.

6. **Requests and events.** Payloads are strict: an unknown field, a missing
   target, an empty prompt (except a team start) and an unknown enum value fail
   with a stable code.

   | Message | Payload |
   | --- | --- |
   | `schedule.list` | optional `target_kind`, `session_id`, `team_id` |
   | `schedule.get` | `id` |
   | `schedule.create` | `name`, `enabled`, `prompt`, `target`, `rule`, `busy_policy`, `missed_policy` |
   | `schedule.update` | `id` and at least one of those writable fields |
   | `schedule.delete` | `id` |
   | `schedule.setEnabled` | `id`, `enabled` |
   | `schedule.runNow` | `id` |
   | `schedule.runs` | `id`, optional `limit` (1-50, default 50), optional `before_id` |
   | `schedule.changed` | `change` (`created` \| `updated` \| `deleted`) and the `schedule` |
   | `schedule.run` | `run` |

   `id`, `created_by`, `created_at`, `updated_at`, `next_run_at` and `last_run_at`
   are server-owned. The handler sets `created_by` from the connection: `user`
   for a person, or the agent conversation id. A client cannot claim either.
   A create that sends any of these fields is `unknown_field`. Every stored
   timestamp is ISO-8601 with `Z` or a numeric offset; a value with no zone is
   `invalid_payload`, so two processes cannot read it as different local times.
   An update is a patch. After the patch is merged, the stored document is parsed
   again with `scheduleFromJson`, so a team start cannot be left with a prompt, a
   repeating rule or `missed_policy` `skip`. Clearing a prompt in a patch requires
   the `team_start` target in the same request.

   Success payloads include `ok: true`. List returns `schedules`. Get, create,
   update and setEnabled return `schedule`. Delete returns `id`. Runs returns
   `runs` (newest first) and `has_more`. Run now returns `run`. It does not answer
   `not_implemented`; that code stays in the header for clients that still branch
   on it. A missing schedule is `schedule_not_found`.

   Stable codes: `unknown_field`, `empty_prompt`, `missing_target`, `invalid_enum`,
   `invalid_payload`, `invalid_rule`, `prompt_not_allowed`, `team_start_constraint`,
   `schedule_not_found`, `target_deleted`, `target_not_found`, `not_implemented`.
   `target_deleted` refuses to enable a schedule whose conversation was deleted.
   `target_not_found` refuses a create or update whose conversation is not in the
   journal, or whose team id is not that team's decimal id.

7. **Who may create one.** The product creates a schedule only from New
   conversation and New team (mock pull request #681). Agent-created schedules
   are the documents whose server-assigned `created_by` is a conversation id.
   Their limits are pending decision D-2 and are not part of this contract.

8. **Journal.** A following change stores the documents above: deleting a schedule
   deletes its runs; deleting a team deletes its `team` and `team_start`
   schedules and emits `schedule.changed` (`deleted`) for each; deleting a
   conversation disables schedules that target it, records `status`
   `target_deleted`, and emits `schedule.changed` (`updated`) for each. Existing
   journals open unchanged. This decision does not add those tables.

## Consequences

- Desktop and the daemon share one JSON document, so a later screen does not
  invent a second shape.
- The recurrence engine can change DST and timezone behaviour without a protocol
  revision, as long as `rule` stays a JSON object string with the v1 kinds.
- A client that does not know `schedules/1` ignores the feature. This daemon
  advertises it on minor 1 now that the handlers are registered. Older daemons never do.
- Run now is handled by the daemon runner and returns the run. It does not answer
  `not_implemented`.

## Alternatives considered

- **Scheduling in the desktop.** A closed window would miss every fire. The
  daemon is already the always-on process.
- **A JSON object for `rule` instead of a string.** The engine owns the bytes.
  A string keeps that schema out of this parser, apart from the team-start
  `once` check.
- **Camel case, matching the task sketch.** The daemon protocol is snake_case
  (`session_id`, `error_code`). The sketch's names are the same fields.
