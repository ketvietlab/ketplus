# KetPlus Desktop · A space to create

Interactive desktop mock aligned with the latest fetched KetPlus Mobile design and
implemented desktop features. **10 current screens, 21 workflows, 260 nodes, 339 transitions.**
All product UI copy is English, including the signed-out “Sign in” CTA.

The Workspace `?activity=browser` fixture groups ordinary browser steps under
`Used the browser`. Capture and visual comparison evidence stay outside the
collapsible groups, with independently clickable image previews.
The same fixture accepts `&groupDetails=loading`, `error`, or `updated` for
on-demand detail loading, Retry recovery and a new step while expanded. These
are deterministic prototype states, not daemon networking. The existing static
HTML mock is retained; production rendering remains native Qt/QML.

## Sources and scope

- Desktop: `origin/develop` at `e32e355`, fetched and rebased before this mock change.
- Design reference: `ketvietlab/ketplus-app`, `origin/develop` at `2508489`,
  `ketplus-worktree-mobile.ketatlas`. The bundle is unchanged from `e909d4e`; there is no `design/` directory.
- User adjustments: keep the native desktop's compact **44 px chat-local header**
  and let the right detail/editor pane own its top tab row; put the context/limit ring **after model/effort in the lower composer
  toolbar**, as on mobile; open its panel in place above the ring; show weekly usage **only inside Context & limits**.
- Sidebar repository/conversation lists follow `SidebarRepositoryList.cpp` and
  `ActivityBar.cpp` at Medium zoom: 28 px repository headers with 13 px text,
  36 px conversation rows with 14 px regular text, one status icon and one
  ellipsized title. Worktree names appear in the row tooltip, not a second line.
  Pending questions and confirmations use a static warning Pause icon, including
  async requests while the turn is running. Answering or expiry restores the
  turn's current status; selecting another conversation keeps the waiting mark.
  Use `screens/workspace.html?state=running&pendingInput=true` for the async case.
  The repository's new-conversation action appears on hover or keyboard focus.
  Main navigation labels use the native ActivityBar Body role at Medium zoom:
  12 px, weight 500 (600 selected). Rows are 34 px tall with 2 px between rows,
  16 px icons and a 9 px icon-to-label gap.
  The sidebar Machine select shows repositories from This Mac or one trusted
  Linux machine at a time, in newest-conversation order. The options say
  This Mac or Linux plus the machine name; offline state appears beside the
  list. Connections is the only navigation entry for
  both phones and Linux machines.
- This change updates the mock, documentation and deterministic tests. It does
  not change native Qt code or certify native visual/performance acceptance.
  The Atlas control sits in each conversation's existing chat-local header; its
  list is a right-pane tab. Each Atlas offers Edit in the existing KétPlus
  builder page, with a return to the same conversation, or Open in browser for
  the external viewer.

See [DESIGN.md](DESIGN.md) for the feature/source mapping and design decisions,
and [FLOW-CONTRACT.md](FLOW-CONTRACT.md) for state and ownership contracts.

## Open

```sh
npx --yes ketatlas@0.6.1 serve ./ketplus.ketatlas --port 4198
```

Open `http://127.0.0.1:4198`. Select a workflow, then choose **Open screen**.
The desktop consumer still resolves KetAtlas 0.5.0; this static bundle is
validated and served with 0.6.1. Node.js 22+ is required. There is no bundle
package install or build step.
Refresh after source changes.

## Viewer and mock appearance

KetAtlas 0.6.1 keeps the compact theme switcher and **Sync** control in the
viewer sidebar footer. Enable sync to apply the viewer theme to every canvas preview
and **Open screen** dialog. Sync is off by default. Changing themes does not reload
the screen, discard a draft, close a dialog, or reset navigation.

All seven registered screens load the shared `screens/theme.js` bridge during initialization.
It accepts only the parent viewer's `ketatlas:theme` messages, validates the theme,
and announces `ketatlas:theme-ready`. The bridge uses the existing Két semantic
tokens through `data-theme` and sets `color-scheme` for native form controls.
Sandbox permissions are unchanged.

