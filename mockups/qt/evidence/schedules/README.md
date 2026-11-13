# Schedules mock evidence

English UI and synthetic sample content. The language requirement was updated before review.
All files are generated from the standalone Qt mock with the pinned `Ket` kit. There is no new
sidebar navigation entry and no production scheduler implementation.

Each basename below has `-light.png` and `-dark.png` variants.

| Surface | Screenshot basename | Reproduce with |
| --- | --- | --- |
| 1. Menu entry; list, enable controls and next/last run | `01-schedules`, `01-view-menu`, `01-conversation-menu`, `01-team-menu` | `--screen=schedules`; `--screen=workspace --open=view-menu`; `--screen=workspace --open=conversation-menu`; `--screen=teams --open=team-menu` |
| 2. Editor, three targets, repeat options and policies | `02-editor-team`, `02-editor-new-conversation`, `02-editor-weekly`, `02-editor-policies`, `02-editor-validation` | `--screen=schedules --state=edit`, `new-conversation`, `weekly`, `policies`, `validation` |
| 3. Detail/history and delete confirmation | `03-history-failure`, `03-delete` | `--screen=schedules --state=failure`, `delete` |
| 4. Schedule-send picker beside Send | `04-schedule-send` | `--screen=workspace --state=schedule-send` |
| 5. Scheduled user-message header, opens that schedule | `05-scheduled-transcript` | `--screen=workspace --state=scheduled` |
| 6. Failure toast with Open schedule | `06-failure-toast` | `--screen=workspace --state=schedule-failure` |
| 7. Scheduled team creation, presets and invalid start | `07-team-start-dialog`, `07-team-start-invalid` | `--screen=teams --open=new-team-scheduled`, `new-team-start-invalid` |
| 8. Waiting team, recurring schedules and Updates | `08-team-schedules`, `08-team-updates` | `--screen=teams --state=scheduled`; add `--scroll=1` for Updates |
| Empty state | `09-empty` | `--screen=schedules --state=empty` |
| Minimum schedule window and scrolling editor | `10-minimum`, `10-minimum-editor` | `--screen=schedules --size=900x600`; add `--state=new-conversation` for editor |

Use the executable's `--light` / `--dark` and `--screenshot=<file>` options with these arguments.
Captures use `QT_QPA_PLATFORM=offscreen`, 1× zoom: main surfaces 1440 × 900, schedule window
1160 × 800, minimum schedule window 900 × 600. The offscreen platform draws the menu bar in
the client capture; native macOS owns its system menu bar. Native review must use
`--display=secondary` and happens on DELL, separately from the deterministic capture run.

## Behavior verified

- CTest covers date/time parsing, one-minute/thirty-day team-start bounds, required prompt,
  weekly day selection, interval bounds, three zone previews and schedule-store mutations.
- The Pro smoke opens every existing screen plus scheduling states, exercises editor validation,
  create, filtered empty state, cancel/confirm deletion, conversation prefill and one reused
  non-modal window. The existing team rules still run; no provider quota is used.
- Native review on the secondary DELL display exercised search, clearing a no-results filter,
  opening New schedule, rejecting an empty save and cancelling back to the list. English
  labels and the live list/detail layout were inspected.
- The editor body scrolls while Save/Cancel stay visible. The detail pane scrolls while its
  Run now/Pause/Edit/Delete actions stay visible. The list and detail selection share the store.

The fixture clock is 13 November 2026, 15:00 GMT+7. History and Run now are simulated. The
supported preview zones have fixed offsets; this mock is not evidence for production recurrence,
DST, sleep/wake, concurrency, provider authentication or daemon behavior.
