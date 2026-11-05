# Licensing and distribution

This document records the license boundary planned for KetPlus, the licenses of
the dependencies currently used by the development tree, and the work required
before distributing official binaries. It is an engineering compliance record,
not legal advice.

## Product boundary

The repository and licensing split is (ADR 0029):

| Repository or product | Visibility | License |
| --- | --- | --- |
| `ketplus` / KétPlus (Lite) and KétPlus Pro | Public | MPL 2.0 |
| `ketplus-app` | Private | Proprietary |
| Ketsuite cloud services (relay, gateway, accounts, KétRouter) | Private | Proprietary |

The Lite edition contains only `core/`: the native editor, workspace, Git
integration, terminal, and local document preview. Pro adds Agent Workspace,
model-provider adapters, Atlas, the daemon, and the relay client. Until ADR 0029
Pro was the proprietary `ketplus-desktop` repository; the copyright holder
relicensed that source under MPL 2.0 when importing it here.

MPL 2.0 applies at file level. Files that are not MPL-covered state their own
license, such as the MIT agent skills below and third-party license texts. A fork may combine this source with
proprietary files, but a shipped modification to an MPL-covered file must stay
available under MPL 2.0.

Reference: https://www.mozilla.org/en-US/MPL/2.0/FAQ/

## Direct dependencies

The following components are compiled, linked, or embedded into the current
application.

| Component | Pinned version | Integration | License | Distribution requirement |
| --- | --- | --- | --- | --- |
| Qt | 6.11 required; 6.11.1 audited locally | Dynamic frameworks/libraries | LGPL 3.0, GPL alternatives, or commercial | Follow the selected Qt license and include Qt and third-party notices |
| Scintilla | `rel-5-5-2`, commit `a1c86144eed9e3d2187e3a8b391d11ca909f00d2` | Compiled statically | Scintilla permissive license | Preserve its copyright and permission notice in copies and supporting documentation |
| Lexilla | `rel-5-5-2`, commit `3e6f317eed854bc312b4c4453206189d381c7ae7` | Compiled statically | Scintilla permissive license | Preserve its copyright and permission notice in copies and supporting documentation |
| libvterm | `v0.3.3`, commit `9d6d2112335080312ef8c36667fa717ded4f7daf` | Compiled statically | MIT | Include its copyright and license text |
| Inter | 4.1 archive pinned by SHA-256 | Font embedded in Qt resources | SIL OFL 1.1 | Include the copyright and complete OFL text in an easily accessible form |
| KÉT Sans | 1.0.0 official ZIP pinned by SHA-256 | Four unmodified hinted TTF faces fetched at build time and embedded in Pro Qt resources | KÉT Sans Font License 1.0, proprietary | App embedding only; no loose font redistribution; ship the complete license |
| Két Design System | 0.1.41 Mermaid snapshots in CM `cm-v0.3.6` | Embedded configuration data | MIT | Include the publisher notice in CM Mermaid resources |
| Lucide Static | 1.40.0 assets | Vendored SVG path data | ISC, plus Feather MIT for derived icons | Include both applicable notices |
| Sparkle | 2.10.0 release archive pinned by SHA-256 | Unmodified framework in `Contents/Frameworks`, only when `KETPLUS_UPDATE_FEED_URL` is set (ADR 0019) | MIT, plus the bsdiff and other notices in its `LICENSE` | Ship `Sparkle-MIT.txt` complete |

Scintilla and Lexilla license:
https://www.scintilla.org/License.txt

libvterm license:
https://github.com/neovim/libvterm/blob/v0.3.3/LICENSE

Inter license and reserved font name information:
https://github.com/rsms/inter

Lucide license, including the Feather-derived icon list:
https://github.com/lucide-icons/lucide/blob/main/LICENSE

### Daemon WebRTC dependencies

`cmake/WebRtc.cmake` pins libdatachannel 0.24.6 (MPL-2.0), libjuice
`cb523763255f241442da1fbdd72c498f33a54b35` (MPL-2.0, the upstream zero-tiebreaker
fix), usrsctp `fec583d54493f879d2ae44a743423bf8a04371ab` (BSD-3-Clause) and
plog `94899e0b926ac1b0f4750bfbd495167b4a6ae9ef` (MIT). They are statically linked
into the daemon; upstream source is unmodified. Media, SRTP, upstream
WebSockets, examples and tests are disabled. Qt still owns the relay socket.
OpenSSL Crypto and SSL are dynamic SDK dependencies (Apache-2.0 for supported
OpenSSL 3+), and must be bundled/audited alongside Qt for a redistributable app.
Source URLs and complete notices ship in `THIRD_PARTY_NOTICES.md` and
`third_party/licenses`. Upstream development headers/libraries are excluded from
the product install. This adds no code to `core/`.

