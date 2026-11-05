/* Deterministic catalogs demonstrate the native provider contract, not live availability. */
K.providers = {
    Codex: { models: ["GPT-5.6 Luna", "GPT-5.6 Terra", "GPT-5.6 Sol"], efforts: ["Low", "Medium", "High", "Max"], fork: true, steer: true },
    Claude: { models: ["Sonnet", "Opus 5.5", "Haiku"], efforts: ["Low", "Medium", "High"], fork: true, steer: true },
    Cursor: { models: ["Auto", "Composer 2.5"], efforts: [], fork: false, steer: false },
};
K.current.provider ||= "Codex";
K.current.model ||= "GPT-5.6 Luna";
K.current.effort ||= "Medium";
K.current.mode ||= "workspace-write";
for (const key of ["provider", "model", "effort", "mode"])
    if (K.query.has(key)) K.current[key] = K.query.get(key);
if (!Object.hasOwn(K.providers, K.current.provider)) K.current.provider = "Codex";
if (!K.providers[K.current.provider].models.includes(K.current.model))
    K.current.model = K.providers[K.current.provider].models[0];
if (K.query.get("forked") === "1" && !K.current.forkedFrom) {
    K.current.forkedFrom = "original-conversation";
    K.current.title += " (fork)";
    K.current.ownsWorktree = false;
}
K.provider = () => K.providers[K.current.provider];
K.configurationLocked = () => K.isActiveTurn() || K.current.connection !== "connected" || !K.current.available;
K.forkBlocker = () => !K.provider().fork
    ? "This provider does not support conversation forks."
    : K.current.turn === "empty" ? "Send the first message before forking."
      : K.isActiveTurn() ? "Wait for this turn to finish before forking."
        : K.current.connection !== "connected" ? "Reconnect to fork this conversation." : "";
