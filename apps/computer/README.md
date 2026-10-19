# KetPlus Computer (ADR 0026 spike)

`KetPlus Computer.app` is the only KetPlus process that captures the screen or
posts input, so macOS attributes Screen Recording and Accessibility to this
bundle (`dev.ketplus.computer`). `ketplus-computer-mcp`, inside the same bundle,
is a stdio MCP server that providers launch. It forwards each tool call to the
agent app and never captures or clicks itself.

```text
provider MCP client ── stdio ──> ketplus-computer-mcp
                                   │ computer.v0, newline JSON, per-launch token
                                   ▼ ~/Library/Application Support/Ketsuite/KetPlus/computer/agent.sock (0700 dir)
                                 KetPlus Computer.app (started with `open -g -a`, parent launchd)
                                   ├─ ScreenCaptureKit: main display, KetPlus windows excluded
                                   └─ CGEvent: left click
```

Spike tools: `screenshot` and `left_click` (`coordinate: [x, y]` in the last
screenshot's pixels). The names follow `computer_toolset_20260801`.

## Build and check

```sh
cd apps/computer
swift test
scripts/build-app.sh --identity <codesigning SHA-1>   # → dist/KetPlus Computer.app
"dist/KetPlus Computer.app/Contents/MacOS/ketplus-computer-mcp" --status
node scripts/smoke-mcp.mjs "dist/KetPlus Computer.app" --out /tmp/shot.png [--click x,y]
```

Sign with a real identity. TCC keys grants to the designated requirement
(bundle ID and team), so they survive rebuilds. An ad-hoc signature resets them.
The smoke script uses no provider quota. `--click` clicks the real screen, so
pick a harmless point.

To try it from Claude Code (this uses your Claude quota):

```sh
claude mcp add ketplus_computer -- "$PWD/dist/KetPlus Computer.app/Contents/MacOS/ketplus-computer-mcp"
```

Not in the spike: per-app grants, tiers, window filtering beyond excluding
KetPlus, Esc kill switch, control lock, daemon-issued per-conversation tokens,
daemon/sidecar injection, and CMake bundling into `KetPlus.app`.
