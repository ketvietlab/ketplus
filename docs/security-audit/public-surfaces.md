# Public surfaces and sandbox audit

Audit date: 2026-11-16. Task: T-2. Audited integration commit:
`7c12c773372b1d63eae867b7a6f8dcfefb6cdbbb`.

This is an offline source audit, not release approval. Only this report changes;
product fixes, publication-history decisions and artifact acceptance belong to
separate tasks. No provider requests, external service probes, browser launches,
screen captures, input control, signing or deployment were performed.

## Decision and scope

Two High and four Medium findings require disposition before the applicable
public release. High means a significant executable-content or publication
boundary failure under the stated conditions. Medium means a conditional code
execution/supply-chain risk or an incomplete distribution gate. These are audit
priorities, not CVSS scores; no remotely unauthenticated exploit was demonstrated.

Read: root `AGENTS.md`, `docs/project-status.md`, `repository-boundaries.md`,
`architecture.md`, `development.md`, `testing.md`, `product-architecture.md`,
`licensing.md`, `release-process.md`, `public-tree-hygiene.md`, and relevant
browser, Computer, Atlas readiness and update decisions/contracts.

| Surface | Source coverage and observed controls | Verification limit |
| --- | --- | --- |
| `apps/browser` | Origin normalization/narrowing, DNS/address classification and pinning proxy, redirects/CONNECT/WebSockets, request API, action schemas and snapshot fencing, redaction, artifact paths, engine download and pinned launch defaults | Proxy unit tests ran; Chromium-dependent suites did not. Offline dependency inspection confirms sandbox-disabling launch arguments (PS-06); no running browser was inspected. |
| `modules/browser` | Settings fail closed when damaged; host launch arguments, runtime canonical-path containment, private artifacts, Node preload/download-host neutralization, routing and cleanup signals | No provider process or live adapter wiring exercised. Default host setting permits all origins; it is a separate capability from provider filesystem sandboxing. |
| `apps/computer`, `modules/computer` | Host key/token format, environment binding, peer UID, private socket, remote markers, app tiers/grants, lease/Esc state, capture filtering, process launch | Protocol tests and real server dispatch with no-op tools; no TCC/AX/HID or permission dialogs exercised. |
| `modules/atlas` | Manifest bounds/canonical worktree fencing, local screen paths/thumbnails, process arguments, selected-port readiness, managed renderer launch, WebView/JavaScript state handling | External KetAtlas renderer implementation was not audited or executed. WebView navigation is not covered by the browser worker's origin proxy. |
| `core` | Git read/mutation arguments and process environment, revision reads, workspace indexing, Markdown links/resources, Mermaid invocation/configuration, terminal launch | Harmless Git helper reproduction; no C++ build or native editor run. Local resources remain readable by the desktop user. |
| `apps/desktop` | Transcript link classification/confirmation, Atlas entry points, worktree launch, Computer settings, Sparkle adapter and compiled-feed policy | Static review plus packaging fixtures; no signed bundle or live update acceptance. |
| Build/package/dependencies | CMake pins, browser lock/runtime packaging, Sparkle pin, macOS verification/signing/update scripts, Linux package/SBOM generation, licensing gates and public-tree CI | No dependency download, online vulnerability database lookup, full native build, legal review or actual distribution artifact inventory. |

## Findings

### PS-01 — High: opening an Atlas implicitly authorizes its renderer command

- **Location:** `modules/atlas/src/atlas/ManagedAtlasViewers.cpp:105–110`;
  `apps/desktop/src/app/AtlasConversationPanel.cpp:193–198`;
  `apps/desktop/src/app/MainWindowAtlasResults.cpp:39`.
- **Condition:** a user opens an untrusted or agent-modified bundle containing
  `atlas.renderer.json`, with a compatible KetAtlas CLI installed. The UI action
  is labelled “Open in browser”; no renderer-command trust check exists along
  these call paths.