K.modelDialog = () => {
    const requestedProvider = K.query.get("picker") || K.current.provider;
    const provider = Object.hasOwn(K.providers, requestedProvider) ? requestedProvider : K.current.provider;
    const catalog = K.providers[provider];
    const locked = K.compactWindow.modelBlocked();
    const selected = catalog.models.includes(K.current.model) ? K.current.model : catalog.models[0];
    const effort = catalog.efforts.includes(K.current.effort) ? K.current.effort : "Medium";
    return K.modal("Model & context", `<div class="provider-tabs" role="tablist" aria-label="Agent provider">${Object.keys(K.providers).map(name => K.button(name, "provider-tab", null, name === provider ? "active" : "ghost", `role="tab" aria-selected="${name === provider}" data-provider="${name}"`)).join("")}<button class="btn ghost" disabled title="Not available">KétRouter</button></div>
        ${locked ? '<p class="banner">Model changes are available when this conversation is idle and connected.</p>' : ""}
        <label class="search-field">${K.icon("search")}<input id="model-search" placeholder="Search models…" aria-label="Search models"></label>
        <div class="model-options">${catalog.models.map(name => `<label class="model-option"><input type="radio" name="model" value="${name}" ${name === selected ? "checked" : ""} ${locked ? "disabled" : ""}><span><b>${name}</b><small>${provider}${name === "Auto" ? " chooses the model for each turn" : name === "Opus 5.5" ? " · 1M context capacity" : ""}</small></span></label>`).join("")}</div>
        <p class="model-catalog-status" id="model-search-empty" role="status" hidden>No models found. Try another search.</p>
        ${catalog.efforts.length ? `<div class="effort-picker"><div class="row"><b>Reasoning effort</b><span class="spacer"></span><output id="effort-output">${effort}</output></div><input id="effort" type="range" min="0" max="${catalog.efforts.length - 1}" value="${Math.max(0,catalog.efforts.indexOf(effort))}" step="1" aria-label="Reasoning effort" ${locked ? "disabled" : ""}><div class="range-labels"><span>${catalog.efforts[0]}</span><span>${catalog.efforts.at(-1)}</span></div></div>` : '<p class="help">This sample model has no separate effort control.</p>'}${K.compactWindowMarkup(provider, selected)}`,
        `${K.button("Cancel", "close-dialog", null, "ghost")}${K.button(K.query.get("compactState") === "saving" ? "Applying…" : ["error", "conflict"].includes(K.query.get("compactState")) ? "Retry" : "Apply", "apply-provider", "check", "primary", locked || !K.compactWindow.validate(K.compactDraft).valid ? "disabled" : "")}`, "model-context-dialog");
};
K.usagePanel = () => {
    const reported = K.query.get("usage") !== "unknown" && K.current.turn !== "empty";
    const codex = K.current.provider === "Codex";
    const body = `<p class="muted">${K.escape(K.current.provider)} · ${K.escape(K.current.model)}</p><div class="usage-stat"><span>Current context window</span><strong>${reported ? "24%" : "Not reported"}</strong></div>${reported ? '<progress value="24" max="100" aria-label="Current context occupancy">24%</progress><p class="help">32,768 of 136,533 tokens · sample provider snapshot</p>' : '<p class="help">No exact current-window value has been reported. Cumulative usage is not used as an estimate.</p>'}<div class="settings-divider"></div><h3>Account limits</h3><div class="usage-stat"><span>5-hour usage</span><b>${codex ? "18%" : "Unavailable"}</b></div><div class="usage-stat"><span>Weekly usage</span><b>${codex ? "32%" : "Unavailable"}</b></div><p class="help">Account limits belong to the selected provider, not this conversation.</p>${K.query.get("compact") === "done" ? '<p role="status">Context compacted · simulated acknowledgment</p>' : ""}`;
    const footer = `${K.button("Compact session", "compact-session", "layers", "ghost", !codex || !reported || K.configurationLocked() ? "disabled" : "")}`;
    return `<aside class="usage-popover" role="region" aria-label="Context and limits"><header><h3>Context & limits</h3>${K.ib("x", "Close context panel", "close-usage")}</header>${body}<footer>${footer}</footer></aside>`;
};
K.newConversationDialog = () => K.modal("New conversation", `<p class="muted">Choose where this conversation will work.</p><label for="base-worktree">Base worktree</label><select id="base-worktree">${Object.values(K.demo.scopes).filter(s => s.repo === K.repo && !s.deleted && !s.archived).map(s => `<option value="${K.escape(s.tree)}" ${s.tree === K.tree ? "selected" : ""}>${K.escape(s.tree)}</option>`).join("")}</select><label for="new-branch">Create new worktree <span class="muted">(optional)</span></label><input id="new-branch" placeholder="feature/navigation" autocomplete="off"><p class="help">Leave blank to use the selected worktree. A new branch starts from that checkout’s HEAD.</p><p id="new-conversation-error" class="error-text" role="alert">${K.query.get("creation") === "error" ? "The worktree could not be created. Choose a different branch and retry." : ""}</p>`, `${K.button("Cancel", "close-dialog", null, "ghost")}${K.button(K.query.get("creation") === "pending" ? "Creating…" : "Create conversation", "confirm-new-conversation", "plus", "primary", K.query.get("creation") === "pending" ? "disabled" : "")}`);
K.productDialog = () => {
    const dialog = K.query.get("dialog");

    if (dialog === "execution-worktree") return K.modal("Files and Source Control worktree", `<div class="execution-worktree-content"><p>Choose the worktree for Files and Source Control. The agent keeps running independently.</p><p><b>Current · feat/customer-care-routine-workflow ✦</b></p><input aria-label="Search worktrees" placeholder="Search branch or folder" oninput="this.parentElement.querySelectorAll('[data-worktree-row]').forEach(row => row.hidden = !row.textContent.toLowerCase().includes(this.value.toLowerCase()))"><p class="muted">Model-touched first · 1,248 worktrees</p><div class="execution-worktree-list">${["feat/customer-care-routine-workflow ✦ · Current", "docs/cosmetic-user-guide ✦", "feat/core-ui ✦", ...Array.from({length: 80}, (_, i) => `feature/worktree-${i + 1}`)].map((branch, i) => `<button data-worktree-row class="btn ghost" data-action="close-dialog"><span>${K.escape(branch)}<small>${i === 0 ? "ketviet-care-routine-implementation" : "ketviet-" + i}</small></span></button>`).join("")}</div><p class="help">Earlier files and running Atlases keep their source worktree.</p></div>`, K.button("Close", "close-dialog", null, "ghost"));
    if (dialog === "new-conversation") return K.newConversationDialog();
    if (dialog === "rename") return K.modal("Rename conversation", `<label for="conversation-title">Name</label><input id="conversation-title" maxlength="100" value="${K.escape(K.current.title)}">`, `${K.button("Cancel", "close-dialog", null, "ghost")}${K.button("Save name", "rename-conversation", "check", "primary")}`);
    if (dialog === "fork") return K.modal("Fork this conversation", `<p>Continue independently from <b>${K.escape(K.current.title)}</b>.</p><p class="help">The fork keeps the provider, model and worktree. It does not own the worktree and does not start a turn.</p>${K.forkBlocker() ? `<p role="status">${K.forkBlocker()}</p>` : ""}${K.query.get("fork") === "error" ? '<p class="error-text" role="alert">Fork failed. Your original conversation is unchanged. Retry when ready.</p>' : ""}`, `${K.button("Cancel", "close-dialog", null, "ghost")}${K.button(K.query.get("fork") === "pending" ? "Forking…" : "Create fork", "confirm-fork", "git-branch", "primary", K.forkBlocker() || K.query.get("fork") === "pending" ? "disabled" : "")}`);
    return "";
};

const settingsWithArchive = K.views.settings;
K.views.settings = () => {
    if (K.query.get("tab") !== "providers") return settingsWithArchive();
    return `<section class="view"><div class="page-content settings-page"><div class="page-heading"><div><h1>Providers & browser</h1><p>Use the agents installed on your computer.</p></div></div><div class="provider-settings">${Object.entries(K.providers).map(([name, data]) => `<section class="provider-setting"><div class="row"><h2>${name}</h2><span class="spacer"></span>${K.badge("Ready", "success")}</div><p>${name === "Claude" ? "Bundled Agent SDK sidecar" : "Locally installed CLI"} · sample runtime status</p><dl><dt>Models</dt><dd>${data.models.join(", ")}</dd><dt>Conversation fork</dt><dd>${data.fork ? "Available for idle sessions" : "Not supported"}</dd><dt>Send guidance</dt><dd>${data.steer ? "Available during a turn" : "Not supported"}</dd></dl></section>`).join("")}<section class="provider-setting"><h2>Browser tools</h2><label class="choice"><input type="checkbox" id="browser-enabled" ${K.demo.browserEnabled !== false ? "checked" : ""}><span>Enable browser tools for Codex, Claude and Cursor</span></label><p class="help">Uses an isolated browser profile. Changes apply to idle Codex processes and the next Claude or Cursor turn.</p>${K.button("Save preference", "save-provider-settings", "check", "primary")}</section></div>${K.link("settings", "Back to appearance", "arrow-left", {}, "btn ghost")}</div></section>`;
};