### Qt modules

The current CMake configuration references these Qt modules:

```text
Core, Gui, Widgets, Network, WebSockets, Sql, Concurrent,
Core5Compat, Svg, SvgWidgets,
Quick, Qml, QuickControls2, WebView
```

`Qt6::Test` is used only by test executables and is not intended to ship in the
application bundle.

For both editions, the initial distribution
plan is to use Qt under LGPL 3.0 and link it dynamically. Before release, the
distribution process must:

1. Bundle or otherwise resolve the required dynamic Qt libraries without
   relying on developer-specific Homebrew paths.
2. Include LGPL 3.0 and GPL 3.0 license texts, Qt notices, and notices for the
   third-party components actually shipped inside Qt.
3. Provide the complete corresponding Qt source used for the build, including
   applied modifications, or a compliant written offer under Ketsuite's
   control. A link controlled only by the Qt Project is not sufficient for the
   planned compliance process.
4. Permit replacement and relinking of the Qt libraries. Any end-user terms
   must preserve the LGPL exception for reverse engineering needed to debug
   such modifications.
5. Record the exact Qt build, patches, build configuration, deployed plugins,
   and transitive libraries in the release SBOM.

Qt's official guidance on the LGPL option, including dynamic linking:
https://www.qt.io/development/open-source-lgpl-obligations

Qt module and third-party license inventory:
https://doc.qt.io/qt-6/licenses-used-in-qt.html

The current macOS development executable links dynamically but points to
frameworks below `/opt/homebrew/opt`. Its application bundle contains no Qt
frameworks and is not a distributable release artifact.

Qt WebView uses a native system web view on macOS. On Linux it depends on Qt
WebEngine, which brings Chromium and a much larger third-party license set. The
Lite build omits Atlas, WebView, Quick/QML, and AI modules unless a `core/`
feature requires them. If a Linux product build ships Qt WebEngine, its
Chromium notices must be generated from the exact deployed build.

Qt WebView licensing:
https://doc.qt.io/qt-6/qtwebview-index.html

Qt WebEngine licensing:
https://doc.qt.io/qt-6/qtwebengine-licensing.html

## External tools

The application currently discovers or invokes the following separately
installed executables. They are not linked or bundled into KetPlus, so their
licenses do not change the KetPlus source license.

| Tool | Current use | License |
| --- | --- | --- |
| Git CLI | Repository status, diffs, and worktrees | GPL 2.0 |
| Codex CLI | AI app-server process | Apache 2.0 |
| Cursor CLI (`agent`) | AI ACP process | Anysphere proprietary terms; not redistributed. https://cursor.com/terms-of-service |
| Mermaid CLI (`mmdc`) | Optional diagram rendering | MIT |
| KetAtlas CLI | Optional Atlas process | MIT |
| Node.js and npm | Runtime used by optional CLI tools (the browser runtime bundles its own Node; Claude reuses that binary, see below) | Separate upstream licenses |
| Google Chrome, Microsoft Edge | Browser driven by the bundled browser runtime | Vendor terms |

Calling a user-installed executable across a process boundary is different from
redistributing it. If an official installer later bundles any of these tools,
the tool and all content in its binary or package must be audited again and its
required notices and sources must be included. In particular, bundling Mermaid
CLI may also bundle Puppeteer or Chromium.

Git licensing: https://git-scm.com/about

Codex licensing: https://github.com/openai/codex/blob/main/docs/license.md

Mermaid CLI: https://github.com/mermaid-js/mermaid-cli

### Bundled browser runtime

`apps/browser` is not linked into KetPlus, but macOS Release bundles built with
`KETPLUS_BUNDLE_BROWSER_RUNTIME` (default ON) redistribute it as a separate
helper process in `KetPlus.app/Contents/Resources/ketplus-browser`
(`decisions/0009-bundled-browser-runtime.md`). That directory contains:

| Component | Pinned version | Integration | License | Distribution requirement |
| --- | --- | --- | --- | --- |
| Node.js | v24.21.0 (sha256 pinned in `apps/browser/runtime/node-runtime.json`) | Unmodified executable, separate process | MIT plus the third-party licenses in Node's `LICENSE` | Ship Node's complete `LICENSE` |
| playwright-core | 1.63.0 | JavaScript package | Apache-2.0 | Ship the license, `NOTICE` and `ThirdPartyNotices.txt` |
| @modelcontextprotocol/server, core | 2.0.0 | JavaScript packages | MIT | Ship license texts |
| pixelmatch | 7.2.0 | JavaScript package | ISC | Ship license text |
| pngjs | 7.0.0 | JavaScript package | MIT | Ship license text |
| Zod | 4.6.5 | JavaScript package | MIT | Ship license text |

