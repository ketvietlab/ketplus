# ADR 0116: A paired phone reads and changes teams

Status: accepted (2026-11-12); the daemon side is implemented, the phone app is pending. Extends ADR 0114 and ADR 0115. Uses ADR 0025 (remote
transport), ADR 0113 (sealed session metadata).

## Context

ADR 0114 serves teams only to trusted local clients and says: "The phone does not show
teams yet; the cloud projection is unchanged". The phone app's native mock already draws
teams. Home shows each repository as a tree, with its teams and the members under them.
The team page shows the goal, the settings, Waiting for you, tasks, members and
Updates. The phone can also answer a decision, pause, resume, delete a team, create one,
add a developer, remove a member, add a conversation to a team, and hand a task over.

Three parts have to be built for this: the daemon, the relay and the phone's core. This
record fixes the protocol, so the three can be built at the same time.

A paired phone reaches the Mac only through the relay. The relay can read only what
ADR 0025 leaves in plaintext. A command is either sealed to a conversation's session key
or, when no conversation exists yet, sealed to the Mac (`session.create`,
`worktree.branches.read`). A team spans several conversations and has no session key
of its own.

## Decision

### 1. Capability `teams.remote/1`, advertised by all three sides

The name follows the capability rules of `docs/remote-transport.md`:

- **Daemon.** It lists `teams.remote/1` in the heartbeat's `capabilities` once it serves
  the commands of §4 and writes the `team` metadata of §3.
- **Relay.** It offers the name and enables it for a machine only when the host
  advertises it. The relay's answer lists it in `Compatibility.capabilities`, and the
  machine record lists it in `Machine.capabilities`. Unlike `events.forward_only/1`,
  every device decides for itself: a device that lacks the name blocks nobody else.
- **Device.** It lists the name on its device record
  (`PUT /v2/mobile/device/capabilities`). The relay accepts team commands only from a
  device that lists it.

**When any side lacks it**, the phone shows no team UI at all. There is no team branch
in Home, no team page, no team actions, no role under the conversation header, and no
"Conversations (n)" label. The members show as ordinary conversations of their
repository, each with its title and number. A team message in a member's transcript
(`assignment`, `digest`, `decision`) stays plain text, as ADR 0114 already says.

The phone checks the name on the machine record, not on the session metadata. A daemon
may write the `team` metadata while the relay does not yet offer the feature, and the
phone ignores that metadata until the machine enables the name. The relay refuses a
team command for a machine without the name with `teams_remote_unavailable`. A daemon
that does not know a team kind answers `unknown_operation`, as it does today.

### 2. Transport: one command, sealed to the Mac, answered to the device

Every team read and every team change is a durable mailbox command, as ADR 0025
describes them.

- **Command kind.** The kind is the name of the daemon's local `team.*` request
  (ADR 0114 §Protocol). The cloud layer forwards the decrypted payload to that request
  with actor `user`. The phone never writes a local path: the daemon resolves an opaque
  `repositoryId` through its worktree catalog, as it does for `session.create`.
- **Sealing.** A team command has no conversation: `sessionId` is `""` and
  `sessionGeneration` is `0`. The device seals the content to the Mac's agreement key,
  as for `session.create`. The content's `keyId` is `team-command:<command id>`. The
  associated data adds, in this order: `ownerSubjectId`, `repositoryId` (`""` when
  absent), `expectedCatalogRevision` (`-1` when absent), `recipientDeviceId`,
  `recipientKeyVersion` and `ephemeralPublicKey`.