Turning sync off restores the mock's own `?theme=light|dark` preference (light by
default), or the current OS appearance when `system=true`. Viewer overrides never
enter the mock's URLs or saved Appearance settings. Explicit theme cards disable
Follow system appearance; that preference otherwise survives mock navigation.
Newly loaded routes announce readiness and receive the viewer's current theme.
The viewer's theme controls are independent of the native Qt application's theme.

KetAtlas 0.6.1 preloads the selected flow and retains a bounded preview cache.
Reload the viewer to reset it, and close the browser session after verification.

## Workflows

| Workflow | Coverage |
| --- | --- |
| Work with your Agent | Send, steer, Stop, question, reconnect, missing folder, file/command/image/browser activity |
| Explore code and review changes | Editor, Markdown, unstaged/staged diffs, terminal, clean worktree |
| Switch and create worktrees | Branch validation, creation acknowledgment, switching, missing-folder recovery |
| Switch a conversation worktree | Current context, large-catalog search and paging, pending acknowledgment, busy/stale/offline/denied recovery |
| Open or edit an Atlas from a conversation | Chat header control, right-pane worktree list, in-app Edit and return, browser handoff, running badge, Stop, last-tab-close and recovery |
| Connect your phone | Demo sign-in, QR expiry/renewal, confirmation, paired devices, revoke/reconnect |
| Connect a Linux machine over SSH or the relay | SSH (free) or relay (Pro) choice, SSH host-key confirmation, unreachable/key/daemon errors, relay sign-in, one-time pairing code, identity comparison, connection, offline recovery and acknowledged removal |
| Personalize your workspace | Light/dark, typography, conversation width and save |
| Find files and Atlases with Spotlight | Workspace files and Atlases; keyboard navigation and no results |
| Read a long conversation | Earlier pages, partial turns, Latest, retention and preview errors |
| Archive and restore | Conversation/worktree archive, acknowledgment/error and Settings restore |
| Delete safely | Typed confirmation, active/main/dirty guards and acknowledged removal |
| Choose an agent and inspect context | Codex/Claude/Cursor, effort, usage, capability gates, browser settings |
| Use desktop context menus | Ten target types, 26 menu states, keyboard navigation, disabled actions and recovery |
| Create, rename and fork a conversation | Base worktree, optional branch, validation, creation/fork pending and error, unsupported fork |
| Monitor and stop background tasks | Running list above the composer, shell output tail, subagent progress, stop confirmation, stopping/stopped/completed/failed, daemon offline, orphaned after restart |
| Keep KetPlus up to date | Settings → Updates: up to date, checking, release notes, download, ready while agents run, sidebar reminder, skip, feed error and unconfigured build |
| Set a compact window | Model capacity versus auto-compaction threshold, preset/custom/Auto selection, validation and acknowledgment |

## Try it

- **Context menus:** right-click a conversation, transcript, changed file, worktree, editor/tab, Explorer item or Git change. Shift+F10 also opens the menu. See [CONTEXT-MENUS.md](CONTEXT-MENUS.md) for the native inventory, direct fixtures and implementation contracts.

- **Composer:** Enter sends; Shift+Enter adds a line. Drafts/configuration are per
  conversation. Running Codex/Claude offers text guidance and Stop; Cursor offers
  Stop without unsupported steering. Attach/model/mode lock during a running turn.
- **Context:** the toolbar's far-right ring opens current-window usage and account
  limits. Claude/Cursor never borrow Codex quota. `usage=unknown` shows Not reported.
  Compact is limited to an idle connected Codex session with reported context.
- **Models:** choose a provider tab, model and its discrete effort, then Apply.
  Catalogs are fixtures representing native provider capability contracts.
- **New conversation:** each repository's `+` opens Base worktree and optional
  Create new worktree. `bad branch` and an existing branch fail validation;
  `feature/desktop-design` creates a fixture after acknowledgment.
- **Fork:** right-click a conversation → Fork. An idle Codex/Claude fork preserves
  provider/model/worktree, starts no turn and owns no worktree. Cursor and active
  turns are blocked. Add `outcome=error` before confirming to test recovery.
- **Background tasks:** add `bg=running` (also `stopping`, `stopped`, `completed`,
  `failed`, `orphaned`, `agent`). Rows sit above the composer; Output/Progress opens
  the context pane (`panel=task&task=build|review`) and Stop asks for confirmation.
  `state=error` shows last-known rows with Stop disabled. Fixtures model the Claude
  Agent SDK `background_tasks_changed`, `task_*` events, `get_task_output` (8 KiB
  tail) and `stopTask`; KetPlus does not relay these yet.