- **Offline evidence:** `open()` appends `--renderer` solely because the sidecar
  exists, then starts the CLI in the bundle directory with the host environment.
  The public renderer contract at
  `assets/agent-skills/ket-atlas/SKILL.md:64–86` explicitly defines an argument
  array/cwd and says this flag executes that local command. This establishes the
  host's implicit opt-in by source inspection; an actual malicious renderer was
  not executed.
- **Impact:** viewing a bundle can run its declared program with desktop-user
  filesystem/network access, outside the provider's selected sandbox and approval
  broker. Shell-string injection is unnecessary: an argument array can directly
  name an interpreter. Plain bundle discovery alone was not shown to execute it.
- **Remediation:** require explicit renderer trust before adding the flag, showing
  the resolved executable, arguments and cwd; bind approval to canonical bundle
  identity and contract contents, invalidate it on change, and recheck just before
  launch. Keep static viewing possible without authorizing a renderer. Add a fake
  CLI regression proving an untrusted sidecar cannot cause automatic opt-in.

### PS-02 — High: a clean current tree does not make the available history public-safe

- **Location:** `docs/public-tree-hygiene.md:3–12`;
  `tests/hygiene/test_public_tree.py:65–82`;
  `.github/workflows/public-tree-hygiene.yml:17–20`.
- **Condition:** publication includes the audited commit's existing ancestry or
  other local refs, rather than a separately approved sanitized history.
- **Offline evidence:** the current tracked-tree guard passed all four tests.
  Applying its same redacting `violations()` rules to text blobs and commit
  objects reachable from the exact audit commit still flags historical private
  ownership/source/infrastructure metadata. The document explicitly states that
  history was not rewritten. A concrete public-repository object is blob
  `85ec0396ef199ec3dacd989da35f5825fee286d2`, historical
  `docs/repository-boundaries.md:26`, rule `private repository`; its matched value
  is deliberately omitted. This is a public Git object ID, not a private upstream
  revision. Counts below are rule matches requiring disposition, not a count of
  distinct confirmed disclosures.

  | Reachable from audit commit | Objects inspected | Flagged objects by boundary rule |
  | --- | ---: | --- |
  | Commits | 1,754 | Private repository: 5; deployment address: 2 |
  | Blobs | 11,041 | Private repository: 436; private module: 20; private source path: 265; deployment address: 4 |

  Categories overlap. Binary/non-UTF-8 content is excluded from this boundary
  text check; trees were traversed but not interpreted as text. A broader scan
  of then-available local refs covered 1,950 commits and 11,582 blobs; this is
  additional local coverage, not a claim those refs will all be published.
- **Impact:** deleting current files leaves forbidden metadata retrievable through
  Git ancestry. The existing PR guard examines `git ls-files`, not history or
  commit messages, so its success does not satisfy the publication-history gate
  in `docs/licensing.md:280`.
- **Remediation:** leader must inventory the exact refs intended for publication,
  disposition historical matches privately, and choose a publication-safe history
  strategy with repository owners. Re-scan every intended ref, commit message and
  tag before publication. Do not paste matched values into public issues or
  rewrite shared history as part of this documentation task.

### PS-03 — Medium: automatic Git status can execute a repository-configured helper

- **Location:** `core/src/git/GitService.cpp:106–146`, `158–160`, `284–291`.
- **Condition:** the user opens an untrusted local repository whose `.git/config`
  is attacker-controlled, for example an extracted repository including Git
  metadata. An ordinary fresh clone does not copy a remote repository's local
  `.git/config`; tracked source files alone do not establish this condition.
- **Offline evidence:** `setWorkspacePath()` calls `refresh()` after discovery;
  `refresh()` runs `git status --porcelain=v2 -z --branch --untracked-files=all`
  without overriding `core.fsmonitor`. In an isolated temporary repository,
  setting that option to a harmless fixture executable and running the same
  status arguments created its marker and returned exit 0. System/global Git
  config was excluded from the fixture. No Qt application or malicious payload
  was needed to establish this Git behavior.
- **Impact:** a background operation presented as repository inspection can run
  a local command as the desktop user before an explicit Git mutation or terminal
  command. Provider turn sandbox settings do not constrain this host process.
