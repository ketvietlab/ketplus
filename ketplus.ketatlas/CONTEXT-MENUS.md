# Desktop context-menu contract

English-only product copy. This inventory was read from Desktop
`7b0771abed15986d323c26766ee9f53c11dec7e3` and its pinned public CM
`120897634aa8fbd7af932f1e2ac7272f6589db47`. Public source was inspected at that
exact revision, not at the local public checkout's HEAD. No native or vendored
implementation was changed.

Open **Use desktop context menus** in the Atlas viewer. Its 26 addressable states
cover ten target types (including the separate legacy conversation variant).
Right-click the target, or focus it and press Shift+F10. Direct `context=` URLs
open the menu for review without a pointing-device gesture.

## Inventory and provenance

| Target | Native menu order | Gate / behavior | Source |
| --- | --- | --- | --- |
| Product sidebar conversation | Tags… (ADR 0028); Fork this conversation; separator; Archive conversation; Delete conversation… | Fork requires a started, idle, connected Codex/Claude conversation. Delete disabled while running. Archive does not stop a turn. Capture the clicked worktree and conversation IDs, without selecting it. | `apps/desktop/src/app/MainWindowAgent.cpp`, `showAgentConversationContextMenu`; `SidebarRepositoryList.cpp` |
| Legacy QML conversation | Rename…; Fork this conversation; Delete…; Archive | Additional Rename entry and different order are present in the older QML sidebar. Do not silently merge this into the product sidebar contract. | `modules/ai/src/agent/qml/AgentSidebar.qml`, `conversationContextMenu` |
| Transcript background/message | Fork this conversation | Conversation-wide fork, never fork from a historical message. Same provider/lifecycle gate. No Archive, Delete or Rename here. | `TranscriptContextMenu.qml` |
| Changed-file activity row | Copy Path; Reveal in Finder; Open File | Copy remains available for a missing file; reveal/open are disabled. This row menu takes precedence over the enclosing transcript menu. Windows label: Reveal in Explorer. Linux: Reveal in File Manager. | `TranscriptActivityRow.qml`, `changedFileContextMenu`; `AgentWorkspaceView::revealFileActionText` |
| Worktree | Archive worktree; Delete worktree… | Current workspace cannot be archived. Main/current, active, locked, dirty, creator-busy or unsaved-editor cases must not delete. Unknown/prunable/missing paths are not eligible menu targets. Recheck the actual Git result at execution. | `apps/desktop/src/app/MainWindowWorktrees.cpp`, `showAgentWorktreeContextMenu` |
| Editor document tab | Markdown Preview (Markdown only); Open in Split Right; Open in Split Down; separator; Close; Close Other Tabs; Close All Tabs | Preview is checked when visible. Close Other Tabs requires >1 tab. Close uses the native unsaved-document decision before disposing content. | `MainWindowDocuments.cpp`, `showTabContextMenu` |
| Agent detail tab | Close; Close Other Tabs; Close All Tabs | No editor split actions in this host. One-tab fixture disables Close Other Tabs. | `MainWindowPanels.cpp`, `showAgentDetailTabContextMenu` |
| Editor content | Undo; Redo; separator; Cut; Copy; Paste; Delete; separator; Select All; separator; Go to Definition; Split Right; Split Down; Close Split (when split exists); Markdown Preview (Markdown only) | Undo/redo depend on history; cut/copy/delete on selection; paste on editor/clipboard state; Select All on nonempty content. Split only for a document owned by the editor host, not Agent/Atlas detail. | `MainWindowDocuments.cpp`, `showEditorContextMenu` / `updateEditorActions` |
| Explorer file/folder/root | Open; View Git Diff (file only); New File…; New Folder…; Rename; Move to Trash (non-root only); Open Containing Folder; Copy Path; Show Hidden Files; Refresh | Root/empty area targets the workspace root and omits Rename/Trash. New entry uses the directory or selected file's parent. Reject invalid/duplicate names. Trash requires confirmation and leaves the item on failure. Hidden Files is checked state. | Public CM `src/workspace/ExplorerPanel.cpp`, `showContextMenu` |
| Git changed-file row | View Diff; Open File; separator; Copy Path | Only file rows, not group headers. Preserve staged vs working-tree diff mode. Missing/deleted file disables Open File. No Stage, Unstage, Commit or Discard items. | Public CM `src/git/GitChangesPanel.cpp`, `showContextMenu` |

