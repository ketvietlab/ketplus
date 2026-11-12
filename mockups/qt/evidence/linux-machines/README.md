# Linux machines in Connections (#419)

Side by side of the mock and the app for the Linux rows, the "Connect a Linux
machine" dialog and a machine's Details (ADR 0118 states). Both were rendered
offscreen.

- Mock: `KetPlusMock --screen=connections --size=1280x1640` and
  `--open=linux-hostkey|linux-relay|linux-details` at 1280 × 900.
- App: `KETPLUS_CONNECTIONS_CAPTURE=<dir> ketplus_remote_workspace_tests` in a
  Debug build with `KETPLUS_ENABLE_REMOTE_DEMO=ON`. The page is 1040 px wide,
  which is the mock's window without its 240 px sidebar.

| | Mock | App |
| --- | --- | --- |
| Rows | [mock-rows.png](mock-rows.png) | [app-rows-light.png](app-rows-light.png), [dark](app-rows-dark.png) |
| SSH first-use host key | [mock-ssh-hostkey.png](mock-ssh-hostkey.png) | [app-ssh-hostkey.png](app-ssh-hostkey.png) |
| Relay pairing | [mock-relay.png](mock-relay.png) | [app-relay.png](app-relay.png) |
| Details | [mock-details.png](mock-details.png) | [app-details.png](app-details.png) |

Known differences:

- The app's error row also prints what to do under the detail line. The
  mock's card shows only the badge.
- The app's Details card is 540 px wide instead of 500 px. At the app's font
  metrics, the full SHA256 key needs that width to stay on one line.