- **Composer:** there is no touched-worktree bar (Create PR / View PR rows), in the
  mock or the native app.
- **Activity:** expand file groups, command rows and image previews. Command output
  is collapsed initially. File actions open the desktop context pane.
- **Settings:** Typography includes Default/Wide/Extra wide/Full conversation
  width; Providers & browser shows capability fixtures and a browser-tools toggle.
- **Updates:** Settings → Updates (or the user menu's Check for updates…) walks
  checking → available → downloading → ready. `update=<state>` opens a state
  directly; `result=error` with `pending=1` makes the next check fail. Later keeps
  an **Update available** reminder in the sidebar. Restart to update relaunches only the
  window: running agent turns stay in the background service, which hands over to
  the new version when idle. Nothing is downloaded or signed in the mock.
- **Atlas:** open the network icon in any conversation header. The right pane
  lists the active worktree's Atlases and running viewers elsewhere. The icon's
  badge counts all running viewers; Open in browser shows it, Open browser
  returns to that viewer, and Stop clears it after acknowledgment. Direct fixtures include
  `?panel=atlas&atlasList=empty` and `atlasView=starting|error|running|multiple|elsewhere|stopping|stopped|browser-closed`.
  New Atlas fills the Agent composer; its synthetic bundle appears in the
  selected worktree only after the demo turn acknowledges the request.
  This static mock changes visible state only; the native browser handoff and
  process ownership remain implementation work. KetAtlas 0.6.1 supplies the
  managed CLI/browser lifecycle for that integration.
- **Connections:** the signed-out introduction explains phone access and the three pairing steps; Sign in opens the existing sign-in flow.
- **Machine switch:** use the unframed select above the repository list to move
  between This Mac and a trusted Linux machine. In Agent Workspace it opens the
  newest conversation on that machine; a new conversation moves its repository
  to the top of that machine's list. Offline machines keep history visible and
  disable new remote turns.
- **Linux machines in Connections:** choose Add → Linux machine, then **SSH**
  (free) or **KetPlus Relay** (Pro; `plan=free` shows the upgrade gate instead).
  SSH takes `user@host` or a `~/.ssh/config` Host and uses the system `ssh`:
  `ubuntu@dev-server.local` reaches host-key confirmation and connects;
  `nobody@nowhere`, `bad@dev-server.local`, `new@dev-server.local` and
  `old@dev-server.local` show an unreachable host, a refused key, a missing daemon
  (with copyable install commands KetPlus does not run) and an outdated daemon.
  Each trusted machine shows its route, SSH or Relay. For the relay, use
  `KETP-4826` to reach identity confirmation, `KETP-0000` for an expired code or
  `KETP-9999` for an unavailable machine. Compare the six digits, tick the
  confirmation checkbox, then connect. Machine details offer Disconnect,
  Reconnect and Forget with an acknowledgment step. Atlas nodes expose signed-out,
  empty, permission, pending, error and recovery states directly.
- Pairing, archive, delete, Spotlight and terminal demonstrations remain
  available. The older in-window Atlas pages remain as direct legacy test
  fixtures but are absent from the current screen registry and flows. Terminal
  commands are simulated; use `help` for examples.

Use synthetic credentials only. Pending operations take about 0.9 seconds. Direct
pending URL fixtures stay stable until an interaction creates an operation.
Non-sensitive demo state travels in the URL **fragment**, avoiding HTTP request and
Referer limits. Older `?demo=` snapshots remain readable. No passwords, private
answers, tokens or attachment bytes belong in this state.

## Design system and renderer

This static HTML/CSS/classic-JavaScript prototype represents a native Qt application.
It keeps the public Két AppShell contracts, runtime and semantic tokens;
`desktop-alignment.css` owns desktop composition. No `atlas.renderer.json` is needed.

- Official desktop adapter: `@ketvietlab/design-system@0.1.44`, revision
  `e8adee2da3bfeea66c01f46f110fdc5243fb23c0` (ketviet PR #2177, Két Blue); see
  descriptor and lock. Refresh it with the package's
  `ket-design-system-atlas materialize ketplus.ketatlas` from that
  revision, after `npm run design:inventory` and `npm run build:design-system` in
  `ketsuite/`. `assets/fonts/InterVariable*.woff2` are the font files the
  stylesheet references, copied unchanged from the same revision.
- KétPlus is not a KetSuite app: `K.template("shell")` drops the application
  switcher and `desktop.css` collapses its column.
- Mobile's native Két adaptation informs navigation, activity and composer patterns.
  Desktop tokens are not replaced with copied mobile CSS or touch geometry.
- KÉT Sans 1.0 (tag `v1.0.0`), Lucide Static 1.40.0 and unchanged KétPlus v1 branding assets.
  `assets/fonts-inline.css` embeds the four released optical-size KÉT Sans WOFF2 files
  (300, 400, 500–600, 700) and KÉT Sans Mono 0.10.2 Regular (review build from ket-art
  `work/mono`), and points `--kv-font-sans` and `--kv-font-mono` at them. The mono has one weight, so bold code is
  synthesized. The KÉT Sans licence
  (`licenses/KET-Sans-1.0.txt`), which covers both, allows self-hosted embedding but not redistributing the
  font files, so the fonts are only embedded, never shipped as files. Embedding
  also lets opaque sandboxed previews load the font without cross-origin requests.
  `assets/fonts.css`, `assets/fonts/` (Inter) and `licenses/Inter-OFL.txt` are no longer
  loaded; they stay only because the wave-1 baseline manifest records their hashes.
  Font sizes are whole pixels. Configuration defaults are integers (interface 15, editor
  and terminal 13, Markdown 14 px), and sizes derived from the interface size are rounded
  with `round(…, 1px)`, so no deployed setting stores a fractional size. The locked Két
  Design System stylesheet keeps its rem sizes, which resolve to whole pixels at 16 px.
  Licenses are in `licenses/`; brand provenance is in `assets/brand/ketplus-v1/`.

## Verification

```sh
npx --yes ketatlas@0.6.1 discover . --json
npx --yes ketatlas@0.6.1 validate ./ketplus.ketatlas
npx --yes ketatlas@0.6.1 audit ./ketplus.ketatlas --strict

atlas_test_deps=$(mktemp -d)
npm install --prefix "$atlas_test_deps" --no-audit --no-fund jsdom@26.1.0
for shard in 0 1 2 3 4 5 6 7 8; do
  KETATLAS_TEST_SHARD="$shard/9" NODE_PATH="$atlas_test_deps/node_modules" \
    node --test tests/design/ketatlas-flows.test.cjs || exit 1
done
NODE_PATH="$atlas_test_deps/node_modules" node --test tests/design/ketatlas-remote.test.cjs
```

Tests use local scripts, synthetic data and a synthetic clock, with no provider
quota, native runtime or Git mutation. They cover declared URLs, state continuity
and capability gates. The flow suite runs in nine bounded processes because a
single process can exhaust the V8 heap. Browser checks use KetPlus browser MCP; see
[REVIEW-2026-09.md](REVIEW-2026-09.md) for results and limits.
Sidebar follow-up (2026-09-30): all 220 DOM tests pass. KetPlus browser checks
on direct mock URLs in light and dark confirm single-line titles; computed
conversation rows are 36 px high with 14 px, weight 400 text. Repository labels
are 13 px, weight 500. These checks do not claim native raster parity.

The 0.5.0 theme upgrade and its browser verification limit are recorded in
[REVIEW-THEME-SYNC.md](REVIEW-THEME-SYNC.md).

`atlas.progress.json` continues to track native delivery. Existing checks, evidence
and blockers are preserved; passing mock tests does not complete native acceptance.

Mock hardening (2026-10-08): `#demo=` snapshots now require bounded, valid v1
structures before restoring. Malformed snapshots open fresh sample data, and
prototype-named URL values cannot select undeclared provider, flow or bundle
objects. The execution-worktree picker keeps its pending state visible until the
simulated acknowledgment. These guards protect the review prototype; production
storage, permissions and daemon acknowledgments remain separate native contracts.
The static bundle also embeds its KÉT Sans WOFF2 sources in one stylesheet,
so opaque sandboxed KetAtlas previews load the intended font without font requests.

Final mock check (2026-10-08): 280/280 deterministic DOM/asset tests pass;
this is the baseline before the Linux connection workflow was added.
`discover` finds one bundle, and `validate` plus strict `audit` pass for 17 flows,
10 screens and 62 local files. Browser sweeps of all ten direct screens at
1440 px light and 390 px dark found no horizontal overflow or console errors.
The execution-worktree dialog fit at 360, 390, 768, 1024, 1280, 1440 and
1920 px in both themes; its search control measured 36 px on mobile and 32 px
on desktop after the final CSS adjustment. A fresh 1440 px browser check of a
malformed device fragment rendered the signed-out CTA, no injected node and no
failed local asset requests. The KetAtlas viewer listed all 17 flows and served
`fonts-inline.css` with HTTP 200. Its own `/favicon.ico` returned 404; Chromium
background requests to Google/Autofill were denied by the browser origin fence.
At 390 × 844 px, dark theme and 2× density, the worktree dialog measured
354.09 × 600 px with 16 px padding; its search field measured 36 px high, with
no document overflow, console errors or failed asset requests.
These checks do not certify native Qt rendering or deployment.

Remote Linux mock check (2026-10-08): 311/311 DOM tests pass across the shared
flow harness and the separate remote tests. `discover` finds one bundle;
`validate` and strict `audit` pass for 18 flows, 11 screens and 66 local files.
The KetAtlas viewer opens the 21-step Linux flow. Browser interaction reaches
identity comparison and Connected only after the code and confirmation box; a
390 px dark confirmation dialog is centered at 340 × 290 CSS px. Pairing and
connected layouts have no horizontal overflow at 360, 390, 768, 1024, 1280,
1440 and 1920 px in both themes. The pairing input and button measure 36 px
on mobile and 32 px on desktop, and the 1440 px browser run records no local
console errors or failed local requests. No real Linux daemon, relay, native Qt
screen, VoiceOver or browser zoom at 200% is certified by this mock check.

Current Connections and machine-switch check (2026-10-08): all 337 DOM cases
pass in bounded processes: 303 flow/route cases across nine groups and 34
remote cases. A single unpartitioned Node test process exhausted its V8 heap,
so one-process execution is not claimed to pass. `discover` finds one bundle;
`validate` and strict `audit` pass for 18 flows, 10 screens and 65 local files.
KetPlus browser inspection at 1440 px in light and dark measured the chat-local
header and independent detail tabs at 44 px each. The Machine select has a
0 px border and transparent background in both themes. Connections has no
document overflow at 360, 390, 767, 768, 1024, 1280, 1440 or 1920 px in the
dark theme; 1440 px light also has no overflow. Browser interaction switched
from This Mac to the Linux workspace, and the corrected light capture
(`capture-24`) shows one Linux conversation and its own synthetic build transcript.
This remains a review mock: a live daemon, native Qt, VoiceOver, and 200% zoom
have not been verified here.

External Atlas mock check (2026-10-09): after rebasing the Desktop worktree to
`771c761`, the published `ketatlas@0.6.0` discovers the bundle and passes
`validate` plus strict `audit` for 18 flows, 7 current screens and 64 local
files. All 298 sharded DOM/flow cases and 34 Remote Connections cases pass.
The browser checked 360, 390, 768, 1024, 1440 and 1920 px with light/dark
fixtures; document width stayed at the viewport width. At 1440 px the chat
header and right tab row each measured 44 px, and the search input measured
30 px inside its 32 px frame. Clicking Stop kept the badge during the pending
state, then cleared it at acknowledgment. No console errors or failed local
requests appeared in the inspected 1440/390 runs. VoiceOver, 200% zoom,
native Qt, actual external-window handoff and managed-process integration remain
unverified by this static mock.

Viewer sidebar check (2026-10-09): the Desktop branch is current with
`origin/develop` (rebase was a no-op). Published `ketatlas@0.6.1` discovers 18
flows and 7 screens, validates the bundle, and passes strict audit for 64 local
files. At 1440 × 900, the viewer rail measured 256 px; the compact theme toggle
and Sync switch remained in its one-row footer. The only browser console error
was an optional `favicon.ico` request returning 404. This viewer-only update did
not rerun the mock's DOM/flow or Remote Connections suites above.

Navigation spacing follow-up (2026-10-02): browser inspection confirmed 36 px
rows, 4 px gaps, 18 px icons and unchanged 14 px regular labels.

## Conversation execution worktree

The conversation header opens **Execution worktree**. Selection preserves conversation
identity, draft, and transcript; only the next turn changes its execution root.
`contextSwitch=ready|busy|stale|offline|denied|switched` provides deterministic fixtures.
A busy turn blocks switching; stale state offers Refresh; offline and revoked access
never silently queue a switch. Existing file links and Atlas runtimes keep their source.
This is a mock contract, not evidence that remote switch or native UI is deployed.
The existing Két Design System and platform composition are retained.

Large-catalog fixture: 1,248 worktrees. Current context stays pinned; the initial list
shows model-touched worktrees first, then the remaining choices. Search matches branch or folder; Browse all and Previous /
Next paginate 20 results at a time. `worktreeQuery=atlas` and
`worktreeQuery=no-such-branch` expose results/empty states. No huge select menu or
full-catalog DOM list is rendered. Production catalog search/pagination remains backend work.

## Manual compact window

The **Set a compact window** flow adds manual auto-compaction controls to the existing model picker. The Opus 5.5 example selects 272k while retaining 1M capacity. See [COMPACT-WINDOW-REVIEW.md](COMPACT-WINDOW-REVIEW.md) for fixtures, scope and verification. Native UI wiring is pending design review.


## Conversation repository picker · Desktop and iPhone mock

The two **Switch conversation repository** flows are the proposed replacement for
choosing only a Files/Source Control worktree. Native Desktop and iOS are unchanged.
Both entrypoints share `screens/repository-picker-model.js`,
`screens/repository-picker.js` and `screens/repository-picker.css`; the HTML files
only load the same presenter with the platform flag. This extends the existing
static native-app prototype and its pinned Két adapter; it does not introduce a
second production framework, copied design-system components or a renderer server.

- Desktop: `screens/repository-picker.html?theme=dark&search=pilot`.
- iPhone: `screens/repository-picker-mobile.html?theme=dark&search=pilot`, 390 × 844.
- Viewer: select **Switch conversation repository · Desktop** or **· iPhone**,
  then open a screen to try its sandboxed preview.
- Current repository is pinned first, including during repository search. Worktree
  search stays scoped to that repository, accepts branch or folder paths, and keeps
  its query when the repository changes. Try `pilot`, then choose `ketviet`.
- Choosing a repository commits immediately after a simulated acknowledgment; no
  extra Apply action. It restores the last chosen worktree for that repository,
  otherwise the conversation's recorded agent worktree when available, otherwise
  the main checkout. A further worktree choice applies the same way.
- The same conversation ID, title, transcript and edited draft survive. Header,
  Files/Source Control scope and selected repository update as one state change.
  The active conversation moves out of its old sidebar group, into the new group,
  which is promoted to the top. The iPhone Conversations drawer shows the same order.
- iPhone uses a bottom sheet with Repository → Worktree steps, Back and Done;
  returning to Repository pins the new current choice first. Search and controls
  have touch targets of at least 44px. Step changes focus headings, not the keyboard.
- A running provider turn is retained. Choosing another scope updates the
  conversation and next-turn target, without pretending to move an in-flight tool.
  A compact current-turn/next-turn note appears only when those worktrees differ.
- Fixture recovery: `scenario=error|offline|loading|stale`. Rejection keeps the
  old repository; Retry/Refresh/Reconnect work. Closing during acknowledgment does
  not reopen the sheet. Late acknowledgments cannot replace a newer choice.
- All data, acknowledgments, host connectivity and persistence are simulated in
  memory. `/workspace/...` paths are portable fixtures. No real Git, filesystem,
  daemon, relay or provider operation runs. Reloading resets the draft/history;
  there is no cross-device synchronization in this mock.

Focused checks (no provider quota):

```sh
NODE_PATH=/tmp/ketplus-repository-picker-deps/node_modules node --test tests/design/repository-picker.test.cjs
ketatlas validate ketplus.ketatlas
ketatlas audit ketplus.ketatlas --strict
ketatlas serve ketplus.ketatlas --port 62350 --no-preload
```

Use the existing external jsdom 26.1.0 setup described above; dependencies are not
installed inside the bundle. Browser checks cover direct prototype interactions
at 1440 × 900 and 390 × 844 in light/dark, plus layout at 320/767/768px. Atlas's
sandboxed **Try** iframe and its local resources load; the browser worker cannot
observe inside iframes, so actions were exercised on the exact standalone routes.
The viewer itself has an unrelated `/favicon.ico` 404; direct mock pages have no
console errors or failed network requests. Native Qt/SwiftUI, physical iPhone
keyboard/safe-area behavior, daemon permissions and real cross-device switching
remain implementation work. Mock checks never mark native implementation verified.

## Conversation tags and two-line sidebar rows

Flow `conversation-tags`. Implemented natively per ADR 0028: Desktop SQLite storage, no agent
access, filter not persisted. The native header shows a solid-bordered Add tag button
instead of the mock's dashed one.

- Every sidebar conversation row has two lines in a 48 px row (the native row is 36 px today).
  Line 1 has the status icon, the title at full width and, while a turn started by another
  conversation's message runs, `from N`. Line 2 has the conversation's number (no `#`), then its tags.
  An untagged row shows its status there instead.
- Tags show as whole chips (84 px at most, long names end with an ellipsis). Chips that do not
  fit are hidden and counted in `+N`, whose tooltip lists them. The tag of the active filter
  always comes first. The default sidebar is 214 px wide (180 px minimum), and these rules keep
  line 2 inside it.
- Right-click → **Tags…** opens a popover with a search field and checkable tags. Typing a new
  name offers **Create “…”**, and Enter creates the tag and applies it. Limits: 8 tags per
  conversation, 32 in total, names up to 24 characters, unique regardless of case. Choices that
  are blocked stay visible with the reason.
- Clicking a chip filters every repository on the current machine by that tag. A bar shows the
  tag and the match count with a clear (×) button. Repositories without a match are hidden.
- The conversation header shows the open conversation's tags after the worktree chip: up to
  three chips, then `+N`, then `+`. An untagged conversation shows a dashed **Add tag** button.
  Any of them opens the same tag editor beneath the header. Edits update the header and the
  sidebar row together.
- **Manage tags…** renames tags, cycles their colour (8 colours, light and dark values) and
  deletes them. Deleting a tag removes it from its conversations and clears a filter that uses it.
- Fixtures: `?repo=ketviet`, `&context=conversation`, `&tags=edit[&tagQuery=Release]`,
  `&tagFilter=account`, `&tags=manage`, `&tags=header` (editor opened from the header). Conversation numbers are a stable hash for the mock;
  native numbers come from the daemon (ADR 0027).

## Agent messaging between conversations

Flow `agent-messaging`. Implemented natively per ADR 0027: the daemon numbers every
conversation and its `ketplus_agents` MCP server lets an agent message another one by number.
The mock shows the Desktop projection only.

- Conversations are referenced as `@agent/NNNN`. `#` is not used, because `#1234` also reads as
  an issue or pull request number. The sidebar shows the bare number on a row's second line.
- The header shows the open conversation's `@agent/NNNN` before its title. Right-click a
  conversation → **Copy @agent/NNNN** copies the reference.
- The sender's turn keeps its `agents_send` tool row and adds an **Agent message** note:
  `Sent to @agent/N · title · started a turn · reply requested.`
- An idle receiver starts a turn. Its opening row is an outlined bubble labelled
  `Message from @agent/N · title · provider` instead of the user's text. While that turn runs,
  its sidebar row ends with `from N`.
- The receiver's final answer comes back to the sender as the opening row of the next turn,
  labelled `Reply from @agent/N · title · provider`.
- Over a loop-guard limit (hop 6, 30 automatic turns in one exchange, 20 per receiver per hour)
  the message is held: both conversations show a warning note. A held message is delivered with
  the receiver's next message from the user.
- Typing `@agent/` in the composer lists other conversations by number, title and worktree,
  filtered by number prefix or title; any other `@` stays a file mention. Arrow keys move, Enter
  or Tab inserts `@agent/N `, Escape closes. The open conversation is never offered.
- Fixtures: `?conversation=startup` (receiver), `?conversation=navigation` (sender),
  `&agent=reply`, `&agent=held`, `&mention=conversation` (picker open on `Ask @agent/`).
