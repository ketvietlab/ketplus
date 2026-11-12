# Linux machines in Connections (#419)

Side by side of the mock and the app for the Linux rows, the "Connect a Linux
machine" dialog and a machine's Details (ADR 0119 states). Both were rendered
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

- The app's error row also prints what to do under the detail line, with the
  command on a line of its own so it never wraps at its hyphen. The mock's
  card shows only the badge.
- The app's Details card is 540 px wide instead of 500 px. At the app's font
  metrics, the full SHA256 key needs that width to stay on one line.
- The rows captures above set `LinuxMachineModel::setBackendsAttached(true)`
  on a hand-written model. The wired captures below come from the real
  controllers.

## Wired backends (T-13)

The same capture run also renders Connections with the real controllers
attached. `LinuxSshMachines` drives `SshMachineBackend` over a fake link to a
fake daemon (`tests/daemon/FakeSshLink.h`), connected from the connect card.
`LinuxRelayMachines` drives a fake Relay link (`tests/FakeRelayLink.h`) for a
host that the Relay lists with its pinned key, last seen 9 minutes ago.

| | Mock | App |
| --- | --- | --- |
| Rows | [mock-rows.png](mock-rows.png) | [app-wired-rows-light.png](app-wired-rows-light.png), [dark](app-wired-rows-dark.png) |
| Relay Details | [mock-details.png](mock-details.png) | [app-wired-relay-details.png](app-wired-relay-details.png) |
| SSH Details with Remove | [mock-details.png](mock-details.png) | [app-wired-ssh-details.png](app-wired-ssh-details.png) |

Differences from the mock:

- A Relay host that has not been seen for more than two minutes reads "Host
  offline" with "last seen …" under its detail line, and its Details says that
  requests wait for the host for up to 15 minutes. The mock has no presence.
- Details names the daemon's key "Host key" on both routes. On the Relay it is
  the key pinned at pairing, shown as Verified.
- Remove appears only for SSH machines.