No custom right-click menu was found in the inspected private Atlas library/builder,
repository headers, Connections or public terminal surface. Their existing click
controls remain; this mock does not invent a menu for them. Browser/WebView or
platform text-field defaults are not product-owned menu specifications.

## Interaction rules

- Open at the pointer (or focused target for Shift+F10), clamped inside the
  viewport. The menu overlays content, with no backdrop or layout shift.
- Arrow Up/Down, Home/End move through enabled items. Escape and Tab dismiss and
  return focus. Outside pointer-down dismisses without moving focus back.
- Disabled items remain visible with their reason. Activation rechecks the
  target's availability. Destructive items use the danger color; separators keep
  related operations together.
- A background conversation action carries its own repository/worktree/record
  identity through the confirmation and acknowledgment. It must not operate on
  the currently selected conversation by accident. Drafts stay intact.
- Fork queues the existing mock acknowledgment directly. It does not add a
  second confirmation absent from the native menu action. Failures preserve the
  source; a background fork does not select its new conversation.
- Platform menus should use native command shortcuts from the action registry;
  this mock intentionally avoids hardcoding macOS shortcuts onto other platforms.

## Direct fixtures

All paths are relative to the bundle's served root.

| State | URL |
| --- | --- |
| Sidebar idle / running / unsupported provider | `screens/workspace.html?context=conversation` + `&state=running` or `&provider=Cursor` |
| Legacy sidebar | `screens/workspace.html?context=legacy-conversation` |
| Transcript | `screens/workspace.html?context=transcript` |
| Changed file missing | `screens/workspace.html?context=changed-file&contextFixture=missing&activity=files` |
| Main worktree | `screens/worktrees.html?context=worktree` |
| Clean linked worktree | `screens/worktrees.html?context=worktree&fixture=clean&targetTree=fix-scroll-anchor` |
| Dirty / locked / unsaved | Change `fixture=dirty`, or append `contextFixture=locked` / `contextFixture=unsaved` to the clean fixture |
| Markdown tab | `screens/editor.html?context=editor-tab&file=design-system.md&preview=markdown` |
| Unsaved tab | `screens/editor.html?context=editor-tab&contextFixture=unsaved` |
| Agent detail tab | `screens/workspace.html?panel=file&context=detail-tab` |
| Editor selection | `screens/editor.html?context=editor&contextFixture=selection` |
| Explorer file / folder / root | `screens/editor.html?context=explorer-file` (also `explorer-folder`, `explorer-root`) |
| Explorer failure | `screens/editor.html?context=explorer-file&outcome=error` |
| Staged Git row | `screens/changes.html?context=git-file&group=staged` |
| Deleted Git file | `screens/changes.html?context=git-file&contextFixture=missing` |

## Mock outcomes and implementation gaps

Existing navigation, archive/delete confirmation and acknowledgment flows are
reused. Explorer create/rename/trash, hidden-file display, split layout and tab
close update the current DOM fixture. Editor selection, cut/paste and undo/redo
operate on sample DOM content. These local presentation changes reset on reload;
they are not filesystem/document persistence. Copy uses a synthetic demo clipboard
with a synthetic `/workspace/…` path. Reveal displays the native handoff target,
without opening the user's file manager. Go to Definition shows the no-result
state for the sample. No provider, Git mutation or OS clipboard is invoked.

The existing mock archive confirmation is an extra review step; the current
native sidebar archives immediately. The mock's typed DELETE confirmation is
also stricter than the native worktree dialog. Native conversation deletion has
linked-worktree/force options that are outside this mock's guarded deletion
contract. Implementers must reconcile these explicitly, not infer parity from
the context menu alone. The native worktree menu pre-check does not itself know
all Git dirtiness/lock outcomes; Git can still reject after confirmation.

The editor fixture does not provide native selection granularity, language-server
definitions, document save failure, or all split/tab ownership combinations.
Those remain native acceptance requirements. The menu audit demonstrates entry
points and command contracts, not a replacement editor implementation.

## Verification

The deterministic suite includes real DOM `contextmenu` events, menu precedence,
cross-repository archive identity, disabled actions, keyboard focus, missing-file
gates, staged diff routing, invalid-name/retry and unsaved-close cancellation.
KetPlus browser MCP checks rendered menus and Shift+F10/keyboard activation; its
click API does not expose a right mouse button. Pointer right-click dispatch is
therefore covered by DOM tests, not claimed as a real-browser mouse test.

Native delivery statuses/checks remain unchanged. This is mock review evidence.
