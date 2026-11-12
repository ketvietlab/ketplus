# ADR 0106: The agent reports work through a tool and KetPlus renders it

Status: accepted (Jay, 2026-11-10). Extends ADR 0027. Implements #404; mock #403.

## Context

Every user-started turn ends with a progress matrix, which `responseGuidance()` asks
for. The matrix is free-form markdown that the agent rewrites each turn:
- items are renamed, merged or dropped between turns;
- counts are typed by hand, so they can disagree with the rows;
- nothing links an item in one turn to the same item in an earlier turn.

The report reads as fragments rather than one record of the conversation's work. The app
cannot count it, filter it or show it anywhere else.

## Decision

- **Tool.** The ketplus_agents MCP server gains `work_report`. Its call carries an
  optional `title` and up to 50 `items`. Each item has:
  - a stable `key`;
  - `group`, `title` and `status` (`done`, `active`, `waiting`, `blocked` or
    `cancelled`);
  - optional `evidence` and `remaining`;
  - an optional `next` with `action` and `owner` (`agent`, `user` or a conversation
    number);
  - optional `verified` checks (`code`, `tests`, `build`, `package`, `deploy`,
    `device`, `capture`, `review`). Each check stays separate: passing one does not
    complete the others.

  The server routes the call to `agents.work_report`, authenticated by the
  conversation's agent token like the other `agents.*` operations.
- **Merge.** The daemon merges a call into the conversation's ledger (`WorkReport.cpp`):
  - an item upserts by key;
  - items the call does not mention stay as they were;
  - an item leaves the open set only as `done` or `cancelled`. Closing it removes
    its `next` and `remaining` unless the same call reports them, so a finished item
    never shows a stale next step;
  - each item records `created_turn` and `updated_turn`, from which the app derives
    New and Updated.

  A call is rejected as a whole, with an error that says what to fix, when it has:
  - an unknown status, owner or check;
  - an invalid or repeated key;
  - a new item without a title, group or status;
  - a `done` item with neither evidence nor a verified check.
- **Bounds and privacy.** Every text field is reduced to one line and redacted with the
  transcript's redactions (`sanitizeCloudText`) before it is stored. It is then capped:

  | Field | Cap |
  | --- | --- |
  | Title | 200 |
  | Group | 80 |
  | Evidence | 1,000 |
  | Remaining | 600 |
  | Next action | 300 |

  A ledger holds at most 200 items and 160 KiB of JSON, so its event, and the
  `turn.completed` that repeats it, stay under the journal's 256 KiB event limit.
  Beyond either bound, the oldest closed items leave first; open work never leaves
  silently, and a call that would need it is rejected. The tool
  description tells the agent never to include secrets, credentials or file contents.
- **Storage.** `work_reports` holds one row per conversation generation, with the
  current ledger and its version. Retention may drop old events, so the ledger is not
  rebuilt from them. A store never replaces a newer version.

  The row is part of the generation's history:
  - a fork copies it;
  - deletion removes it with the other per-generation state;
  - an abandoned fork's purge removes it.
- **Event.** Each accepted call journals `work_report.updated` with the merged ledger.
  A turn's last such event is the report that turn ended with, so earlier turns keep
  their own snapshot while later turns change the ledger.
  - **Cloud:** the event's text is redacted and capped again on its way to the cloud
    (`projectEventPayloadForCloud`), and it is relayed only where the transcript is.
  - **Turn end:** `turn.completed` carries the stored ledger as `work_report` when the
    ledger was last changed in that turn. The end row is then complete on its own,
    even when a history page boundary falls between the report and the turn's end.
  - **Failure:** when the event cannot be journaled, the tool answers with an error
    (the ledger is saved) and the agent ends the turn with the matrix, so a report
    is never hidden behind a success.
- **Guidance.**
  - When a session has the ketplus_agents server (Claude, Codex and Cursor), the
    response guidance replaces the matrix: the agent records the turn's work with
    `work_report` and ends with a short summary, without repeating the report as a
    table. The audit rules are unchanged: carry unfinished work, reflect scope changes,
    never invent success, and keep working rather than stopping.
  - When the tool is unavailable or fails, the agent ends with the matrix as before. A
    session without the server keeps the old guidance unchanged.
  - Agent-started turns neither add a matrix nor call `work_report` (ADR 0027).
- **App.** The app renders the turn's last report as a card at the end of the turn and
  shows the whole ledger in the Report document tab. It computes every count and the
  progress itself.
  - The header's Work report button and the card's footer open the tab. There is one
    Report tab, and it follows the selected conversation.
  - The tab reads the stored ledger with `session.work_report.read` and then applies
    the conversation's `work_report.updated` events. An older ledger version never
    replaces a newer one, so a slow read cannot undo a live update.
  - While a turn runs, its latest report closes the live transcript, as the mock's
    `workspace/report-running`: a Running badge, and the footer shows the turn's live
    activity and "Now" instead of the next step. The card replaces the run line until
    the turn ends, when the end-of-turn card takes over. The desktop marks a
    `work_report.updated` of the running turn as current; a new turn or another
    conversation starts without it.
- **Reconciling a long conversation.** The agent cannot read the stored ledger, so after
  many turns and compactions it forgot keys: it reported the same work again under new
  keys and never closed items from early turns (a team leader had 41 open items of 74 at
  turn 156, eight of them untouched since turn 2). Therefore:
  - The tool's answer lists every open item (key, status, title capped at 120 characters,
    `updated_turn`), newest first, at most 100, with a `reconcile` instruction. An open
    item no call changed for 20 turns (`staleTurns`) is marked `stale`, and `stale` counts
    them. The response guidance asks the agent to close finished or obsolete items, confirm
    or close stale ones and never add a new key for listed work; a team task's key is its
    task id.
  - An item may carry `replaces: [keys]`: the same work reported earlier under another key.
    Those open items become `cancelled` with `replaced_by`; done items stay done. A key the
    report does not have, or one the same call also reports, is refused in words.
  - The card shows a stale item muted with "No update for N turns", and a stale item whose
    next step is the user's does not make the report "Needs you".

## Consequences

- The project version moves to 0.1.52 for this daemon change. An older desktop ignores
  `work_report.updated`. An older daemon does not offer the tool, so its sessions keep
  the matrix.
- The markdown matrix remains the fallback and the export format.
- Plans (#198) are separate. A plan owns tasks, dependencies and continuation; the
  report is the end-of-turn record and may reference plan tasks by key.

## Alternatives considered

- **Parse the markdown matrix.** Rejected: rows have no stable identity, and a parser
  would encode the agent's formatting quirks.
- **Rebuild the ledger from events.** Rejected: retention may drop the events that hold
  long-lived items.
- **Let the agent send counts.** Rejected: the counts must always match the items, so
  the app computes them.
