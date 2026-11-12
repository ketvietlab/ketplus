# Automatic turns: mock and app

ADR 0114, open point 2. The three places that show the automatic-turn budget, each as
the Qt mock draws it beside the app, in light and dark.

| Place | Mock | App |
|---|---|---|
| Settings → Teams | [light](mock-settings.png), [dark](mock-settings-dark.png) | [light](app-settings-light.png), [dark](app-settings-dark.png); refused: [light](app-settings-refused-light.png), [dark](app-settings-refused-dark.png) |
| New team | [light](mock-new-team.png), [dark](mock-new-team-dark.png) | [light](app-new-team-light.png), [dark](app-new-team-dark.png) |
| A team's settings (header and Automatic turns…) | [light](mock-team-budget.png), [dark](mock-team-budget-dark.png) | [light](app-team-budget-light.png), [dark](app-team-budget-dark.png) |

Mock, offscreen at 1440 × 900 and zoom 1:

```sh
KetPlusMock --screen=settings --state=teams [--dark] --screenshot=mock-settings.png
KetPlusMock --screen=teams --open=new-team [--dark] --screenshot=mock-new-team.png
KetPlusMock --screen=teams --open=turn-budget [--dark] --screenshot=mock-team-budget.png
```

App, from the widget tests with `QT_QPA_PLATFORM=offscreen` and
`KETPLUS_TEAM_BUDGET_CAPTURE=<folder>`: `ketplus_settings_teams_page_tests`
(Settings at 1200 × 760), `ketplus_team_dialogs_tests` (New team) and
`ketplus_team_page_tests` (the team page with its dialog), both at 1440 × 900. Each test
also checks that the field, and the refusal, end inside the window.

The app's New team lists the members in its own existing layout; only the Automatic turns
row is new there. The sample data differ (the mock's Release 1.4, the tests' Shop).
