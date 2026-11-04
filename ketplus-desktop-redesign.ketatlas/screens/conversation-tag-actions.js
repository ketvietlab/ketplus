/* Tag editor popover, tag filter and Manage tags dialog. Local prototype state only. */
K.persistDemo = () => {
    const fragment = "#" + new URLSearchParams({ demo: JSON.stringify(K.demo) });
    history.replaceState(null, "", location.pathname + location.search + fragment);
    // Every rendered link carries the snapshot; keep them in step with the edit.
    document.querySelectorAll('a[href*="#demo="]').forEach(link => {
        link.setAttribute("href", link.getAttribute("href").replace(/#demo=.*$/, fragment));
    });
};
K.refreshSidebar = () => {
    const sidebar = document.querySelector(".sidebar-inner");
    const scroll = document.querySelector(".repository-groups")?.scrollTop || 0;
    if (sidebar) sidebar.outerHTML = K.sidebar();
    const groups = document.querySelector(".repository-groups");
    if (groups) groups.scrollTop = scroll;
    const header = document.querySelector(".agent-heading .header-tags");
    if (header) header.outerHTML = K.headerTags();
    K.fitConversationTags();
    K.persistDemo();
};
K.tagSlug = label => {
    const base = label.normalize("NFD").replace(/[̀-ͯ]/g, "").replace(/đ/gi, "d")
        .toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 32) || "tag";
    let id = base, n = 2;
    while (K.tagById(id)) id = `${base}-${n++}`;
    return id;
};
K.nextTagColor = () => {
    const used = K.demo.tags.map(tag => tag.color);
    return K.tagColors.find(color => !used.includes(color)) || K.tagColors[K.demo.tags.length % K.tagColors.length];
};
K.tagLabelError = (label, except = "") => {
    if (!label) return "Enter a tag name.";
    if (label.length > K.tagLimits.label) return `Use ${K.tagLimits.label} characters or fewer.`;
    if (K.demo.tags.some(tag => tag.id !== except && tag.label.toLocaleLowerCase() === label.toLocaleLowerCase()))
        return "A tag with this name already exists.";
    return "";
};

K.closeTagEditor = (restoreFocus = true) => {
    const editor = document.querySelector(".tag-editor");
    if (!editor) return;
    const id = editor.dataset.conversation;
    editor.remove();
    if (restoreFocus) document.querySelector(`.conversation[data-conversation="${CSS.escape(id)}"]`)?.focus();
};
K.renderTagEditor = (editor, target) => {
    const record = target.record;
    const input = editor.querySelector("input");
    const query = input.value.trim();
    const lower = query.toLocaleLowerCase();
    const matches = K.demo.tags.filter(tag => !lower || tag.label.toLocaleLowerCase().includes(lower));
    const exact = K.demo.tags.some(tag => tag.label.toLocaleLowerCase() === lower);
    const full = (record.tags || []).length >= K.tagLimits.perConversation;
    const createReason = K.demo.tags.length >= K.tagLimits.catalog ? `You can keep up to ${K.tagLimits.catalog} tags.`
        : full ? `A conversation can have up to ${K.tagLimits.perConversation} tags.` : K.tagLabelError(query);
    const rows = matches.map(tag => {
        const checked = (record.tags || []).includes(tag.id);
        const blocked = !checked && full;
        return `<button type="button" class="tag-option" role="menuitemcheckbox" aria-checked="${checked}" data-tag-toggle="${K.escape(tag.id)}" ${blocked ? `disabled title="A conversation can have up to ${K.tagLimits.perConversation} tags."` : ""}>${K.tagChip(tag)}<span class="tag-option-check">${checked ? K.icon("check") : ""}</span></button>`;
    }).join("");
    const create = query && !exact
        ? `<button type="button" class="tag-option tag-create" data-tag-create ${createReason ? `disabled title="${K.escape(createReason)}"` : ""}>${K.icon("plus")}<span>Create “${K.escape(query.slice(0, 40))}”</span></button>${createReason ? `<p class="tag-editor-note" role="alert">${K.escape(createReason)}</p>` : ""}`
        : "";
    editor.querySelector(".tag-editor-list").innerHTML = rows + create ||
        `<p class="tag-editor-note">No tags yet. Type a name to create one.</p>`;
    // Below the row when it fits, otherwise above it; always inside the window.
    const anchor = target.anchor ? document.querySelector(target.anchor)
        : document.querySelector(`.conversation[data-conversation="${CSS.escape(target.id)}"]`);
    const row = anchor?.getBoundingClientRect();
    const height = editor.offsetHeight;
    const below = row ? row.bottom + 4 : 80;
    const top = row && below + height > innerHeight - 8 ? row.top - height - 4 : below;
    editor.style.left = `${Math.max(8, Math.min(row ? row.left + (target.anchor ? 0 : 24) : 24, innerWidth - editor.offsetWidth - 8))}px`;
    editor.style.top = `${Math.max(8, Math.min(top, innerHeight - height - 8))}px`;
};
K.openTagEditor = target => {
    K.closeTagEditor(false);
    if (!target?.record) return;
    const editor = document.createElement("div");
    editor.className = "tag-editor";
    editor.setAttribute("role", "dialog");
    editor.setAttribute("aria-label", `Tags for ${target.record.title}`);
    editor.dataset.conversation = target.id;
    editor.innerHTML = `<header><span>Tags</span><small>#${K.conversationNumber(target.scope, target.record)} · ${K.escape(target.record.title)}</small></header>
        <input type="text" aria-label="Find or create a tag" placeholder="Find or create a tag" maxlength="40" value="${K.escape(K.query.get("tagQuery") || "")}">
        <div class="tag-editor-list" role="menu"></div>
        <footer>${K.button("Manage tags…", "manage-tags", "settings", "ghost")}</footer>`;
    document.body.append(editor);
    const input = editor.querySelector("input");
    K.renderTagEditor(editor, target);
    input.addEventListener("input", () => K.renderTagEditor(editor, target));
    editor.addEventListener("click", event => {
        const toggle = event.target.closest("[data-tag-toggle]");
        const create = event.target.closest("[data-tag-create]");
        if (toggle && !toggle.disabled) {
            const id = toggle.dataset.tagToggle;
            const tags = target.record.tags ||= [];
            target.record.tags = tags.includes(id) ? tags.filter(tag => tag !== id) : [...tags, id];
        } else if (create && !create.disabled) {
            const label = input.value.trim();
            const tag = { id: K.tagSlug(label), label, color: K.nextTagColor() };
            K.demo.tags.push(tag);
            target.record.tags = [...(target.record.tags || []), tag.id];
            input.value = "";
            K.toast(`Created tag ${label}.`);
        } else return;
        K.refreshSidebar();
        K.renderTagEditor(editor, target);
        input.focus();
    });
    editor.addEventListener("keydown", event => {
        if (event.key === "Escape") { event.preventDefault(); K.closeTagEditor(); }
        if (event.key === "Enter" && event.target === input) {
            event.preventDefault();
            const lower = input.value.trim().toLocaleLowerCase();
            const exact = K.demo.tags.find(tag => tag.label.toLocaleLowerCase() === lower);
            editor.querySelector(exact ? `[data-tag-toggle="${CSS.escape(exact.id)}"]` : "[data-tag-create]")?.click();
        }
        if (["ArrowDown", "ArrowUp"].includes(event.key)) {
            event.preventDefault();
            const items = [input, ...editor.querySelectorAll(".tag-option:not(:disabled)")];
            const i = items.indexOf(document.activeElement);
            items[(i + (event.key === "ArrowDown" ? 1 : -1) + items.length) % items.length].focus();
        }
    });
    input.focus();
};
document.addEventListener("pointerdown", event => {
    if (!event.target.closest(".tag-editor,.desktop-context-menu,.header-tags")) K.closeTagEditor(false);
});

// A tag chip in a sidebar row filters the list instead of opening the conversation.
document.addEventListener("click", event => {
    const chip = event.target.closest("[data-tag-filter]");
    if (!chip || !chip.closest(".conversation")) return;
    event.preventDefault();
    event.stopPropagation();
    K.demo.tagFilter = K.demo.tagFilter === chip.dataset.tagFilter ? "" : chip.dataset.tagFilter;
    K.refreshSidebar();
}, true);
handlers["clear-tag-filter"] = () => { K.demo.tagFilter = ""; K.refreshSidebar(); };

K.openManageTags = () => {
    K.closeTagEditor(false);
    const usage = id => Object.values(K.demo.scopes).flatMap(scope => Object.values(scope.conversations))
        .filter(record => (record.tags || []).includes(id)).length;
    const draft = K.demo.tags.map(tag => ({ ...tag, deleted: false }));
    const row = (tag, i) => `<li class="manage-tag-row ${tag.deleted ? "deleted" : ""}" data-index="${i}">
        <button type="button" class="tag-swatch tag-${tag.color}" data-swatch="${i}" aria-label="Change colour of ${K.escape(tag.label)}" title="Change colour"><span class="tag-dot"></span></button>
        <input aria-label="Tag name" value="${K.escape(tag.label)}" maxlength="${K.tagLimits.label + 8}" ${tag.deleted ? "disabled" : ""}>
        <small>${usage(tag.id)}</small>
        ${K.ib(tag.deleted ? "rotate-ccw" : "trash-2", tag.deleted ? `Keep ${tag.label}` : `Delete ${tag.label}`, "tag-delete-toggle", `data-delete="${i}"`)}</li>`;
    const body = () => draft.length
        ? `<p class="help">Tags belong to this Mac and appear on every repository. Deleting a tag removes it from its conversations.</p><ul class="manage-tags">${draft.map(row).join("")}</ul><p class="error-text" role="alert"></p>`
        : `<p class="help">No tags yet. Right-click a conversation and choose Tags… to create one.</p>`;
    K.contextDialog("Manage tags", body(), "Save", () => {
        const error = document.querySelector(".context-command-dialog .error-text");
        document.querySelectorAll(".manage-tag-row").forEach(node => { draft[node.dataset.index].label = node.querySelector("input").value.trim(); });
        const kept = draft.filter(tag => !tag.deleted);
        for (const tag of kept) {
            const others = kept.filter(other => other !== tag);
            const message = !tag.label ? "Enter a name for every tag."
                : tag.label.length > K.tagLimits.label ? `Use ${K.tagLimits.label} characters or fewer.`
                : others.some(other => other.label.toLocaleLowerCase() === tag.label.toLocaleLowerCase()) ? `“${tag.label}” is used twice.` : "";
            if (message) { error.textContent = message; return; }
        }
        const removed = new Set(draft.filter(tag => tag.deleted).map(tag => tag.id));
        K.demo.tags = kept.map(({ deleted, ...tag }) => tag);
        for (const scope of Object.values(K.demo.scopes))
            for (const record of Object.values(scope.conversations))
                if (record.tags) record.tags = record.tags.filter(id => !removed.has(id));
        if (removed.has(K.demo.tagFilter)) K.demo.tagFilter = "";
        handlers["context-dismiss"]();
        K.refreshSidebar();
        K.toast(removed.size ? `Saved tags · ${removed.size} deleted.` : "Saved tags.");
    });
    const dialog = document.querySelector(".context-command-dialog");
    dialog.querySelector(".modal")?.classList.add("manage-tags-dialog");
    dialog.addEventListener("click", event => {
        const swatch = event.target.closest("[data-swatch]");
        const remove = event.target.closest("[data-delete]");
        if (!swatch && !remove) return;
        event.preventDefault();
        event.stopPropagation();
        document.querySelectorAll(".manage-tag-row").forEach(node => { draft[node.dataset.index].label = node.querySelector("input").value; });
        if (swatch) {
            const tag = draft[swatch.dataset.swatch];
            tag.color = K.tagColors[(K.tagColors.indexOf(tag.color) + 1) % K.tagColors.length];
        } else draft[remove.dataset.delete].deleted = !draft[remove.dataset.delete].deleted;
        dialog.querySelector(".modal-body").innerHTML = body();
    }, true);
};
handlers["manage-tags"] = () => K.openManageTags();
handlers["edit-header-tags"] = () => {
    if (document.querySelector(".tag-editor")?.dataset.anchor === "header") return K.closeTagEditor();
    K.openTagEditor({ kind: "conversation", scope: K.scope, id: K.activeConversation, record: K.current, anchor: ".agent-heading .header-tags" });
    document.querySelector(".tag-editor").dataset.anchor = "header";
};

K.fitConversationTags();
addEventListener("resize", K.fitConversationTags);

// Deterministic Atlas states: ?tags=edit[&tagQuery=…] opens the editor on the selected row, ?tags=header from the header, ?tags=manage the dialog.
{
    const fixture = K.query.get("tags");
    const node = document.querySelector(".conversation.selected");
    if (fixture === "edit" && node) K.openTagEditor(K.contextTarget(node));
    if (fixture === "manage") K.openManageTags();
    if (fixture === "header" && K.current) handlers["edit-header-tags"]();
}
