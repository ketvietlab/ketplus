# Schedules mock evidence — round 2

English UI and synthetic sample content, rendered by the standalone Qt mock using pinned
Ket components. Schedules are created only in New conversation and New team. The manager
is edit/manage only. There are no sidebar/header clocks or legacy creation menu actions.
The composer clock is immediately right of the Context/Limit ring, with equal visible diameter
and a shared vertical center, including at the minimum main-window size.

All paths below are relative to `mockups/qt/evidence/schedules/`. Each basename has both
`-light.png` and `-dark.png` variants (30 states, 60 PNGs).

| Surface/state | Screenshot basenames | Reproduce with |
| --- | --- | --- |
| Manager and View menu | `01-schedules`, `01-view-menu` | `--screen=schedules`; `--screen=workspace --open=view-menu` |
| Edit existing schedule: team/new-conversation/weekly | `02-editor-team`, `02-editor-new-conversation`, `02-editor-weekly` | `--screen=schedules --state=edit`, `new-conversation`, `weekly` |
| Visible busy/missed policies; required prompt error | `02-editor-policies`, `02-editor-validation` | `--screen=schedules --state=policies`, `validation` |
| Failure history and delete confirmation | `03-history-failure`, `03-delete` | `--screen=schedules --state=failure`, `delete` |
| New conversation: default, one-time, repeat + More, weekly | `04-new-send-now`, `04-new-at-time`, `04-new-repeat-more`, `04-new-weekly` | `--screen=workspace --state=schedule-new`, `schedule-at`, `schedule-more`, `schedule-weekly` |
| Active clock with the normal Send action | `04-scheduled-send` | `--screen=workspace --state=schedule-ready` |
| Existing-conversation manage-only clock menu | `04-existing-schedule` | `--screen=workspace --state=schedule-existing` |
| Scheduled user-message marker | `05-scheduled-transcript` | `--screen=workspace --state=scheduled` |
| Failure notification with Open schedule | `06-failure-toast` | `--screen=workspace --state=schedule-failure` |
| New team default Now/Off and Scheduled start | `07-team-now`, `07-team-start-dialog` | `--screen=teams --open=new-team`, `new-team-scheduled` |
| New team recurrence and goal-prefilled work order | `07-team-repeat` | `--screen=teams --open=new-team-repeat` |
| Existing locked roles, model/effort and permissions | `07-team-members` | `--screen=teams --open=new-team-members` |
| Invalid scheduled start | `07-team-start-invalid` | `--screen=teams --open=new-team-start-invalid` |
| Inactive waiting team; recurring schedule management; Updates | `08-team-waiting`, `08-team-recurring`, `08-team-updates` | `--screen=teams --state=scheduled`; `--state=recurring`; add `--scroll=1` for Updates |
| True empty manager, creation guidance only | `09-empty` | `--screen=schedules --state=empty` |
| Minimum manager/editor | `10-minimum`, `10-minimum-editor` | `--screen=schedules --size=900x600`; add `--state=new-conversation` for editor |
| Minimum composer popover and scheduled submit | `10-minimum-popover`, `10-minimum-scheduled-send` | `--screen=workspace --size=1024x640 --state=schedule-more`, `schedule-ready` |

## Reproduction and checks

Run `python3 mockups/qt/tools/capture_schedules.py <KetPlusMock executable>`. It renders
offscreen in both themes, rejects QML errors and invalid startup states, removes obsolete
scheduling captures, and records exact arguments, dimensions and SHA-256 in `manifest.json`.
It rejects identical empty/list and policies/editor evidence. Add `--light` or `--dark` and
`--screenshot=<path>` to an individual command above to reproduce one image.

Default main windows are 1440 × 900, Schedules 1160 × 800; minimum manager 900 × 600 and
minimum main window 1024 × 640. Captures use 1× zoom. Native macOS owns its menu bar;
the offscreen platform renders the menu bar inside the captured client window. Every
native review run uses `--display=secondary` on DELL, separately from these captures.

CTest covers recurrence validation, start bounds, store mutations, deterministic empty and
policy startup, six composer startup states at minimum size, a genuinely inactive waiting
team, recurring New team creation and visible role/model controls. The Pro smoke visits
84 screen states and exercises in-memory creation, editing, pause/run/delete and existing
team behavior. The Lite smoke checks that the unrelated edition still opens correctly.

Round-1 corrections: deterministic empty/policy captures (F1/F2), obsolete sidebar clocks
removed by the new design (F3), standard Updates icon/time column (F4), one banner icon
(F5), Scheduled radio wording (F6), separate waiting-team data with zero used turns and no
pending decisions/tasks/updates (F7). The members images expose the original role controls.

The fixture clock is 13 November 2026, 15:00 GMT+7. History and Run now are simulated;
the preview zones have fixed offsets. This evidence does not verify production recurrence,
DST, sleep/wake, provider access or daemon integration.
