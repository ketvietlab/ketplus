# Desktop design alignment

## Pinned references

The selected reference is the mobile bundle at
[`ketplus-app` `e909d4e`](https://github.com/ketvietlab/ketplus-app/tree/e909d4e/ketplus-worktree-mobile.ketatlas),
unchanged in the latest fetched `origin/develop` (`2508489`). Desktop matched
`5aba76d` in the earlier review and was rebased to `771c761` before this change.

Desktop pins the Két 0.1.44 static adapter (Két Blue, ketviet `e8adee2d`),
its semantic colors, the public AppShell without the KetSuite application
switcher, local Inter and Lucide. Accent-coloured text and icons use
`--kv-accent-text` and `--kv-nav-selected-text`, never `--kv-accent`, so they
stay readable on dark surfaces. The product implementation is Qt Widgets/QML;
this existing dependency-free web bundle remains a review surrogate, not production
markup. Mobile's older native token adaptation is not copied over desktop tokens.

## Composition

- **Header stays desktop-native:** one 44 px row inside the chat pane, with title,
  compact status, muted repository/branch chip, New conversation, Atlas and Open terminal.
  The right editor/detail pane starts at the window top and owns its own tab row.
  There is no global conversation bar or header ellipsis. Source:
  `AgentWorkspace.qml` and `AgentWorkspaceHeader.qml`.
- **Sidebar:** one Connections entry serves phones and Linux machines. A native
  Machine select scopes the repository list to This Mac or one trusted Linux
  machine. Its options distinguish This Mac from Linux by name; an offline
  machine also has a visible status and recovery link. Offline repositories
  remain identifiable and cannot start a new conversation.
  Switching machines in Agent Workspace opens the newest conversation on that
  machine without stopping turns on the previous one.
  Within the chosen machine, repositories are ordered by their newest
  conversation activity, never by selection. Repository groups contain
  conversations directly. Branch is muted
  conversation metadata. Groups collapse without stopping/selecting a turn.
  Five conversations remain visible, plus running/selected ones; Show more reveals
  the rest. Every repository has a New conversation action.
- **Composer:** outlined 12 px input with inline Send/Stop, followed by the
  attachment/mode and provider/model/effort toolbar. The context ring sits at the
  far right **after model/effort**, matching mobile, not inside the input.
- **Usage:** account limits live only in Context & limits. The panel is a non-modal popover anchored above the ring; closing it returns focus to the ring. The sidebar has no usage
  row. This explicitly requested adjustment differs from the current native footer;
  no native code changes are part of this update.
- **Reading surface:** centered 760 px conversation column; Settings can choose
  864 px, 1200 px or full width. Atlas retains its compact overlay. File and
  terminal context stay in the desktop's right pane.
- **Connections:** one Add action chooses phone pairing or a Linux machine. A
  Linux machine connects over SSH (free, the user's own `ssh` and keys) or the
  KetPlus Relay (Pro: no SSH or open port, phones and notifications). Both are
  offered side by side; a plan without the relay still offers SSH.
  Phones and trusted Linux machines have separate sections on the same page;
  the Linux workflow stays inline while the phone list remains visible.
- **Activity:** muted disclosures, a thin grouping rail, exact command text and
  optional output. File rows link to context; image groups retain previews.
  Working combines text and a spinner with reduced-motion support.
- **Desktop controls:** compact pointer/keyboard geometry and bounded dialogs.
  Mobile bottom sheets, keyboard drawings, remote upload queues and touch-specific
  target sizes are not copied to desktop.

## Implemented-feature mapping

Paths below are relative to the desktop repository.

| Mock behavior | Desktop source of truth |
| --- | --- |
| Repository groups and conversation actions | `docs/agent-workspace.md`; `apps/desktop/src/workspace/SidebarRepositoryList.cpp` |
| Chat-local 44 px header and independent detail tabs | `modules/ai/src/agent/qml/AgentWorkspace.qml`; `AgentWorkspaceHeader.qml`; `apps/desktop/src/app/MainWindowPanels.cpp` |
| Base worktree and optional branch for new conversation | `apps/desktop/src/app/AgentWorktreeDialog.cpp` |
| Codex/Claude/Cursor models, effort, mode and steering | `docs/agent-workspace.md` → Composer and provider capabilities |
| Idle Codex/Claude fork, no Cursor fork or worktree ownership | `docs/decisions/0016-conversation-fork.md`; `apps/desktop/src/app/MainWindowAgent.cpp` |
| Exact context occupancy and provider-specific account limits | `docs/agent-workspace.md` → Current behavior |
| Context compaction restricted to Codex | `docs/agent-workspace.md` → Context compaction |
| Conversation width in saved typography | `docs/agent-workspace.md`; `docs/design-system.md` |
| Browser setting across the three providers | `docs/decisions/0011-browser-tools-for-all-providers.md` |
| Spotlight finds files and Atlases only | `docs/agent-workspace.md` |
| Archive, worktrees, Connections | `docs/project-status.md`; `docs/architecture.md` |
| Worktree Atlas list, in-app Edit, managed external viewer and recovery | Existing `AtlasBuilderTab` behavior and new mock contract in `FLOW-CONTRACT.md`; native navigation update pending |
| Desktop to headless Linux pairing and trust | `docs/product-architecture.md` → daemon outbound relay and device identity; synthetic UI only |
| Settings → Updates, Check for Updates…, Update ready reminder | `docs/decisions/0019-desktop-auto-update.md`; `apps/desktop/src/app/SettingsUpdatesPage.cpp` |

Router, billing, organizations and push are not exposed as implemented features.
Catalogs, runtime readiness, usage and files are deterministic examples; rendering
them proves neither a live installation nor successful provider calls.

## State and interaction

Provider, model, effort, mode, draft and attachments belong to a conversation.
Switching restores its own configuration; a new conversation inherits the current
configuration. Busy states lock configuration. Steering is text-only and
capability-gated. Disconnected Stop is disabled rather than queued.

Creation/fork retain the source until acknowledgment. A failed fork changes nothing.
A successful fork selects its new record only if the source is still selected;
background acknowledgment cannot steal focus. Forks preserve their source worktree
and configuration, own no worktree and start no turn.

The snapshot lives in `#demo=…`, avoiding oversized HTTP request/Referer headers.
Legacy `?demo=` URLs remain readable. Private answers and credentials are excluded.

## Coverage and acceptance

There are 8 current reusable screens and 18 workflows, including provider/context and
conversation-lifecycle journeys. Error/pending/unsupported states have direct URLs.
The `atlas-journey` flow ID is preserved. The Atlas Builder screen is registered
again for in-app editing. The old Atlas library and in-window preview remain
outside the manifest and main sidebar.

The Atlas destination from Spotlight now opens the worktree Atlas tab. Old
in-window progress checks were removed from active screen records and replaced
by incomplete Workspace lifecycle, bundle-identity and visual/performance checks. Their previous evidence remains in
Git history and does not count as acceptance of the new native behavior.

DOM/model tests are the interaction harness. Browser checks cover rendered layouts
and selected primary/recovery routes. They do not replace native Qt tests, pixel
acceptance, provider integration or performance benchmarks.

Limits: one pending synthetic operation, fixed sample files/history, no real Git,
process, account or device actions. Existing deletion fixtures retain the conservative
clean-worktree path; the native optional force-remove path is not represented here.
Full native acceptance remains in `atlas.progress.json` and `tasks/desktop-refactor/`.

## Content language

All product UI copy is English. Use “Sign in”, “View diff” and “Open in browser”;
proper product names such as KétPlus and Két Design System keep their branding.