- **Remediation:** establish workspace trust for executable repository behavior;
  before trust, override `core.fsmonitor=false` for automatic queries and review
  other Git helper surfaces such as textconv, external diffs and configured
  filters. Preserve explicitly authorized hooks on mutations through a deliberate
  policy. Add a temporary-repository marker test at the host integration boundary.

### PS-04 — Medium: first-use browser downloads have no independent executable integrity check

- **Location:** `apps/browser/src/engine-installer.mjs:13–15`, `31–48`;
  `apps/browser/src/engine.mjs:146–168`;
  `apps/browser/src/browser-service.mjs:90–94`.
- **Condition:** no usable system Chrome/Edge or managed cache exists, downloads
  are enabled (the default), and the upstream browser-distribution channel is
  compromised or returns substituted bytes. This is a supply-chain precondition,
  not an exploit available to an ordinary page.
- **Offline evidence:** the installer delegates to the pinned Playwright registry;
  its own integrity comment explicitly records HTTPS-only archive trust. Cached
  readiness checks executable/`INSTALLATION_COMPLETE` existence, not an expected
  artifact digest or publisher signature. Engine selection subsequently launches
  the returned executable. Contrast the bundled Node and Sparkle staging paths,
  which verify pinned SHA-256 values. No download or CDN tampering was attempted.
- **Impact:** a compromised download can become code executed with the browser
  worker's host-user privileges, beyond the origin policy's network protection.
- **Remediation:** verify each supported browser artifact against a maintained
  trusted digest/signature before extraction/launch, and validate the cache against
  that identity. Until available, a documented hardened deployment can require an
  installed browser and disable fallback downloads. Test substituted cached and
  downloaded artifacts with offline fixtures.

### PS-05 — Medium: binary publication evidence and dependency inventory are incomplete

- **Location:** `docs/licensing.md:233–246`, `275–295`;
  `packaging/linux/build-packages.py:117–146`.
- **Condition:** engineering packages or successful packaging checks are treated
  as completion of official distribution requirements.
- **Offline evidence:** licensing records missing exact Qt third-party notices,
  notice access in About, and release license-policy automation. The Linux SPDX
  generator does now exist, so the blanket “no release SBOM” sentence is stale;
  however its package list contains only KetPlus, Qt and AppImage runtime, with
  explicit comments retaining publication gates. It does not enumerate all
  bundled Node/npm/native libraries, fonts and other transitive components.
  Public source pins and npm integrity fields were reviewed; these do not prove
  that the final installed artifacts contain the required notices/source offers.
- **Impact:** a distributable can pass engineering checks while its component
  inventory and documented release evidence remain incomplete. This obstructs
  dependency incident response and the repository's own release acceptance; this
  report makes no independent legal-compliance determination.
- **Remediation:** generate per-artifact inventories from actual staged contents,
  reconcile dependencies/notices/source offers for each supported OS/architecture,
  expose notices/source identity as specified, and make the applicable release
  gate fail on missing evidence. Update the stale licensing status in a separate
  task. Do not equate fixture tests with signed-package acceptance.

### PS-06 — Medium: production browser launch disables Chromium's process sandbox

- **Location:** `apps/browser/src/browser-service.mjs:97–101`, `179–193`;
  `apps/browser/src/mcp.mjs:17–22`; pinned `playwright-core` 1.63.0 from
  `apps/browser/package-lock.json`, archive member `package/lib/coreBundle.js:43346–43347`.
- **Condition:** the production MCP worker uses the default browser launch options
  and loads hostile content that can exploit a browser-engine vulnerability.
  The worker does not set `chromiumSandbox=true`; the production caller supplies
  no override. No browser vulnerability or working exploit was demonstrated.