- **Result.** The command's `recipientDeviceId` and `recipientKeyVersion` name the Mac.
  The daemon seals the result back to the device that sent the command: the content's
  `senderDeviceId` and `senderKeyVersion`, with a fresh ephemeral key, as it seals a
  `session.create` result. The result content has:
  - `keyId` `team-result:<command id>` and `keyEpoch` 1;
  - `senderDeviceId` and `senderKeyVersion`: the Mac's;
  - `recipientDeviceId` and `recipientKeyVersion`: the requesting device's;
  - its own `ephemeralPublicKey`.

  Its associated data is the command-result associated data of ADR 0025 (`sessionId`
  `""`, `sessionGeneration` `0`), followed in this order by `ownerSubjectId`,
  `recipientDeviceId` and `recipientKeyVersion`. No other device and not the relay can
  read it. The fixture holds a vector for each direction.
- **Route.** The phone posts to `POST /v2/mobile/machines/{machineId}/team-commands` and
  reads the result at `GET /v2/mobile/commands/{commandId}`. The daemon claims team
  commands with `/v2/commands:poll`, like every other command.
- **Mailbox only in this version.** Team commands do not go over the WebRTC peer
  connection, because their results are small and rare. Carrying them on the peer
  connection later needs no protocol change, since the grant of ADR 0025 already holds
  what the checks need.
- **Capability field.** It is `teams.read` for the reads of §4 and `teams.write` for the
  changes.

### 3. Live changes and reconnects reach the phone through the sealed metadata

- **The `team` field.** Each member's sealed session metadata (ADR 0113) gains one
  field: `team: {id, revision, role}`.
  - `role` is `leader`, `developer` or `reviewer`. It is a field of its own, never a tag:
    the phone-facing model carries no tags (§6).
  - The field is absent when the conversation is on no team.
  - It is inside the existing ciphertext, so the relay learns no more than it does from
    a title change.
- **When the daemon sends it.** On `team.changed` or `team.removed` it re-seals the
  metadata of the members concerned. Then it sends a heartbeat, at most one every
  2 seconds, carrying every change of that interval.
  - A member that leaves loses the field at that heartbeat.
  - A deleted team leaves all its members without it.
  - A new team appears with its members, whose conversations start in the same change.
- **A live change.** Whenever the phone refreshes the session list, it compares each
  member's `team.revision` with the revision it holds for that team.
  - A greater revision makes the phone read `team.list` again. If the team's page is
    open, it reads `team.get` as well.
  - A team id that it does not know also makes it read `team.list` again.
  - It keeps at most one read in flight per team, and starts at most one read per team
    every 2 seconds. The latest revision wins.
- **A reconnect.** This covers launch, coming to the foreground, a new network path and
  a machine coming online. The phone reads `team.list` for each machine that enables
  `teams.remote/1`, then `team.get` for an open team page.
  - Until those answers arrive, it keeps the teams it last read and marks them stale.
    Home keeps grouping by them, so a team never scatters (ADR 0115 §4).
  - A machine that comes back without the capability clears them.
- **An older answer.** An answer with a lower revision than the one the phone holds is
  dropped.
- **Why no team event in the stream.** The event stream is per conversation and fenced
  by its generation. A team event there would be uploaded once per member, and would
  need a key that every member shares. The metadata already reaches every device that
  can read the members, already sealed and already bounded.

### 4. Commands

**Common rules for every command:**

- **Idempotency.** The command's `idempotencyKey` (16 to 128 characters, one per user
  action) is the mailbox's key. A retry with the same key returns the original command.
  The daemon stores each mutating team result under the command id (`command_results`)
  before it acknowledges it, so a delivery repeated after a lost acknowledgement returns
  the first result instead of acting twice. This matters most for `team.create`, which
  starts conversations.
- **Revision precondition.** Every change carries `expected_revision`, the team revision
  the phone drew when the user acted. `team.create` carries the catalog revision
  instead. To check it, the daemon keeps a *changed revision* for each part a command
  depends on:
  - the team's settings (name, goal, branch, merge setting, paused and its reason);
  - its member list;
  - each task;
  - each decision.

  A command is refused with `stale_team` only when its target part changed after
  `expected_revision`. `team.delete` depends on the settings and the member list: the
  user must have seen who is in the team they delete, but a task or an Update that moves
  while agents work does not refuse it. A team whose agents are at work changes every few seconds, so a
  check on the whole team would refuse almost every answer. A refusal carries the
  current summary, so the phone redraws it and the user can act again.
