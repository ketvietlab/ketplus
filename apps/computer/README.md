# KetPlus Computer

`KetPlus Computer.app` (`dev.ketplus.computer`) is the only KetPlus process that
captures the screen or posts input, so macOS attributes Screen Recording and
Accessibility to it. It also enforces who may control what (ADR 0026). Input runs
in the background, so the user keeps their pointer and front app (ADR 0027).
Granted apps move to a work display, a secondary display by default (ADR 0028).
`ketplus-computer-mcp`, inside the same bundle, is the stdio MCP server that
providers launch. It never captures or clicks itself.

```text
provider ── stdio ──> ketplus-computer-mcp   (env: KETPLUS_COMPUTER_CONVERSATION / _TOKEN from the daemon)
                        │ computer.v0: newline JSON, conversation + HMAC token
                        ▼ <KetPlus data>/computer/agent.sock   (0700 directory, peer UID checked)
                      KetPlus Computer.app   (opened with `open -g -a`, parent launchd)
                        ├─ grants per conversation and app (native approval panel)
                        ├─ tiers: blocked / view_only / click_only / full, plus the user's deniedApps
                        ├─ ScreenCaptureKit: only granted apps; KetPlus, own panels and menu bar excluded
                        ├─ background: accessibility observe/act and clicks, keyboard events to one process,
                        │   overlay pointer; granted apps work behind other windows
                        ├─ foreground (opt-in): HID events, checked against the topmost window or frontmost app
                        └─ one control lease, Stop button and Esc, banner, audit log
```

Tools follow `computer_toolset_20260801`: `screenshot`, `zoom`, `left_click`,
`right_click`, `middle_click`, `double_click`, `triple_click`,
`left_click_drag`, `mouse_move`, `scroll`, `type`, `key`, `cursor_position` and
`wait` (60 s maximum). KetPlus adds `list_apps`, `request_access`,
`open_application`, `observe` and `act`. `left_mouse_down`, `left_mouse_up` and
`hold_key` are not implemented.

`left_click`, `right_click`, `scroll`, `type` and `key` run in the background
unless `"foreground": true`. The other pointer tools need it and otherwise
answer `NEEDS_FOREGROUND`.

`screenshot` captures the work display unless `display` is `main` or an ID from
`list_apps`. The work display comes from `display` in `computer-tools.json`
(`secondary` by default, or `main`); with one display it is the main display.

### Local MCP clients

An agent outside KetPlus, such as a terminal coding agent, can use the same tools
with `ketplus-computer-mcp --local-client <name>`. The front mints the token from
`host.key` itself and uses the conversation `local:<name>`, so its grants, lease
and audit stay apart from daemon conversations. It still needs the approval panel
for every app. This adds no access: `host.key` is 0600 and any process of the same
user could already read it. Without a host key (KetPlus never opened) it exits
with `UNAUTHORIZED`. For example:

```sh
claude mcp add ketplus-computer -- "<KetPlus.app>/Contents/Helpers/KetPlus Computer.app/Contents/MacOS/ketplus-computer-mcp" --local-client claude-code
```

Files in `<KetPlus data>/computer` (`KETPLUS_COMPUTER_DIR`):

- `host.key`: 0600, created by the daemon.
- `agent.sock`: the agent's socket.
- `remote-turns/<stem>`: present while a conversation's turn was started remotely.
- `audit/<stem>.jsonl`: one line per action, with no images and no typed text.

`<stem>` is the first 32 hex digits of SHA-256 of the conversation ID.

## Build and check

```sh
cd apps/computer
swift test
scripts/build-app.sh --identity <codesigning SHA-1>   # → dist/KetPlus Computer.app
```

The desktop CMake build runs the same script into `KetPlus.app/Contents/Helpers`
(`KETPLUS_COMPUTER_SIGN_IDENTITY`). TCC keys grants to the designated
requirement (bundle ID and team), so a real identity keeps them across rebuilds.
An ad-hoc signature resets them.

`swift build -c release && .build/release/KetPlusComputer --render-previews <dir>`
writes the approval alert, the control banner and the overlay pointer as PNGs
in light and dark appearance. It needs no TCC grant.

### End-to-end on a Mac

This check uses no provider quota. Its background part covers the fixture with
`fixture-cover` and must not move the pointer; its foreground part moves and
clicks the real pointer. Do not type or use the mouse while it runs.

```sh
scripts/build-app.sh --testing --identity <sha1>   # adds KETPLUS_COMPUTER_TEST_APPROVAL (test builds only)
scripts/build-fixture.sh
node scripts/e2e.mjs "dist/KetPlus Computer.app" "dist/KetPlus Computer Fixture.app" [--manual-esc]
```

To check the real approval panel, run against a release build, for example the
one inside the desktop build's `KetPlus.app/Contents/Helpers`, and answer the
panel when asked (Deny, Esc, Allow):

```sh
node scripts/approval-check.mjs "<KetPlus.app>/Contents/Helpers/KetPlus Computer.app" "dist/KetPlus Computer Fixture.app"
```

The test build must already hold both TCC grants. With `--manual-esc`, the
script asks you to press Esc and then click Stop on the banner. Synthetic events
from a shell without Accessibility never reach the kill switch, so only real
input verifies them.
