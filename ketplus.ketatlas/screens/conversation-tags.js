/* Conversation tags and two-line sidebar rows. Prototype data only: tags live in the URL snapshot. */
window.KET_ICONS["rotate-ccw"] ??= '<path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/>';
window.KET_ICONS.tag ??= '<path d="M12.59 2.59A2 2 0 0 0 11.17 2H4a2 2 0 0 0-2 2v7.17a2 2 0 0 0 .59 1.41l8.71 8.71a2.43 2.43 0 0 0 3.42 0l6.58-6.58a2.43 2.43 0 0 0 0-3.42z"/><circle cx="7.5" cy="7.5" r=".5" fill="currentColor"/>';
K.tagColors = ["blue", "green", "violet", "amber", "teal", "red", "pink", "slate"];
K.tagLimits = Object.freeze({ catalog: 32, perConversation: 8, label: 24 });
K.defaultTags = [
    { id: "ui", label: "UI", color: "blue" },
    { id: "account", label: "Account", color: "green" },
    { id: "review", label: "Review", color: "violet" },
    { id: "perf", label: "Performance", color: "amber" },
    { id: "atlas", label: "Atlas", color: "teal" },
    { id: "bug", label: "Bug", color: "red" },
];
K.demo.tags ??= K.defaultTags.map(tag => ({ ...tag }));
K.tagById = id => K.demo.tags.find(tag => tag.id === id);
K.recordTags = record => (record?.tags || []).map(K.tagById).filter(Boolean);
K.validTags = saved =>
    (saved.tags === undefined || (Array.isArray(saved.tags) && saved.tags.length <= K.tagLimits.catalog &&
        saved.tags.every(tag => tag && typeof tag === "object" && /^[a-z0-9][a-z0-9-]{0,39}$/.test(tag.id) &&
            typeof tag.label === "string" && tag.label.trim() && tag.label.length <= K.tagLimits.label &&
            K.tagColors.includes(tag.color)) &&
        new Set(saved.tags.map(tag => tag.id)).size === saved.tags.length)) &&
    (saved.tagFilter === undefined || saved.tagFilter === "" || typeof saved.tagFilter === "string") &&
    Object.values(saved.scopes || {}).every(scope => Object.values(scope.conversations || {}).every(record =>
        (record.tags === undefined || (Array.isArray(record.tags) && record.tags.length <= K.tagLimits.perConversation &&
            record.tags.every(id => typeof id === "string" && id.length <= 40))) &&
        (record.agentFrom === undefined || /^\d{4,5}$/.test(record.agentFrom))));
if (!K.validTags(K.demo)) {
    K.demo.tags = K.defaultTags.map(tag => ({ ...tag }));
    K.demo.tagFilter = "";
    for (const scope of Object.values(K.demo.scopes))
        for (const record of Object.values(scope.conversations)) { delete record.tags; delete record.agentFrom; }
}

// ADR 0027 numbers are random four-digit aliases (referenced as @agent/NNNN; the sidebar shows the bare number); the mock derives a stable one per conversation.
K.conversationNumber = (scope, record) => {
    let hash = 2166136261;
    for (const char of `${scope.repo}:${scope.tree}:${record.id}`)
        hash = Math.imul(hash ^ char.charCodeAt(0), 16777619);
    return String(1000 + ((hash >>> 0) % 9000));
};
K.tagSeeds = {
    "ketplus-desktop:develop": { navigation: ["ui"], startup: ["perf", "review"], atlas: ["atlas"] },
    "sample-project:develop": { navigation: ["account", "ui"], startup: ["account", "bug", "review"] },
};
K.seedConversationTags = scope => {
    if (scope.tagsSeeded) return scope;
    scope.tagsSeeded = true;
    const seeds = K.tagSeeds[K.scopeKey(scope.tree, scope.repo)] || {};
    for (const record of Object.values(scope.conversations)) {
        record.tags ??= [...(seeds[record.id] || (record.id === "review" ? ["review"] : []))];
    }
    // A running turn that another conversation's message started names its sender (ADR 0027).
    const running = scope.conversations.startup, sender = scope.conversations.navigation;
    if (running && sender && running.agentFrom === undefined)
        running.agentFrom = K.conversationNumber(scope, sender);
    return scope;
};
{
    const ensureScope = K.ensureScope;
    K.ensureScope = (tree = K.tree, repo = K.repo) => K.seedConversationTags(ensureScope(tree, repo));
    K.seedConversationTags(K.scope);
}
K.isTagFiltered = record => !K.demo.tagFilter || (record.tags || []).includes(K.demo.tagFilter);

