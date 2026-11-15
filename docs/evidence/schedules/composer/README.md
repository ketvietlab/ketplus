# Composer schedule

Offscreen grabs of the production agent workspace, light and dark. The schedule is
synthetic. Compare them with the approved mock in `mockups/qt/evidence/schedules/`.

| Files | State |
| --- | --- |
| `01-composer-clock-{light,dark}.png` | Clock immediately after the context ring on a new conversation |
| `02-popover-send-now-{light,dark}.png` | Schedule first message, Send now selected |
| `03-popover-repeat-{light,dark}.png` | Repeat, every day, with time zone and the next-run summary |
| `04-existing-clock-{light,dark}.png` | Clock on a conversation that already has a schedule, before its message is shown |
| `05-existing-menu-{light,dark}.png` | Edit, Pause, Open in Schedules, above the clock |
| `06-scheduled-marker-{light,dark}.png` | Scheduled-run marker, with its clock icon, on the user message |
| `07-failure-toast-{light,dark}.png` | Failed run: schedule name, reason and Open schedule |
| `08-popover-at-time-{light,dark}.png` | At a time, with Tonight / Tomorrow presets and a time zone |

Reproduce with `QT_QPA_PLATFORM=offscreen` and
`KETPLUS_COMPOSER_SCHEDULE_CAPTURE=<directory>` on `ketplus_composer_schedule_tests`.
Without that variable the test does not write files.
