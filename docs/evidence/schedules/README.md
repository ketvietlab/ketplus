# Schedules window

Offscreen grabs of the production Schedules window, with `ket::Style` installed and the
theme pinned to Light or Dark. The schedules are synthetic. The list, detail and editor
follow the approved mock in `mockups/qt/evidence/schedules/`. A team id uses the protocol
alphabet, so the sample reads `Release.1.4` rather than the mock's display name.

The editor target is read-only. An empty prompt is shown under the prompt, with a red
border. The form scrolls above Cancel and Save, and the rule summary stays in the form.
`01-view-menu` shows Schedules… in a View menu, shortcut ⇧⌘J.

| Files | State |
| --- | --- |
| `01-schedules-{light,dark}.png` | List, detail and run history at 1160×800 |
| `02-editor-team-{light,dark}.png` | Edit dialog |
| `02-editor-validation-{light,dark}.png` | Empty prompt |
| `03-delete-{light,dark}.png` | Delete confirmation |
| `09-empty-{light,dark}.png` | No schedules, and no create button |
| `10-minimum-{light,dark}.png` | Same list at 900×600 |
| `01-view-menu-{light,dark}.png` | View › Schedules… |

Reproduce with `QT_QPA_PLATFORM=offscreen` and
`KETPLUS_SCHEDULE_CAPTURE=<directory>` on `ketplus_schedule_window_tests`.
Without that variable the test does not write files.

## Team start and team schedules

Offscreen grabs of the New team dialog and the team page. An immediate team with
repeat off sends neither `start_at` nor `repeat`. A waiting team shows the start
banner. A team schedule is listed for that team only, with Edit, Pause, Open in
Schedules and the leader conversation.

| Files | State |
| --- | --- |
| `07-new-team-{light,dark}.png` | New team, Start now, repeat off |
| `08-team-waiting-{light,dark}.png` | Starts at, with Start now / Change time / Cancel start |
| `08-team-schedules-{light,dark}.png` | One team schedule, skip reason, no add button |

Reproduce with `KETPLUS_TEAM_SCHEDULE_CAPTURE=<directory>` on
`ketplus_team_schedule_form_tests`. Without that variable the test does not
write files.