K.tagChip = (tag, extra = "") =>
    `<span class="conversation-tag tag-${tag.color}" ${extra}><span class="tag-label">${K.escape(tag.label)}</span></span>`;
// Line 1: title. Line 2: number and tags (or the status). The status icon trails the row; the transcript, not the row, names an agent sender.
K.conversationRowContent = (record, scope) => {
    // The active filter's tag leads, so a filtered row never hides the reason it matched.
    const tags = K.recordTags(record).sort((a, b) => (b.id === K.demo.tagFilter) - (a.id === K.demo.tagFilter));
    const status = K.conversationStatus(record.needsInput && K.isActiveTurn(record) ? "question" : record.turn);
    // Every chip is rendered; K.fitConversationTags hides whole chips that do not fit and counts them in +N.
    const chips = tags.map(tag => K.tagChip(tag, `data-tag-filter="${K.escape(tag.id)}" data-tag-label="${K.escape(tag.label)}" title="Show only ${K.escape(tag.label)}"`)).join("") +
        (tags.length ? `<span class="conversation-tag tag-more" hidden></span>` : "");
    return `<span class="conversation-lines">
        <span class="conversation-line"><span class="conversation-title">${K.escape(record.title)}</span></span>
        <span class="conversation-line conversation-meta-line"><span class="conversation-number">${K.conversationNumber(scope, record)}</span>${chips ? `<span class="conversation-tags">${chips}</span>` : `<span class="conversation-meta-status status-${status.tone}">${status.label}</span>`}</span>
    </span>${K.conversationActivity(record)}`;
};
K.tagFilterBar = () => {
    const tag = K.tagById(K.demo.tagFilter);
    if (!tag) return "";
    const count = Object.values(K.demo.scopes).filter(scope => !scope.archived && !scope.deleted)
        .flatMap(scope => Object.values(scope.conversations)).filter(record => !record.archived && (record.tags || []).includes(tag.id)).length;
    return `<div class="tag-filter-bar" role="status"><span>Tag</span>${K.tagChip(tag)}<small title="${count} ${count === 1 ? "conversation" : "conversations"}">${count}</small>${K.ib("x", "Clear tag filter", "clear-tag-filter")}</div>`;
};
if (K.query.has("tagFilter")) K.demo.tagFilter = K.tagById(K.query.get("tagFilter")) ? K.query.get("tagFilter") : "";
// Native rows measure the same way: keep the number, then as many whole chips as fit.
K.fitConversationTags = () => document.querySelectorAll(".conversation-meta-line").forEach(line => {
    const chips = [...line.querySelectorAll("[data-tag-filter]")];
    const more = line.querySelector(".tag-more");
    if (!more) return;
    chips.forEach(chip => { chip.hidden = false; });
    more.hidden = true;
    const fits = () => line.scrollWidth <= line.clientWidth + 0.5;
    let shown = chips.length;
    while (!fits() && shown > 0) {
        chips[--shown].hidden = true;
        more.hidden = false;
        more.textContent = `+${chips.length - shown}`;
    }
    const rest = chips.slice(shown).map(chip => chip.dataset.tagLabel);
    more.title = rest.length ? `Also tagged: ${rest.join(", ")}` : "";
});
// Header: the open conversation's tags after its worktree chip. Every chip opens the tag editor.
K.headerTagLimit = 3;
K.headerTags = (record = K.current) => {
    if (!record) return "";
    const tags = K.recordTags(record);
    const shown = tags.slice(0, K.headerTagLimit).map(tag =>
        `<button type="button" class="header-tag" data-action="edit-header-tags" aria-label="${K.escape(tag.label)} tag. Edit tags" title="Edit tags">${K.tagChip(tag)}</button>`).join("");
    const rest = tags.slice(K.headerTagLimit);
    const more = rest.length ? `<button type="button" class="header-tag" data-action="edit-header-tags" title="Also tagged: ${K.escape(rest.map(tag => tag.label).join(", "))}"><span class="conversation-tag tag-more">+${rest.length}</span></button>` : "";
    const add = `<button type="button" class="header-tag-add" data-action="edit-header-tags" aria-label="${tags.length ? "Edit tags" : "Add tag"}" title="${tags.length ? "Edit tags" : "Add tag"}">${K.icon(tags.length ? "plus" : "tag")}${tags.length ? "" : "<span>Add tag</span>"}</button>`;
    return `<span class="header-tags" aria-label="Tags">${shown}${more}${add}</span>`;
};
