/* Menu inventory follows native Desktop + the pinned CM revision; see CONTEXT-MENUS.md. */
K.contextForkBlocker = record => !record ? "Conversation unavailable."
    : !K.providers[record.provider || "Codex"]?.fork ? "This provider does not support forks."
      : record.turn === "empty" ? "Send the first message before forking."
        : K.isActiveTurn(record) ? "Wait for the active turn to finish."
          : record.connection !== "connected" ? "Reconnect before forking." : "";
K.contextMenuItems = target => {
    const item = (id, label, reason = "", extra = {}) => ({ id, label, reason, ...extra });
    const separator = { separator: true };
    const copyPath = item("copy-path", "Copy Path");
    const missing = K.query.get("contextFixture") === "missing";
    const selection = Boolean(window.getSelection()?.toString()) || K.query.get("contextFixture") === "selection";
    const markdown = target.file?.endsWith(".md");
    const fork = item("fork", "Fork this conversation", K.contextForkBlocker(target.record));
    const close = [item("close-tab", "Close"), item("close-others", "Close Other Tabs", target.tabCount < 2 ? "Only one tab is open." : ""), item("close-all", "Close All Tabs")];
    const preview = item("markdown-preview", "Markdown Preview", "", { checked: K.query.get("preview") === "markdown" });
    switch (target.kind) {
        case "conversation": return [fork, separator, item("archive", "Archive conversation"), item("delete", "Delete conversation…", K.deletionBlocker({ ...target, kind: target.kind === "worktree" ? "worktree" : "conversation" }), { danger: true })];
        case "legacy-conversation": return [item("rename-conversation", "Rename…"), fork, item("delete", "Delete…", K.deletionBlocker({ ...target, kind: "conversation" }), { danger: true }), item("archive", "Archive")];
        case "transcript": return [fork];
        case "changed-file": return [copyPath, item("reveal", K.query.get("platform") === "windows" ? "Reveal in Explorer" : K.query.get("platform") === "linux" ? "Reveal in File Manager" : "Reveal in Finder", missing ? "This file no longer exists." : ""), item("open-file", "Open File", missing ? "This file no longer exists." : "")];
        case "worktree": return [item("archive", "Archive worktree", target.scope.tree === "develop" ? "The current workspace cannot be archived." : ""), item("delete", "Delete worktree…", K.deletionBlocker({ ...target, kind: target.kind === "worktree" ? "worktree" : "conversation" }) || (["locked", "unsaved"].includes(K.query.get("contextFixture")) ? "Locked worktrees or unsaved editors prevent deletion." : ""), { danger: true })];
        case "editor-tab": return [...(markdown ? [preview, separator] : []), item("split-right", "Open in Split Right"), item("split-down", "Open in Split Down"), separator, ...close];
        case "detail-tab": return close;
        case "editor": return [item("undo", "Undo", K.editorUndo?.length ? "" : "Nothing to undo."), item("redo", "Redo", K.editorRedo?.length ? "" : "Nothing to redo."), separator,
            item("cut", "Cut", selection ? "" : "Select text first."), item("copy", "Copy", selection ? "" : "Select text first."), item("paste", "Paste", K.demo.menuClipboard ? "" : "Demo clipboard is empty."), item("delete-text", "Delete", selection ? "" : "Select text first."), separator, item("select-all", "Select All"), separator,
            item("definition", "Go to Definition"), ...(target.detail ? [] : [separator, item("split-right", "Split Right"), item("split-down", "Split Down"), ...(document.querySelector(".context-split") ? [item("close-split", "Close Split")] : [])]), ...(markdown ? [separator, preview] : [])];
        case "explorer-file": case "explorer-folder": case "explorer-root": return [
            ...(target.kind === "explorer-file" ? [item("open-file", "Open"), item("view-diff", "View Git Diff"), separator] : []),
            item("new-file", "New File…"), item("new-folder", "New Folder…"),
            ...(target.kind !== "explorer-root" ? [separator, item("rename-file", "Rename"), item("trash", "Move to Trash", "", { danger: true })] : []),
            separator, item("reveal", "Open Containing Folder"), copyPath, separator,
            item("hidden-files", "Show Hidden Files", "", { checked: Boolean(K.demo.showHiddenFiles) }), item("refresh-files", "Refresh")];
        case "git-file": return [item("view-diff", "View Diff"), item("open-file", "Open File", missing ? "This file no longer exists." : ""), separator, copyPath];
        default: return [];
    }
};
