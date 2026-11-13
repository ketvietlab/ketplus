# Schedules window

Offscreen grabs of the production Schedules window, with `ket::Style` installed and the
theme pinned to Light or Dark. The schedules are synthetic. These are not the mock
renders in `mockups/qt/evidence/schedules/`.

The editor target is read-only. An empty prompt is reported on the card, because the
field note sits under the native text editor. `01-view-menu` is the View action in an
offscreen menu; the rest of the workspace chrome belongs to other tasks.

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
