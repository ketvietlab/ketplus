# Composer schedule

Offscreen grabs of the production agent workspace, light and dark. The schedule is
synthetic. These are not the mock renders in `mockups/qt/evidence/schedules/`.

| Files | State |
| --- | --- |
| `01-composer-clock-{light,dark}.png` | Clock immediately after the context ring on a new conversation |
| `02-popover-send-now-{light,dark}.png` | Schedule first message, Send now selected |
| `03-popover-repeat-{light,dark}.png` | Repeat, every day |
| `04-existing-clock-{light,dark}.png` | Clock on a conversation that already has a schedule |
| `05-existing-menu-{light,dark}.png` | Edit, Pause, Open in Schedules, above the clock |
| `06-scheduled-marker-{light,dark}.png` | Scheduled run marker on the user message. The row is already on screen when the clock is grabbed, so this frame matches `04` |
| `07-failure-toast-{light,dark}.png` | Failed run, with Open schedule |

Reproduce with `QT_QPA_PLATFORM=offscreen` and
`KETPLUS_COMPOSER_SCHEDULE_CAPTURE=<directory>` on `ketplus_composer_schedule_tests`.
Without that variable the test does not write files.