`apps/browser/scripts/package-runtime.mjs` copies every production package's
license and notice files into `ketplus-browser/licenses/` and generates
`ketplus-browser/THIRD_PARTY_NOTICES.md`, so a dependency update cannot silently
drop a notice. The `playwright` package (test runner and CLI) and the MCP client
SDK 2.0.0 (MIT) are development-only and are not bundled. The browser server
source and skill are MPL 2.0 with the rest of the repository.

### Default agent skills

`assets/agent-skills` is copied to `KetPlus.app/Contents/Resources/agent-skills`
(ADR 0021). `ket-atlas`, `enterprise-grade` and `ket-design-system` are
© KETVIET JSC under MIT; each ships with its `LICENSE`.

### Bundled Claude Agent SDK sidecar

Every macOS build copies
`apps/daemon/providers/claude-sidecar` production dependencies into
`KetPlus.app/Contents/Resources/claude-sidecar`. That tree includes the
Claude Agent SDK and its platform CLI (`@anthropic-ai/claude-agent-sdk`,
© Anthropic PBC; use is subject to
[Anthropic's legal agreements](https://code.claude.com/docs/en/legal-and-compliance)).
Node is not duplicated: the daemon executes `index.mjs` with
`Contents/Resources/ketplus-browser/bin/node`. The native `claude` CLI binary
inside `node_modules` is nested Mach-O and must be signed inside-out with the
app for notarized releases; `VerifyBundle.cmake` checks every Mach-O file in
the sidecar tree when it verifies signatures.

No browser is redistributed. The runtime drives the user's installed Google
Chrome or Microsoft Edge, and otherwise downloads Chrome for Testing through
Playwright's registry into the user's data directory
(`Ketsuite/KetPlus/browser-runtime/chrome`). Chrome for Testing is subject to
Google's terms and Chromium's notices as downloaded by the user's machine; a
future build that ships any browser binary must add that exact build's notices
and source obligations before the release gate can pass. Source checkouts keep
using `npm run install:browser` for Playwright's own Chromium in the user cache.

## Known gaps

The following gaps exist in the current tree and packaging configuration:

- Exact Qt and deployed Qt third-party notices are not generated yet; these vary
  by operating system, plugins, and the Qt build used for packaging.
- Pro Windows CI is deferred until the required Qt 6.11 WebView development
  packages are available from Qt's public Windows repository.
- License files are installed by CMake but there is no About dialog entry that
  makes them directly accessible from the running application.
- The referenced libvterm repository is archived. This does not change its MIT
  license, but upgrades and security maintenance need an explicit upstream or
  maintained fork.
- No release SBOM or automated license-policy check is currently generated.

## Required distribution files

Each official binary distribution should expose at least:

```text
licenses/
  KetPlus-MPL-2.0.txt
  Qt-LGPL-3.0.txt
  GPL-3.0.txt
  Qt-Third-Party-Notices.txt
  Scintilla.txt
  Lexilla.txt
  libvterm-MIT.txt
  Inter-OFL-1.1.txt
  Lucide-ISC-and-Feather-MIT.txt
THIRD_PARTY_NOTICES.md
SBOM.spdx.json
```

The About dialog of each edition should identify the source revision used by
the build and link to the corresponding public source. It should also
provide access to the installed license and notice files.

## Release gates

No public source push or official binary release should proceed until all
applicable items are complete:

- [ ] Public history contains only source and assets reviewed for publication.
- [ ] License and source headers are consistently MPL 2.0, except files that
      state their own license.
- [ ] No credentials, signing material, or transcripts are committed.
- [ ] Every bundled library, plugin, font, icon, and executable is represented
      in the generated SBOM.
- [ ] License texts and notices are copied into the installed application.
- [ ] The About dialog exposes notices and the source revision.
- [ ] Qt is dynamically linked and deployed from a controlled, reproducible
      release toolchain.
- [ ] The matching Qt source archive, patches, and build information are hosted
      or offered under Ketsuite's control.
- [ ] No end-user terms restrict rights required by MPL or LGPL.
- [ ] macOS, Windows, and Linux release artifacts are audited independently,
      because their Qt plugins and transitive dependencies differ.
- [ ] A final legal review is completed before the first commercial release.


### Claude BYOK

The API key feature in [ADR 0026](decisions/0026-claude-api-key.md) uses each user's
own Anthropic account and keeps the upstream CLI binary unmodified. It does not
resell Claude Code usage or change the existing runtime authentication options.
Review Anthropic's current [integration conditions](https://code.claude.com/docs/en/legal-and-compliance)
before release; SDK cost estimates must not be used to bill KétPlus users.
