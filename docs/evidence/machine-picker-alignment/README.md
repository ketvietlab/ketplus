# Narrow machine picker and signed-out Connections

These are original offscreen QWidget captures from the production ActivityBar and
RemoteControlView, using temporary INI settings and synthetic machine data. No
installed application, user workspace or provider is opened for the geometry captures.

- `picker-240-{light,dark}.png`: 240×760 sidebar, long synthetic machine name,
  Reconnecting state. The kit Select keeps the complete state at the start and
  elides the remaining label within its recipe text rectangle. Its full control
  geometry remains inside its parent. The state text itself includes an ellipsis.
- `picker-240-open-{light,dark}.png`: the popup shows the complete label.
- `signed-out-alignment-{light,dark}.png`: 1100×900 Connections view. The Linux
  heading and introduction share the 24 px inset of the centered page body
  (69 px from the window at this width). The same geometry is checked at 760 px.

Reproduce with `QT_QPA_PLATFORM=offscreen`,
`KETPLUS_E2E_SCREENSHOTS=<scratch>` and the machine end-to-end test function
`pickerPreservesStateAtDefaultSidebarWidth`; use
`KETPLUS_CONNECTIONS_CAPTURE=<scratch>` with the remote-workspace test function
`signedOutIntroFollowsTheMockAtEveryWidth` for Connections. The picker test checks
all four connection states in both themes and restores Light after each row.