- **Errors.** A refusal is the command's result `{ok: false, error_code, error_message}`.
  The daemon codes of ADR 0114 and ADR 0115 keep their meaning. Codes added for remote
  use:

  | Code | Meaning |
  | --- | --- |
  | `teams_remote_unavailable` | The relay or the machine does not enable the capability. |
  | `team_scope_forbidden` | The device or subject may not act on this machine's teams (§5). |
  | `stale_team` | The target part changed after `expected_revision`. The current `team` is included. |
  | `confirmation_required` | `team.delete` without `confirmation: "DELETE"`. |
  | `repository_not_found` | `repositoryId` is not in the catalog, or the catalog revision moved. |
  | `response_too_large` | The answer would exceed the bound of §6, even shed. |

  Every change can also fail with `team_not_found`, `teams_unavailable` and
  `invalid_payload`.

**Reads**, capability `teams.read`. None of them is recorded in the audit.

| Phone | Kind | Payload | Answer |
| --- | --- | --- | --- |
| Home, a reconnect | `team.list` | `{}` | `teams` (directory entries, §6), `teams_total`, `settings?` (the Mac's global automatic-turn budget, §6) |
| Team page | `team.get` | `team_id`, `known_revision?` | `team` (summary) and `updates` (newest 20), `updates_has_more`; `{unchanged: true, revision}` when `known_revision` is current |
| Task sheet | `team.task.get` | `team_id`, `task_id`, `history_limit` (1 to 50, 20 by default), `history_before?` | `task` (detail), `history_total`, `history_start` |
| Show older Updates | `team.updates.list` | `team_id`, `before_id?`, `limit` (1 to 50) | `updates`, `has_more` |
| Older decisions | `team.decision.list` | `team_id`, `state?`, `before_id?`, `limit` (1 to 50) | `decisions`, `has_more` |

**Changes**, capability `teams.write`. Each one is in the remote audit (§5).

| Phone action | Kind | Payload | Target part (fence) | Error codes besides the common ones |
| --- | --- | --- | --- | --- |
| Answer a decision, including **Start** for the first plan, **Continue** for a used-up budget, **Wake the leader again** | `team.decision.answer` | `team_id`, `decision_id`, `choice` (one of its choices, exactly) or `text` (1 to 4,000 characters), `expected_revision` | that decision | `decision_closed` |
| Pause / Resume | `team.update` | `team_id`, `paused`, `expected_revision` | settings | none; pausing a paused team succeeds and changes nothing |
| Who merges | `team.update` | `team_id`, `merge_policy` (`leader` or `user`), `expected_revision` | settings | none |
| Automatic turns | `team.update` | `team_id`, `turn_budget` (a whole number from 5 to 200, ADR 0114 open point 2), `expected_revision` | settings | `invalid_payload` |
| Language | `team.update` | `team_id`, `language` (`en` or `vi`, ADR 0120), `expected_revision` | settings | `invalid_payload` |
| Delete the team (its conversations stay) | `team.delete` | `team_id`, `confirmation: "DELETE"`, `expected_revision` | settings and members | `confirmation_required` |
| New team | `team.create` | envelope `repositoryId` and `expectedCatalogRevision`; `name`, `goal`, `integration_branch`, `merge_policy`, `leader_writes_requirements`, `execution_policy`, `risk_acknowledged`, `plan {leader, developers {count, members?}, reviewer}`, each agent being `{provider, model?, effort?}`, `turn_budget?`, `language?` (`en` or `vi`, ADR 0120; without it the team is English) (without `turn_budget`, the team takes the global value of Settings → Teams, which the phone reads from `team.list`'s `settings`; an older phone that does not read it shows "The Mac's default (Settings → Teams)" for an empty field) | the worktree catalog | `repository_not_found`, `invalid_repository`, `team_full`, `last_developer`, `provider_unavailable`, `reviewer_unsupported`, `member_unavailable`, `lane_overlap`, `execution_policy_denied` |
| Add a developer (a new conversation) | `team.member.add` | `team_id`, `name`, `lane?`, `paths?`, `provider`, `model?`, `effort?`, `expected_revision` | members | `team_full`, `provider_unavailable`, `member_unavailable`, `lane_overlap` |
| Remove a member | `team.member.remove` | `team_id`, `session_id`, `expected_revision` | members | `not_member`, `last_developer`, `role_required` |
| Add an existing conversation to a team | `team.member.join` | `team_id`, `session_id`, `lane?`, `paths?`, `expected_revision` | members | `session_not_found`, `already_member`, `other_repository`, `team_full`, `lane_overlap` |
| Hand a task over | `team.task.handover` | `team_id`, `task_id`, `owner` (a developer's or the leader's `session_id`), `reason` (1 to 500 characters), `expected_revision` | that task | `task_not_found`, `not_member`, `role_forbidden`, `invalid_transition`, `worktree_unavailable` |

- A phone may change only the `team.update` fields listed above. Any other field is
  refused with `invalid_payload`. Changing the name, the goal, the branch, the shared
  paths or the review rounds from the phone is not in the mock.
- What a decision's answer does stays the daemon's. "Start" releases the held
  assignments, "Continue (N more turns)" resumes the team, "Pause the team" pauses it,
  and so on (ADR 0114 §3, §8 and open points 2 and 5). The phone sends the choice's text
  and draws the result.

### 5. Security

- **End to end.** These are encrypted end to end: team names, goals, branches, lanes and
  their paths, members and their roles, tasks with criteria and history, decisions with
  their answers, and Updates. Every payload and result is sealed to the Mac or to the
  requesting device, and the `team` metadata is inside the session metadata's
  ciphertext. The relay sees none of it and stores no plaintext. Encrypted results
  expire with the mailbox's command time to live.
- **Who may read and who may change.**
  - Both reads and changes are allowed only from an *active* paired device of the
    machine's **owner subject** that lists `teams.remote/1`, and only while its key
    version is current.
  - A pending device, a revoked device, and any other member of an organization
    workspace get `team_scope_forbidden`.
  - The relay checks this when the command is queued and again when the daemon claims
    it, as for `session.create`. The daemon checks the owner and the machine again
    before it acts.
  - The reason: a team change starts turns and creates conversations on the owner's
    Mac, and a team read shows work across conversations, so neither is weaker than
    starting a conversation there.
  - The read and write capabilities are separate, so a later read-only device scope
    needs no protocol change. This version grants both together.
- **Execution policy.** Only `team.create` sets the team's execution policy. `full` needs
  `risk_acknowledged: true`, which the daemon checks exactly as it does for a desktop
  (`execution_policy_denied`). The phone's core sets the flag only from the same
  full-access confirmation that the composer shows before
  `session.execution_policy.apply` with `full`, never by default. `team.member.add` and
  `team.member.join` take no policy: a developer added later gets the team's policy, as
  ADR 0114 says.
- **Audit.** Every change in §4 is recorded in the remote command audit, with the
  command id, the kind, the device, the device's key version, the result code and the
  time. The entry also gains `team_id`, because a team command has no `session_id`, and
  the desktop's audit list names the team. The reads of §4 join the audit's list of
  kinds that only read.
- **Authority.** Nothing here gives the phone more authority than the desktop. The
  daemon applies every role rule of ADR 0114 to a phone command exactly as to a local
  one. A member's messages still approve nothing.
- **Plaintext that is new.** Nothing else about teams is readable by the relay.

  | Field | Where | Why it must be plaintext |
  | --- | --- | --- |
  | `teams.remote/1` | the host's heartbeat capabilities, the device record, the machine record | The relay enables the feature only on both sides (§1). It says that the feature is supported, not that a team exists. |
  | `kind` (`team.*`) and `capability` (`teams.read` or `teams.write`) | the command envelope | The relay authorizes, routes and rate-limits by them. They reveal which kind of action and when, as for every other command kind today. |
  | `repositoryId`, `expectedCatalogRevision` | the `team.create` envelope | They are opaque catalog values that are already plaintext for `session.create`. The relay checks that the repository is in the machine's catalog. |
  | `resultCode` | the command result | It is plaintext for every command, for mailbox status and retries. A team code such as `stale_team` reveals only the outcome. |

  **Known side channels.** The metadata ciphertext grows a few bytes when a conversation
  joins a team, and heartbeats get more frequent while a team works. Both show activity,
  never content. They are accepted, like the timing of turns today.

### 6. The phone-facing read model

All names are snake_case, as in the daemon's local protocol. Every string is bounded as
the daemon stores it, unless a smaller bound is given here. Ids are strings. None of the
objects carries a local path or a tag.

**Directory entry**: what `team.list` returns, one per team.

| Field | Content |
| --- | --- |
| `id`, `revision` | the team and its revision |
| `repository_id` | the opaque catalog id of the team's repository, which Home groups by |
| `name` | up to 120 bytes |
| `paused`, `paused_reason` | the reason is `user`, `budget`, `empty_role` or empty |
| `open_decisions` | drives the warning mark |
| `tasks_total`, `tasks_done` | |
| `members` | at most 7 entries `{session_id, number, role}` |

At most 100 entries are returned, followed by `teams_total`.

**Settings**: `team.list` also answers an optional object `settings` with the Mac's
global automatic-turn budget. It has the four fields of the local `team.settings.get`, with
the same names and meanings: `turn_budget` (the value of Settings → Teams that a team
created without its own takes), `turn_budget_minimum`, `turn_budget_maximum` and
`turn_budget_default`. New team uses it to say what an empty budget field means and
to check a typed one against the bounds. It is read-only: no remote kind changes the
global value, and `team.settings.update` stays a local kind. An older phone ignores
the object. A daemon that does not send it leaves the phone on its label "The Mac's
default (Settings → Teams)", and the daemon still applies the global value. The relay
stores the answer as ciphertext and needs no change.

**Summary**: `team.get`, and the `team` of a change's result or of a `stale_team`
refusal.

- **The team.**
  - Identity: `id`, `revision`, `repository_id`, `name`, `goal` (up to 16,000
    characters and 24 KiB of UTF-8, whole, with its line breaks and its `@agent/<number>` mentions as plain
    text; ADR 0114 §9). The goal was cut to 4,000 characters before; it is now sent whole
    as the same string field, so the contract and its version do not change and an older
    phone shows the longer goal as text.
  - Settings: `integration_branch`, `merge_policy` (who merges: `leader` or `user`),
    `leader_writes_requirements` (who writes requirements), `execution_policy`,
    `language` (ADR 0120: `en` or `vi`; the texts the daemon writes for the team, its Updates
    and its own decisions, are in it, and a phone that shows its own fixed text should follow
    it; an older phone ignores the field).
  - State: `paused`, `paused_reason`, `plan_approved`, `held_count`.
  - The automatic-turn budget: `turn_budget`, `automatic_turns` and `turns_left`.
    `turns_left` is computed by the daemon, so the phone infers nothing.
  - The review queue: `review: {reviewing, queued, reviewers}`, task ids. T-23 adds
    `reviewers`: one `{session_id, reviewing, queued}` per reviewer, in the order they
    joined. `reviewing` and `queued` keep their meaning for a reader from before (the
    first review in flight and every waiting task).
  - Counts: `tasks_total`, `tasks_done`, `open_decisions`, `decisions_total`.
  - Times: `created_at`, `changed_at`.
- **Members**, at most 7, each with these fields:
  - `session_id`, `session_generation`, and `number` (the conversation number,
    `@agent/NNNN`).
  - `title`: the member's name.
  - `role`: `leader`, `developer` or `reviewer`.
  - `lane` and `paths`, for developers only.
  - `state`: `working`, `waiting_for_review`, `blocked` or `idle`. The daemon computes it
    in that order of precedence. A running turn is `working`. Otherwise the member's own
    task decides: `blocked`, then `in_review`. Anything else is `idle`.
  - `now`: the task the member works on, `{task_id, title}` or null.
  - `agent`: `{provider, model, effort}`.
  - `changed_revision`.
- **Tasks**, at most 200: every task that is not done, then the newest done ones. Each
  task has these fields:
  - `id` and `title` (up to 200 characters).
  - `state`: `todo`, `in_progress`, `in_review`, `ready`, `blocked` or `done`.
  - `owner`: the owner's `session_id`, or empty when unassigned.
  - `lane` and `depends_on` (task ids).
  - `criteria_total` and `criteria_met`.
  - `round` (the review round) and `pull_request` (up to 500 characters).
  - `branch` and `worktree_id` (opaque).
  - `assignment_held`, `outside_lane_total`, `decision_id` (the open decision that
    blocks it) and `changed_revision`.

  A summary task carries no description, no criterion text and no history. Those come
  from `team.task.get`.
- **Decisions.** The open ones (at most 10, then page with `team.decision.list`), then
  the 10 newest answered or withdrawn. Each decision has these fields:
  - `id` and `kind`: `question`, `first_plan`, `budget`, `empty_role`, `merge` or
    `stalled`.
  - `question` (up to 1,000 characters), `context` (up to 4,000 characters) and
    `choices` (2 to 4, each up to 200 characters).
  - `recommended`: one of the choices, or null.
  - `task_id`, the task it blocks.
  - `raised_by`: `{role: "leader", number}` or `{role: "daemon"}`.
  - `raised_at`, `state` (`open`, `answered` or `withdrawn`), `answer`, `answered_at`.
  - `delivers_to`: where the answer goes, computed by the daemon. Each entry is
    `{role, session_id, number}`, for the leader and for the owner of the blocked task.
  - `changed_revision`.
- **Updates**: the newest 20. Each update has these fields:
  - `id`, `kind` (the daemon's update kind), `at`;
  - `actor`: `{role, number}`, `{role: "user"}` or `{role: "daemon"}`;
  - `task_id`, `pull_request_number` (the task's pull request when the update is about
    one, or null), `text` (up to 500 characters), `actionable`;
  - `warning`. The daemon sets it for `blocked`, `round_limit`, `outside_lane`,
    `merged_unreviewed`, `stalled`, `message_failed` and `merge_waiting`.

  The phone chooses the icon from `kind`. It draws the detail line under the text from
  `actor` (`@agent/<number>`) and `pull_request_number` (`#<n>`), for example
  "@agent/2035 · #412". The daemon sends no detail text of its own.

**Task detail**: what `team.task.get` returns.

- The summary fields.
- `description`, up to 16 KiB.
- `criteria`: at most 20, each `{text, met}`.
- `outside_lane`: at most 20 entries `{path, lane, owner_number}`. `path` is relative to
  the repository. `lane` and `owner_number` name the lane the path belongs to and the
  member whose lane it is, or `""` and null when it belongs to no lane. The phone draws
  "<path> belongs to <lane>, the lane of @agent/<owner_number>".
- `criteria_note`: up to 500 characters, the daemon's text of the newest change to the
  criteria (when, what, and who was told), or `""` when they never changed.
- `history`: one page of at most 50 entries `{at, actor, kind, text}`, each text up to
  2,000 characters.

**Bounds of a result.**

- The plaintext of a result is at most **384 KiB**, so the sealed result stays within the
  mailbox's 512 KiB result limit.
- When a summary would exceed it, the daemon sheds parts in this order, each flagged
  `<part>_truncated: true`: answered decisions, then Updates, then done tasks.
- If the summary still does not fit, the command fails with `response_too_large`. The
  phone says so on the team page, and its connection is unaffected.
- A command payload is at most 64 KiB, as for every command.

**No tags.** A member's role is the field `role`, both in the summary and in the `team`
metadata. The phone never reads the role from a tag, and nothing here carries tags.

## Consequences

- Each of the three parts can be built against this record and the contract
  (`ketplus-control-v2.openapi.json`, its `x-ketplus-teams` section and the fixture
  `team-remote-v1.json`) without another design round.
- **Older parts.** An older phone, relay or daemon never shows a partial team UI. It
  shows the members as conversations.
- **Load.** A busy team sends more heartbeats, at most one every 2 seconds, and makes the
  phone read `team.get` while its page is open. Nothing is pushed while no page is open,
  beyond the metadata already sent.
- **What the phone shows.** Task states follow the daemon, so the phone draws `ready`,
  which the mock does not have yet (open question 1).
- **The local protocol** is unchanged for local clients, except for the parts added
  below: the changed revisions, the fences, member `state` and `now`, and `delivers_to`
  and `warning`. The desktop may use them too.

## Alternatives considered

- **A team key shared by all devices**, used to publish a sealed team document in the
  heartbeat. Rejected: it needs a new key hierarchy, key distribution and rotation on
  revocation. It would also send the whole document on every change, whether or not a
  page is open.
- **Team events in each member's event stream.** Rejected for the reasons of §3: the
  event would be duplicated per member, and the stream is fenced by the conversation's
  generation.
- **A check on the whole team revision for every change.** Rejected: while agents work,
  the revision moves every few seconds, and the user's answer would be refused again
  and again.
- **Role as a tag.** Rejected: tags are the user's labels (ADR 0113), and a role is
  enforced by the daemon.

## Follow-up tasks

### Daemon (ketplus `apps/daemon`)

1. Advertise `teams.remote/1` in the heartbeat's capabilities.
2. Add the `team` field (`id`, `revision`, `role`) to the member conversations' sealed
   metadata. On `team.changed` and `team.removed`, re-seal it and send a heartbeat, at
   most one every 2 seconds.
3. Accept the team kinds of §4 as Mac-sealed commands:
   - decrypt them with the associated data of §2;
   - check the owner and the machine;
   - resolve `repositoryId` and `expectedCatalogRevision` through the worktree catalog
     (`repository_not_found`);
   - forward the payload to the local `team.*` request with actor `user`;
   - seal the result to the requesting device with `keyId` `team-result:<id>` and the
     result associated data of §2. `encryptSealed` names its key `session-create:<id>`
     today; it takes the key id as an argument instead.
4. Restrict the payloads to the fields of §4:
   - only `paused`, `merge_policy` and `turn_budget` through `team.update`;
   - `confirmation: "DELETE"` for `team.delete`;
   - a reason of 1 to 500 characters for a handover;
   - `choice` or `text` for an answer.
5. Keep a `changed_revision` for the settings, the members, each task and each
   decision, and refuse with `stale_team` as §4 says, including the current summary.
6. Store each mutating result in `command_results` under the command id before
   acknowledging it, and return it on a repeated delivery.
7. Build the phone projection of §6:
   - directory entries, the summary and the task detail;
   - member `state`, `now`, `agent` and `session_generation`;
   - decisions with `delivers_to`, and updates with `warning` and
     `pull_request_number`;
   - `outside_lane` entries with their lane and its owner, and `criteria_note` from the
     newest criteria change in the task's history;
   - `turns_left`, and opaque `repository_id` and `worktree_id`;
   - no tags.
8. Enforce the 384 KiB bound with the shedding order of §6. Add a test of the worst case,
   in the manner of `TeamPayloadTest`.
9. Add the reads to `remoteCommandMayChange`'s list of reads. Add `team_id` to the remote
   audit entry and to `remote.audit`, so the desktop's audit list can name the team.
10. Tests: each kind's mapping and refusals, every fence, a repeated delivery of
    `team.create`, the `full` policy without the acknowledgement, the audit entries,
    and the metadata with a team, without one, and after deletion.

### Relay (private service)

1. Offer `teams.remote/1`. Enable it per machine when the host advertises it, show it in
   `Machine.capabilities` and `Compatibility.capabilities`, and accept it on device
   records.
2. Add `POST /v2/mobile/machines/{machineId}/team-commands` (`TeamCommandRequest`):
   - deduplicate on `idempotencyKey`;
   - authorize the owner subject's active device with a current key version, when
     queued and when claimed (`team_scope_forbidden`);
   - require the capability on the machine and the device (`teams_remote_unavailable`);
   - for `team.create`, require the repository to be in the machine's catalog.
3. Add the team kinds and the `teams.read` / `teams.write` capabilities to the command
   kinds, the associated data (`commandAssociatedData`) and the mailbox's active-command
   scopes, without reading or storing any payload.
4. Keep the existing 512 KiB result bound and time to live, and rate-limit team commands
   with the existing per-device mobile limit.
5. Tests: the capability negotiation, including one side missing; a refused scope (a
   pending device, a revoked device, a workspace member who is not the owner); a
   repeated `idempotencyKey`; the associated-data vectors from
   `contracts/fixtures/team-remote-v1.json`; and no plaintext team field in storage or
   logs.

### iOS core (KetPlusRemoteCore, no UI)

1. Advertise `teams.remote/1` on the device record. Enable teams per machine only when
   `Machine.capabilities` lists it; otherwise expose no teams.
2. Decode `team {id, revision, role}` from the session metadata, and tolerate its
   absence and unknown fields.
3. Models for the directory entry, the summary, the member, the summary task, the task
   detail, the decision and the update, decoded from the fixture.
4. A team store that:
   - reads `team.list` on connect and when a revision changes, and `team.get` for an
     open page, as §3 says;
   - keeps one read per team in flight and starts at most one every 2 seconds;
   - drops older answers;
   - keeps the teams it last read, marked stale.
5. Command intents for every change of §4:
   - a fresh `idempotencyKey` per user action, reused on retry;
   - `expected_revision` from the revision the phone drew;
   - sealing to the Mac with the associated data of §2;
   - opening the result sealed to the device (`team-result:<id>`, the result
     associated data of §2, checked against the fixture vector);
   - mapping each error code to a state the UI can show, with `stale_team` applying the
     summary it carries.
6. Set `risk_acknowledged` only from the full-access confirmation flow.
7. Tests: the fixture round trip, the fallback without the capability, revision ordering
   and stale answers, idempotent retries, and every error code.

## Open questions for the leader

1. **`ready` on the phone.** The daemon has a sixth task state, `ready` (reviewed,
   waiting to merge), which the mock does not draw. *Recommended*: draw it as "Ready to
   merge" with the success tone, and add it to the mock.
2. **A written answer.** The protocol accepts `text` as well as a choice, but the mock
   offers only the choices. *Recommended*: choices only in the first version.
   *Follow-up (T-55)*: the desktop card has "Other…" with a written answer; the phone
   follows as its own change.
3. **The reason of a handover from the phone.** The daemon requires a reason, and the
   mock's handover sheet has no field for it. *Recommended*: add an optional field that
   defaults to "Handed over from <device name>".
4. **Deleting a team.** Should it ask for the deletion challenge that `session.delete`
   uses (a fresh sign-in check), or is the confirmation dialog enough, since the
   conversations stay? *Recommended*: the dialog only.
5. **`full` access for a new team from the phone.** It is allowed with the same
   acknowledgement as for one conversation. Should the phone offer `ask` and
   `read-only` only? *Recommended*: allow it, for parity with the conversation
   setting.
6. **Workspace members.** Should an organization member who can read the owner's
   conversations also read the owner's teams? *Recommended*: no, owner only, until
   workspaces share Macs on purpose.
