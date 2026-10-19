# KetPlus Computer

`KetPlus Computer.app` (`dev.ketplus.computer`) is the only KetPlus process that
captures the screen or posts input, so macOS attributes Screen Recording and
Accessibility to it. It also enforces who may control what (ADR 0026).
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
                        ├─ CGEvent input, checked against the app under the pointer or the frontmost app
                        └─ one control lease, Esc kill switch, banner, audit log
```

Tools follow `computer_toolset_20260801`: `screenshot`, `zoom`, `left_click`,
`right_click`, `middle_click`, `double_click`, `triple_click`,
`left_click_drag`, `mouse_move`, `scroll`, `type`, `key`, `cursor_position` and
`wait` (60 s maximum). KetPlus adds `list_apps`, `request_access` and
`open_application`. `left_mouse_down`, `left_mouse_up` and `hold_key` are not
implemented.

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
writes the approval alert and the control banner as PNGs in light and dark
appearance. It needs no TCC grant.

### End-to-end on a Mac

This check moves and clicks the real pointer and uses no provider quota:

```sh
scripts/build-app.sh --testing --identity <sha1>   # adds KETPLUS_COMPUTER_TEST_APPROVAL (test builds only)
scripts/build-fixture.sh
node scripts/e2e.mjs "dist/KetPlus Computer.app" "dist/KetPlus Computer Fixture.app" [--manual-esc]
```

The test build must already hold both TCC grants. With `--manual-esc`, the
script asks you to press Esc. Synthetic key events from a shell without
Accessibility never reach the kill switch, so only a real key press verifies it.
