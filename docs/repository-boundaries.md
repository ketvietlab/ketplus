# Repository boundaries

## Ownership decision

This repository is public under MPL 2.0 and builds two editions (ADR 0029):
KétPlus (`KETPLUS_EDITION=lite`, the editor alone) and KétPlus Pro (`pro`).
Place code according to which edition needs it:

| Capability | Location | Edition |
| --- | --- | --- |
| Editor, documents, workspace tree, Git, terminal, preview, shared theme/settings UI | `core/` | Lite and Pro |
| Desktop composition and product-specific navigation | `apps/desktop` | Pro |
| AI provider transport and Agent Workspace | `modules/ai` | Pro |
| Atlas process, WebView, and index | `modules/atlas` | Pro |
| Local/remote host session control plane and Linux headless CLI | `apps/daemon` | Pro; the headless daemon also builds alone |
| Local IPC and Qt Control v2 remote-client backend | `modules/remote` | Pro |
| Canonical browser server (browser.v0) | `apps/browser` | Pro; bundled as a separate helper process, never linked |
| Provider browser-tool launch settings and runtime discovery | `modules/browser` | Pro; Qt Core only |

These stay outside this repository and are proprietary:

| Capability | Repository |
| --- | --- |
| Cloud control, relay, gateway configuration, accounts, entitlements, and KétRouter service | `ketviet` or dedicated service repositories |
| iOS and Android remote client | `ketplus-app` |

If a feature is useful in the editor alone, it belongs in `core/` so Lite ships
it. Agent, Atlas, daemon and remote-session behavior stays out of `core/`. Code
in this repository is public: its security must never depend on the source or
protocol staying private, and it holds no credentials, service secrets, signing
material, or user transcripts. Paid features are enforced by the hosted
services, not by this code.

`apps/browser` shares the repository only. It must not link into Desktop, the
daemon, `core/` targets, or product packaging without a separate architecture,
security, dependency, and licensing decision. Its protocol accepts ordinary URLs
and does not own Atlas manifests or viewer behavior.

Exception (`decisions/0008-codex-browser-tools-stage-0.md`): unless the user
turns it off under Settings → Browser, Desktop and the daemon launch the browser
MCP server for Codex as an external process — the bundled runtime when present,
otherwise `apps/browser/src/mcp.mjs` from a source checkout. Nothing is linked;
`modules/browser` only builds launch arguments and reloads them on settings
changes. The origin policy (default all origins) comes from user settings and is
enforced by the browser process, which always uses an isolated temporary
profile. Bundle packaging rules for the runtime are owned by the bundled-runtime
work; ADR 0011 extends the same runtime and settings to Claude SDK and Cursor ACP
without a new broker. Their adapters receive launch configuration on each turn.

Bundling (`decisions/0009-bundled-browser-runtime.md`): macOS Release bundles
copy a staged runtime (pinned Node, `apps/browser` source, production
`node_modules`, the skill, notices, `runtime.json`) into
`KetPlus.app/Contents/Resources/ketplus-browser`. It is still a separate process
started over stdio; Desktop, the daemon and CM targets do not link or embed
JavaScript. `modules/browser/src/BundledBrowserRuntime.*` only validates the
manifest and builds a `ToolLaunch`. The bundle contains no browser binary. Any
new bundled executable or dependency needs the audit in `licensing.md`.

## Core dependency contract

`core/` must build and run without any Pro directory: the Lite edition adds only
`core/`. Pro adds it with `add_subdirectory(core EXCLUDE_FROM_ALL)`, with the
Lite executable and core tests disabled, and links exported `KetPlusCM::*`
targets.

Allowed:

```cmake
target_link_libraries(pro_target PRIVATE KetPlusCM::editor)
```

Not allowed:

```cpp
#include "../../../core/src/editor/InternalHeader.h"
```

Do not add `KETPLUS_EDITION` checks inside `core/`. When Pro needs different
editor behavior, add a `core/` extension point and implement the Pro side in its
own module. A change to both `core/` and a Pro module lands in one pull request.

Keep reusable protocol/value types in the narrowest module that owns their wire
contract. Desktop widgets must not become protocol owners. The daemon must not
depend on Qt Widgets, QML, or the desktop executable.

## License boundary

The whole repository is MPL 2.0, applied at file level. Re-run the dependency
and distribution audit in `licensing.md` whenever adding a library, bundling an
external tool, or changing static/dynamic linkage.