- **Offline evidence:** the locally cached dependency archive was verified against
  the lockfile's SHA-512 integrity before inspection; its SHA-256 is
  `208593d4e1bcd8f8fe5f869cad1cc332dc7f1d70dc1d58c102dc3ac36e30f26c`.
  The pinned Chromium argument builder adds `--no-sandbox` unless
  `chromiumSandbox` is explicitly `true`. Both launch branches in BrowserService
  pass through the unset option, so the production default selects that argument.
  This confirms the launch configuration without starting a browser; effective
  runtime behavior on supported OS/browser combinations was not separately tested.
- **Impact:** an engine compromise lacks Chromium's process-sandbox containment
  layer and can expose host-user resources without that layer's additional escape
  requirement. Origin filtering, DNS pinning and temporary profiles do not restore
  process isolation; provider filesystem sandbox settings do not establish it for
  this separately launched worker/browser. Other OS restrictions are not assessed
  by this finding.
- **Remediation:** in a separate product task, explicitly enable Chromium
  sandboxing for production launches, prevent launch overrides or arguments from
  silently disabling it, and fail closed when the required sandbox cannot start.
  Review supported system-browser and managed-browser paths and platform-specific
  prerequisites before rollout. Add offline regression coverage that captures
  launch options and exercises the pinned argument builder, asserting sandboxing
  is enabled and no sandbox-disabling arguments are generated; separately verify
  effective containment on supported runtime/platform combinations.

## Controls and remaining trust boundaries

- Browser sessions can narrow the host ceiling but not widen it. The proxy checks
  the resolved addresses and dials those same addresses without re-resolution;
  unit tests cover local/private/metadata and rebinding refusal. Contexts block
  service workers/downloads, use temporary profiles, and disable non-proxied
  WebRTC UDP. Node actions require fresh snapshots and verify element fingerprints.
  These network/action controls do not compensate for the disabled process sandbox
  confirmed in PS-06.
- Runtime manifest paths are canonicalized inside the runtime root
  (`modules/browser/src/BundledBrowserRuntime.cpp:19–30`); bundled launch neutralizes
  `NODE_OPTIONS` and download-host overrides (`:121–126`). Screenshot destinations
  use generated names; native comparison checks canonical containment, owner and
  private directory mode (`apps/browser/src/browser-service.mjs:948–960`). A
  compromised process running as the same user remains outside this boundary.
- Computer checks peer UID before dispatch and authenticates the conversation
  token before tools; remote markers refuse calls. HMAC verification, app tiers,
  grants and lease/Esc logic have focused protocol coverage. Tokens bind the
  conversation, not a turn or expiry. A no-op-tools harness built from the audited
  `AgentServer.swift`/ComputerProtocol returned `UNAUTHORIZED` for another
  conversation's token and `REMOTE_TURN` with its marker. The same harness accepted
  dispatch after `enabled=false`: the setting gates host launch, and ADR 0030
  explicitly promises next-turn changes (`:167–168`), not immediate token
  revocation. This is recorded as a capability-lifetime limit, not a demonstrated
  contract violation. An immediate-stop requirement would need server enforcement.
- Atlas manifest reads fence the canonical current worktree, reject manifest
  symlinks and cap size at 4 MiB. Embedded readiness binds the selected port
  (`modules/atlas/src/atlas/AtlasProcess.cpp:46–60`). The managed-viewer variant
  only checks `http` and `127.0.0.1` (`ManagedAtlasViewers.cpp:133–144`); it should
  receive equivalent selected-port binding in a follow-up. No port-hijack exploit
  was established. Its `QWebView` is a separate browsing surface without the
  browser worker's proxy; WebKit/WebEngine profile/navigation isolation and the
  external CLI's static/renderer serving boundaries remain unverified.
- Core Markdown uses `QTextBrowser`, not a JavaScript WebView; external link
  schemes are restricted in its click handler. Mermaid configuration locks
  `securityLevel=strict` and disables HTML labels. Process launch uses argument
  arrays for inspected macOS/Linux paths. Desktop transcript links reveal
  executable/app targets and confirm non-web/mail schemes and out-of-worktree
  viewer files. These checks do not attest image/SVG decoder safety or Windows
  `.cmd` shell quoting.
