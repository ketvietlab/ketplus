/* A viewport-clamped menu shared by right-click, Shift+F10 and direct Atlas fixtures. */
K.contextTarget = element => {
    const match = selector => element.closest(selector);
    const fromLink = node => Object.fromEntries(new URL(node?.getAttribute("href") || location.href, location.href).searchParams);
    const file = K.query.get("file") || "NavigationRail.qml";
    let node = match(".conversation");
    if (node) {
        const q = fromLink(node), scope = K.ensureScope(q.tree || K.tree, q.repo || K.repo);
        return { kind: "conversation", node, scope, id: q.conversation, record: scope.conversations[q.conversation] };
    }
    node = match(".worktree-record");
    if (node) {
        const scope = K.ensureScope(node.querySelector("[data-tree]")?.dataset.tree || K.tree);
        return { kind: "worktree", node, scope, record: scope, id: scope.tree };
    }
    node = match("a.changed-file-row");
    if (node) return { kind: "changed-file", node, file: fromLink(node).file || file };
    node = match(".change-row");
    if (node) { const q = fromLink(node); return { kind: "git-file", node, file: q.file || (q.group === "staged" ? "ThemeManager.cpp" : file), group: q.group || "working" }; }
    node = match(".file-row");
    if (node) return { kind: "explorer-file", node, file: fromLink(node).file || file };
    node = match(".explorer-folder > summary");
    if (node) return { kind: "explorer-folder", node, file: node.querySelector("span")?.textContent || "docs" };
    node = match(".file-explorer:not(.git-explorer)");
    if (node) return { kind: "explorer-root", node, file: "." };
    node = match(".file-tabs > a");
    if (node) return { kind: "editor-tab", node, file: fromLink(node).file || "NavigationRail.qml", tabCount: document.querySelectorAll(".file-tabs > a").length };
    node = match(".context-pane .context-tabs");
    if (node) return { kind: "detail-tab", node, file, tabCount: 1 };
    node = match(".source-code, .markdown-preview");
    if (node) return { kind: "editor", node, file, detail: Boolean(node.closest(".context-pane")) };
    node = match(".chat-scroll, .builder-scroll");
    if (node && !match("input,textarea")) return { kind: "transcript", node, scope: K.scope, record: K.current, id: K.activeConversation };
    return null;
};
K.closeContextMenu = (restore = true) => {
    document.querySelector(".desktop-context-menu")?.remove();
    if (restore && K.openContext?.node?.isConnected) K.openContext.node.focus({ preventScroll: true });
    K.openContext = null;
};
K.openContextMenu = (target, x, y) => {
    K.closeContextMenu(false);
    const items = K.contextMenuItems(target);
    if (!items.length) return;
    K.openContext = target;
    const menu = document.createElement("div");
    menu.className = "desktop-context-menu";
    menu.tabIndex = -1;
    menu.setAttribute("role", "menu");
    menu.setAttribute("aria-label", `${target.kind.replaceAll("-", " ")} actions`);
    menu.innerHTML = items.map(item => item.separator ? '<div role="separator"></div>' : `<button type="button" role="${typeof item.checked === "boolean" ? "menuitemcheckbox" : "menuitem"}" ${typeof item.checked === "boolean" ? `aria-checked="${item.checked}"` : ""} class="${item.danger ? "danger" : ""}" data-context-command="${item.id}" ${item.reason ? `disabled title="${K.escape(item.reason)}"` : ""}><span class="context-check">${item.checked ? K.icon("check") : ""}</span><span>${K.escape(item.label)}${item.reason ? `<small>${K.escape(item.reason)}</small>` : ""}</span></button>`).join("");
    menu.dataset.anchorX = x;
    menu.dataset.anchorY = y;
    document.body.append(menu);
    menu.style.left = `${Math.max(8, Math.min(x, innerWidth - menu.offsetWidth - 8))}px`;
    menu.style.top = `${Math.max(8, Math.min(y, innerHeight - menu.offsetHeight - 8))}px`;
    (menu.querySelector("button:not(:disabled)") || menu).focus({ preventScroll: true });
};
document.addEventListener("contextmenu", event => {
    if (event.target.closest(".desktop-context-menu")) { event.preventDefault(); return; }
    const target = K.contextTarget(event.target);
    if (!target) return;
    event.preventDefault();
    event.stopPropagation();
    K.openContextMenu(target, event.clientX, event.clientY);
});
// Native sidebar renames on double-click. A click on the already selected row
// stays in place so the second click can reach this handler in the static mock.
document.addEventListener("click", event => {
    if (K.page === "workspace" && event.target.closest(".conversation.selected") &&
        !event.metaKey && !event.ctrlKey && !event.shiftKey) event.preventDefault();
}, true);
document.addEventListener("dblclick", event => {
    const row = event.target.closest(".conversation.selected");
    if (K.page !== "workspace" || !row) return;
    event.preventDefault();
    const target = K.contextTarget(row);
    K.go("workspace", { repo: target.scope.repo, tree: target.scope.tree,
        conversation: target.id, state: target.record.turn, dialog: "rename" });
});
document.addEventListener("keydown", event => {
    if (event.key === "ContextMenu" || (event.shiftKey && event.key === "F10")) {
        const target = K.contextTarget(event.target);
        if (!target) return;
        event.preventDefault();
        const r = target.node.getBoundingClientRect();
        K.openContextMenu(target, r.left + 16, r.top + 24);
    }
    const menu = document.querySelector(".desktop-context-menu");
    if (!menu) return;
    if (["Escape", "Tab"].includes(event.key)) { event.preventDefault(); K.closeContextMenu(); }
    if (["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) {
        event.preventDefault();
        const buttons = [...menu.querySelectorAll("button:not(:disabled)")];
        if (!buttons.length) return;
        const i = buttons.indexOf(document.activeElement);
        buttons[event.key === "Home" ? 0 : event.key === "End" ? buttons.length - 1 : (i + (event.key === "ArrowDown" ? 1 : -1) + buttons.length) % buttons.length].focus();
    }
});
document.addEventListener("pointerdown", event => { if (!event.target.closest(".desktop-context-menu")) K.closeContextMenu(false); });
window.addEventListener("resize", () => {
    const menu = document.querySelector(".desktop-context-menu");
    if (!menu) return;
    menu.style.left = `${Math.max(8, Math.min(Number(menu.dataset.anchorX), innerWidth - menu.offsetWidth - 8))}px`;
    menu.style.top = `${Math.max(8, Math.min(Number(menu.dataset.anchorY), innerHeight - menu.offsetHeight - 8))}px`;
});

// Non-link surfaces have the same keyboard entry point as native context-menu targets.
document.querySelectorAll(".worktree-record,.source-code,.markdown-preview,.context-pane .context-tabs,.root-folder,.builder-scroll").forEach(node => node.tabIndex = 0);
const contextFixture = K.query.get("context");
if (contextFixture) {
    const selectors = { conversation: ".conversation.selected", transcript: ".chat-scroll", "changed-file": "a.changed-file-row", worktree: ".worktree-record", "editor-tab": ".file-tabs > a", "detail-tab": ".context-pane .context-tabs", editor: ".source-code,.markdown-preview", "explorer-file": ".file-row", "explorer-folder": ".explorer-folder > summary", "explorer-root": ".root-folder", "git-file": ".change-row" };
    let node = document.querySelector(selectors[contextFixture] || selectors.conversation);
    if (contextFixture === "editor-tab" && K.query.has("file")) node = [...document.querySelectorAll(".file-tabs > a")].find(n => new URL(n.href).searchParams.get("file") === K.query.get("file")) || node;
    if (contextFixture === "git-file" && K.query.get("group") === "staged") node = document.querySelector(".change-row.selected") || node;
    if (contextFixture === "worktree" && K.query.has("targetTree")) node = [...document.querySelectorAll("[data-tree]")].find(n => n.dataset.tree === K.query.get("targetTree"))?.closest(".worktree-record");
    if (node) {
        const target = K.contextTarget(node);
        if (contextFixture === "legacy-conversation") target.kind = contextFixture;
        const rect = node.getBoundingClientRect();
        K.openContextMenu(target, Math.max(230, rect.left + 24), Math.max(70, rect.top + 24));
    }
}
