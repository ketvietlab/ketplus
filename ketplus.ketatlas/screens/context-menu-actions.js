/* Local menu outcomes; no OS, filesystem, provider or real clipboard operations. */
K.contextDialog = (title, body, confirm, action) => {
    document.querySelector(".context-command-dialog")?.remove();
    const host = document.createElement("div");
    host.className = "context-command-dialog";
    host.innerHTML = K.modal(title, body, `${K.button("Cancel", "context-dismiss", null, "ghost")}${K.button(confirm, "context-confirm", null, confirm === "Move to Trash" ? "danger" : "primary")}`).replaceAll('data-action="close-dialog"', 'data-action="context-dismiss"');
    document.body.append(host);
    K.contextConfirm = action;
    (host.querySelector("input") || host.querySelector("button"))?.focus();
};
handlers["context-dismiss"] = () => { document.querySelector(".context-command-dialog")?.remove(); K.contextConfirm = null; };
handlers["context-confirm"] = () => K.contextConfirm?.();
K.contextCopy = text => { K.demo.menuClipboard = text; K.toast("Copied to demo clipboard."); };
K.contextPath = target => {
    const name = target.file || target.scope?.tree || K.tree;
    const directory = target.node?.querySelector("small")?.textContent;
    const relative = target.kind === "changed-file" && directory ? `${directory}/${name}` : name;
    return `/workspace/${target.scope?.repo || K.repo}/${relative}`;
};
K.performContextCommand = (id, target) => {
    const current = K.contextMenuItems(target).find(item => item.id === id);
    if (!current || current.reason) return; // Recheck the target at activation time.
    K.closeContextMenu(false);
    K.query.delete("context");
    const finish = () => { handlers["context-dismiss"](); target.node?.focus?.(); };
    if (["archive", "delete"].includes(id)) {
        const kind = target.kind === "worktree" ? "worktree" : "conversation";
        reload({ dialog: id, kind, target: target.id, targetTree: target.scope.tree, targetRepo: target.scope.repo });
    } else if (id === "tags") {
        K.openTagEditor(target);
    } else if (id === "copy-agent-reference") {
        // ADR 0027: the reference other conversations and the composer use.
        K.demo.menuClipboard = `@agent/${K.conversationNumber(target.scope, target.record)}`;
        K.toast(`Copied ${K.demo.menuClipboard}`);
        finish();
    } else if (id === "fork") {
        if (K.demo.operation) return K.toast("Wait for the pending operation.");
        K.queue("fork");
        K.demo.operation.scope = K.scopeKey(target.scope.tree, target.scope.repo);
        K.demo.operation.conversation = target.id;
        K.toast(`Forking ${target.record.title}…`);
        reload();
    } else if (id === "rename-conversation") {
        K.contextDialog("Rename conversation", `<label for="context-name">Name</label><input id="context-name" value="${K.escape(target.record.title)}"><p class="error-text" role="alert"></p>`, "Rename", () => {
            const value = document.querySelector("#context-name").value.trim();
            if (!value) { document.querySelector(".context-command-dialog .error-text").textContent = "Enter a name."; return; }
            target.record.title = value; finish(); reload();
        });
    } else if (id === "copy-path") K.contextCopy(K.contextPath(target));
    else if (id === "reveal") K.contextDialog(current.label, `<p class="help">Desktop hands this path to the operating system’s file manager.</p><input aria-label="Path" readonly value="${K.escape(K.contextPath(target))}">`, "Done", finish);
    else if (id === "open-file") K.go(target.kind === "changed-file" ? "workspace" : "editor", { file: target.file, panel: target.kind === "changed-file" ? "file" : null, preview: target.file.endsWith(".md") ? "markdown" : null });
    else if (id === "view-diff") K.go("changes", { file: target.file, group: target.group === "staged" ? "staged" : null });
    else if (["new-file", "new-folder", "rename-file"].includes(id)) {
        const rename = id === "rename-file", directory = id === "new-folder";
        K.contextDialog(rename ? "Rename" : directory ? "New Folder" : "New File", `<label for="context-name">Name</label><input id="context-name" value="${rename ? K.escape(target.file) : ""}" placeholder="${directory ? "assets" : "notes.md"}"><p class="error-text" role="alert"></p>`, rename ? "Rename" : "Create", () => {
            const name = document.querySelector("#context-name").value.trim();
            const error = document.querySelector(".context-command-dialog .error-text");
            if (!name || /[\\/\r\n]/.test(name) || [".", ".."].includes(name)) { error.textContent = "Enter a valid name without slashes."; return; }
            if ([...document.querySelectorAll(".file-row > span,.explorer-folder summary > span")].some(n => n.textContent === name && n !== target.node.querySelector("span"))) { error.textContent = "An item with this name already exists."; return; }
            if (K.query.get("outcome") === "error") { error.textContent = "Unable to update this item. Nothing changed; retry or cancel."; return; }
            if (rename) {
                target.node.querySelector("span").textContent = name;
                if (target.node.matches("a")) target.node.href = K.url("editor", { file: name, preview: name.endsWith(".md") ? "markdown" : null });
            } else {
                const explorer = document.querySelector(".file-explorer:not(.git-explorer)");
                explorer.insertAdjacentHTML("beforeend", directory ? `<details class="explorer-folder" open><summary>${K.icon("folder")}<span>${K.escape(name)}</span></summary></details>` : K.link("editor", `<span>${K.escape(name)}</span>`, "file", { file: name }, "file-row"));
            }
            finish(); K.toast("Explorer fixture updated.");
        });
    } else if (id === "trash") {
        K.contextDialog("Move to Trash", `<p>Move “${K.escape(target.file)}” to Trash?</p><p class="error-text" role="alert"></p>`, "Move to Trash", () => {
            if (K.query.get("outcome") === "error") { document.querySelector(".context-command-dialog .error-text").textContent = "Unable to move this item to Trash."; return; }
            (target.node.closest(".explorer-folder") || target.node).remove(); finish(); K.toast("Item moved to Trash · demo.");
        });
    } else if (id === "hidden-files") {
        K.demo.showHiddenFiles = !K.demo.showHiddenFiles;
        document.querySelector(".context-hidden-file")?.remove();
        if (K.demo.showHiddenFiles) document.querySelector(".file-explorer").insertAdjacentHTML("beforeend", K.link("editor", "<span>.gitignore</span>", "file", { file: ".gitignore" }, "file-row context-hidden-file"));
    } else if (id === "refresh-files") K.toast("Explorer refreshed · fixture snapshot.");
    else if (id === "markdown-preview") K.go("editor", { file: target.file, preview: K.query.get("preview") === "markdown" ? null : "markdown" });
    else if (["split-right", "split-down"].includes(id)) {
        const host = document.querySelector(".editor-document");
        if (!host) return;
        host.querySelector(".context-split")?.remove();
        host.classList.toggle("context-split-down", id === "split-down");
        const content = document.createElement("div");
        content.className = "context-split";
        content.setAttribute("aria-label", `Split view: ${target.file}`);
        content.innerHTML = `<div class="context-tabs"><span>${K.escape(target.file)}</span></div>${target.file.endsWith(".md") ? K.markdown() : document.querySelector(".source-code")?.outerHTML || ""}`;
        host.append(content);
    } else if (id === "close-split") { document.querySelector(".context-split")?.remove(); document.querySelector(".editor-document")?.classList.remove("context-split-down"); }
    else if (["close-tab", "close-others", "close-all"].includes(id)) {
        const close = () => {
            finish();
            if (target.kind === "detail-tab") { handlers["close-panel"](); return; }
            const tabs = [...document.querySelectorAll(".file-tabs > a")];
            tabs.filter(node => id === "close-all" || (id === "close-others" ? node !== target.node : node === target.node)).forEach(node => node.remove());
            const remaining = document.querySelector(".file-tabs > a");
            if (!remaining) document.querySelector(".editor-document").innerHTML = '<div class="empty-state"><p>No open files.</p></div>';
            else K.toast("Tab list updated · local fixture.");
        };
        if (K.query.get("contextFixture") === "unsaved") K.contextDialog("Unsaved changes", `<p>Save changes to ${K.escape(target.file)} before closing?</p><p class="help">Cancel keeps the tab open. Saving here updates the mock only.</p>`, "Save and close", close);
        else close();
    } else if (id === "definition") {
        K.contextDialog("Go to Definition", '<p>No definition found in this sample file.</p>', "Done", finish);
    } else {
        const editor = target.node;
        if (id === "select-all") {
            const range = document.createRange(); range.selectNodeContents(editor);
            const selection = window.getSelection(); selection.removeAllRanges(); selection.addRange(range);
        } else if (id === "copy") K.contextCopy(window.getSelection()?.toString() || editor.textContent);
        else if (id === "undo" || id === "redo") {
            const from = id === "undo" ? K.editorUndo : K.editorRedo;
            const to = id === "undo" ? K.editorRedo : K.editorUndo;
            to.push(editor.innerHTML); editor.innerHTML = from.pop();
        } else if (["cut", "paste", "delete-text"].includes(id)) {
            K.editorUndo ||= []; K.editorRedo = []; K.editorUndo.push(editor.innerHTML);
            const selection = window.getSelection();
            if (id === "cut") K.contextCopy(selection?.toString() || editor.textContent);
            if (selection?.rangeCount && editor.contains(selection.anchorNode)) selection.getRangeAt(0).deleteContents();
            else if (id !== "paste") editor.textContent = "";
            if (id === "paste") editor.append(document.createTextNode(K.demo.menuClipboard));
        }
    }
};
document.addEventListener("click", event => {
    const button = event.target.closest("[data-context-command]");
    if (button && !button.disabled && K.openContext) K.performContextCommand(button.dataset.contextCommand, K.openContext);
});