- Update configuration requires an HTTPS feed and matching public-key setting,
  pins Sparkle by SHA-256, and the adapter selects the compiled feed. Update
  preparation checks signatures, release identity, key-file containment and
  archive destinations. Production bundle verification checks daemon identity.
  Release keys, actual notarization/Gatekeeper results, archive signature rejection,
  installed library linkage and end-to-end updates were not verified here.

## Secrets and historical scan evidence

Gitleaks **8.30.1**, default rules, `--redact=100`, no custom configuration and
`--ignore-gitleaks-allow` were used. Reports were kept outside the repository and
only metadata was printed. The exact audited tree was materialized with
`git archive` for a tracked-only scan (2,332 tracked entries). A separate history
scan used `gitleaks git --log-opts=--all`; no remote refs were fetched.

| Scan | Scanner result | Triage |
| --- | --- | --- |
| Audited tracked snapshot | Exit 1; one `generic-api-key` candidate | `core/resources/mermaid/SOURCE.json:6`: source-content SHA-256 provenance, not a credential |
| Available Git history | Exit 1; five `generic-api-key` candidates across three paths | Three source-hash provenance occurrences; two UI measurement-label `key` properties in historical fixture evidence |

Historical source-hash locations: `core/resources/mermaid/SOURCE.json:9` in public
commits `460343c1560a` and `8f0ad4f10a04`, and `resources/mermaid/SOURCE.json:9`
in `8977d3992730`. The two measurement-label properties occur at historical
`ketplus-desktop-redesign.ketatlas/evidence/repository-picker/verification.json:441`
and `:737` in public commit `84e40d5b3760`; their JSON context is
`browser.measurements`, with `elements` siblings. Values are intentionally omitted.
No candidate was confirmed as a credential. Scanner exit 1 is retained honestly;
no blanket “secret-free” conclusion follows. These false positives do not dispose
of PS-02's separate historical metadata findings.

Limits: no credential validity testing, no remote/dangling/LFS/submodule objects
beyond locally available Git content, no complete archive/binary secret analysis,
and no guarantee against unsupported credential formats or transcript content.
Commit messages received boundary-rule scanning; the Gitleaks history check is
diff-oriented, not a claim of exhaustive secret scanning of every commit message.

## Executed checks and handoff

| Check | Verified result |
| --- | --- |
| `python3 tests/hygiene/test_public_tree.py` | 4 passed |
| `node --test apps/browser/test/origin-proxy.test.mjs` | 8 passed; local fixture sockets only, no browser |
| `swift test --package-path apps/computer --scratch-path <temporary-directory> --disable-automatic-resolution` | 37 passed; no TCC/UI actions |
| `python3 tests/packaging/test_update_archive_cli.py` | 33 passed; fixtures |
| `python3 tests/packaging/test_signing_cli.py` | 8 passed; fixture signing tools |
| `python3 tests/packaging/test_linux_desktop_packages.py` | 9 passed; no package build/download |
| Temporary Git helper fixture | Status exit 0, harmless helper marker created |
| Temporary Computer server/no-op-tools harness | Correct-token dispatch accepted; wrong conversation refused; remote marker refused; launch-only disable semantics confirmed |
| Tracked/history Gitleaks and historical boundary scan | Results and limitations above |
| Pinned Playwright archive inspection (review follow-up) | Version 1.63.0, lockfile SHA-512 integrity matched; default Chromium argument builder adds `--no-sandbox` (PS-06); no dependency installation or browser launch |

No `core/` implementation changed, so a Lite/native C++ rebuild was not performed.
The Chromium suite needs dependencies/runtime and the authorized browser test
environment; its existing security tests were read but are not claimed as run.
No browser sessions were created. The complete native matrix, installed artifacts,
third-party dependency CVEs and external/private implementations remain outside
verified coverage.

Leader follow-up: separate fixes/dispositions for PS-01 through PS-06; reviewer
should validate their preconditions and source lines against the pinned commit.
Publication remains gated on historical review and applicable artifact evidence,
not on approval of this report alone.
