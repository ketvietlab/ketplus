/* Shared Desktop/iPhone presenter for the existing static native-app prototype. */
(() => {
    const M = window.RepositoryPickerModel;
    const state = M.createState(K.query);
    const esc = K.escape, icon = K.icon;
    const phone = () => document.body.dataset.platform === "iphone" || matchMedia("(width < 48rem)").matches;
    const button = (text, action, name, extra = "") => `<button type="button" class="btn ghost" data-rp-action="${action}" ${extra}>${name ? icon(name) : ""}${text}</button>`;
    const iconButton = (name, label, action) => `<button type="button" class="btn ghost icon-btn" aria-label="${label}" data-rp-action="${action}">${icon(name)}</button>`;
    const tree = () => M.getTree(state.tree);
    const busy = () => Boolean(state.pending) || ["offline", "loading", "stale"].includes(state.scenario);
    const repositories = () => M.rankedRepositories({ ...state, repoQuery: "" });

    function sidebar() {
        return `<div class="rp-sidebar"><a href="workspace.html" class="rp-brand"><img src="../assets/brand/ketplus-v1/ketplus-mark-transparent.png" alt="" />KétPlus <small>${phone() ? "Mobile" : "Desktop"}</small></a>
            <a class="rp-quick" href="search.html">${icon("search")}Quick search <kbd>⌘ K</kbd></a>
            <nav class="rp-navigation" aria-label="Main navigation">${[["workspace", "Agent Workspace", "messages-square"], ["editor", "Files & Editor", "folder"], ["changes", "Source Control", "git-branch"], ["connections", "Connections", "connect"]].map(([route, title, glyph]) => `<a href="${route}.html?repo=${encodeURIComponent(state.repo)}&amp;tree=${encodeURIComponent(tree().branch)}" ${route === "workspace" ? 'aria-current="page"' : ""}>${icon(glyph)}${title}</a>`).join("")}</nav>
            <div class="rp-machine">${icon("monitor")}This Mac <span>Connected</span></div>
            <nav class="rp-repositories" aria-label="Repositories and conversations">${repositories().map(repo => {
                const selected = repo.id === state.repo;
                const collapsed = state.collapsed.includes(repo.id);
                return `<section data-repository-group="${repo.id}"><button class="rp-group-title" data-rp-action="collapse" data-repo="${repo.id}" aria-expanded="${!collapsed}">${esc(repo.id)}${icon(collapsed ? "chevron-right" : "chevron-down")}</button>${collapsed ? "" : `<div class="rp-conversation-list">${selected ? `<button class="rp-conversation" data-rp-action="conversation" aria-current="true" data-conversation-id="${state.conversationId}">${icon(state.running ? "loader-circle" : "circle-check")}<span>${esc(state.title)}</span></button>` : ""}${repo.conversations.map(title => `<a href="workspace.html?repo=${repo.id}" class="rp-conversation">${icon("circle-check")}<span>${esc(title)}</span></a>`).join("")}</div>`}</section>`;
            }).join("")}</nav><div class="rp-account"><span class="rp-avatar">AN</span><div>An Nguyen<small>Personal workspace</small></div></div></div>`;
    }

    function workspace() {
        const current = tree();
        const running = M.getTree(state.agentTree);
        const mismatch = state.running && running.id !== current.id;
        return `<div class="rp-workspace"><div class="rp-phone-status" aria-hidden="true"><b>9:41</b><span>●●●  ▰</span></div>
            <header class="rp-header">${iconButton("panel-left", "Open conversations", "drawer")}<div class="rp-heading"><div><strong>${esc(state.title)}</strong>${state.running ? '<span class="rp-working">Working</span>' : ""}</div><button id="rp-opener" data-rp-action="open" class="rp-scope" aria-haspopup="dialog" aria-label="Choose repository and worktree">${icon("git-branch")}<span>${esc(state.repo)}<span class="rp-scope-divider"> / </span>${esc(current.branch)}</span>${icon("chevron-down")}</button></div>${iconButton("ellipsis", "Conversation options", "open")}</header>
            <div class="rp-transcript" data-conversation-id="${state.conversationId}"><div class="rp-transcript-inner"><div class="rp-user-message">Check the KétView pilot and its deployment.</div><p>I’m checking the pilot’s control plane and native session flow.</p><div class="rp-activity">${icon("terminal")}Ran 2 commands ${icon("chevron-right")}</div><p>The agent is working in <code>sample-project-ketview-pilot</code>, on <code>feat/sample-preview-pilot</code>.</p><div class="rp-file">${icon("file-code")}deployment.ts <span>sample-project-ketview-pilot</span></div><p>I’ll keep the existing changes and check the next deployment step.</p><div class="rp-live">${icon(state.running ? "loader-circle" : "circle-check")}${state.running ? "Working…" : "Turn complete"}</div></div></div>
            <div class="rp-composer-wrap">${mismatch ? `<p class="rp-execution-note">${icon("git-branch")}Current turn: ${esc(running.repo)} · ${esc(running.branch)}<br />Next turn: ${esc(state.repo)} · ${esc(current.branch)}</p>` : ""}<div class="rp-composer"><textarea id="rp-draft" aria-label="Message draft" rows="2">${esc(state.draft)}</textarea><div class="rp-composer-tools">${icon("plus")}<span>${icon("shield")}Full access</span><span class="rp-model">Codex · GPT-6.1-Sol</span>${icon("circle")}</div></div><div id="rp-receipt" class="rp-receipt" role="status" aria-live="polite">${esc(state.receipt)}</div></div></div>`;
    }

    function repositoryPane() {
        const list = M.rankedRepositories(state);
        return `<section class="rp-repo-pane" aria-label="Choose repository"><h3 id="rp-repository-heading" class="sr-only" tabindex="-1">Repository</h3><label class="rp-search">${icon("search")}<input id="rp-repo-search" type="search" aria-label="Search repositories" placeholder="Search repositories…" value="${esc(state.repoQuery)}" autocomplete="off" /></label><div class="rp-repo-list" role="list" aria-label="Repository choices">${list.map(repo => {
            const count = M.worktrees.filter(item => item.repo === repo.id).length;
            return `<div role="listitem"><button class="rp-choice" data-repo-choice="${repo.id}" title="${esc(repo.path)}" aria-pressed="${repo.id === state.repo}" ${busy() ? "disabled" : ""}><span class="rp-choice-icon">${icon("folder")}</span><span class="rp-choice-copy"><strong>${esc(repo.id)}</strong></span>${repo.id === "sample-project" ? `<span class="rp-agent-mark" title="Agent working here">${icon("sparkles")}<span class="sr-only">Agent working here</span></span>` : ""}<span class="rp-count" title="${count} worktree${count === 1 ? "" : "s"}">${count}<span class="sr-only"> worktree${count === 1 ? "" : "s"}${repo.id === state.repo ? ", current" : ""}</span></span></button></div>`;
        }).join("")}${state.repoQuery && list.length === 1 ? '<p class="rp-empty-copy">No other repositories match.</p>' : ""}</div></section>`;
    }

    function worktreePane() {
        const list = M.visibleWorktrees(state);
        return `<section class="rp-tree-pane" aria-label="Choose worktree"><h3 id="rp-worktree-heading" class="sr-only" tabindex="-1">Worktree in ${esc(state.repo)}</h3><label class="rp-search">${icon("search")}<input id="rp-tree-search" type="search" aria-label="Search worktrees in ${esc(state.repo)}" placeholder="Search branch or folder…" value="${esc(state.treeQuery)}" autocomplete="off" /></label><p class="rp-result-count sr-only" aria-live="polite">${list.length} worktree${list.length === 1 ? "" : "s"} in ${esc(state.repo)}</p><div class="rp-tree-list" role="list" aria-label="Worktree choices">${list.length ? list.map(item => `<div role="listitem"><button class="rp-choice rp-tree-choice" data-tree-choice="${item.id}" aria-pressed="${item.id === state.tree}" ${busy() ? "disabled" : ""}>${icon("git-branch")}<span class="rp-choice-copy"><span class="rp-tree-name"><strong>${esc(item.branch)}</strong>${item.main ? '<span class="rp-chip">Main</span>' : ""}${item.used ? '<span class="rp-chip rp-agent-tag">Agent</span>' : ""}</span><small>${esc(item.path)}</small></span>${item.id === state.tree ? icon("check") : ""}</button></div>`).join("") : `<div class="rp-empty">${icon("search")}<strong>No worktrees match “${esc(state.treeQuery)}”</strong><p>Search is scoped to ${esc(state.repo)}. Choose another repository to look there.</p>${button("Clear search", "clear-search", "x")}</div>`}</div></section>`;
    }

    function feedback() {
        if (state.pending) return `<div class="rp-feedback" role="status">${icon("loader-circle")}Switching to ${esc(state.pending.repo)}…</div>`;
        if (state.failed) return `<div class="rp-feedback rp-error" role="alert">${icon("circle-alert")}<span>Couldn’t switch. Still in ${esc(state.repo)}.</span>${button("Retry", "retry", "rotate-cw")}</div>`;
        if (state.scenario === "offline") return `<div class="rp-feedback" role="status">${icon("wifi-off")}<span>Mac is offline. Your current repository is unchanged.</span>${button("Reconnect", "refresh", "rotate-cw")}</div>`;
        if (state.scenario === "stale") return `<div class="rp-feedback" role="status">${icon("circle-alert")}<span>Repository list changed. Refresh to choose again.</span>${button("Refresh", "refresh", "rotate-cw")}</div>`;
        return "";
    }

    function picker() {
        const mobile = phone();
        const agent = M.getTree(state.agentTree);
        return `<dialog id="rp-dialog" class="rp-dialog" aria-labelledby="rp-dialog-title"><div class="rp-sheet-handle" aria-hidden="true"></div><header class="rp-dialog-header"><h2 id="rp-dialog-title">Repository &amp; worktree</h2>${iconButton("x", "Close repository picker", "close")}</header>
            ${mobile ? `<div class="rp-step-bar">${state.step === "worktrees" ? button("Repositories", "back", "chevron-left") : '<span>Choose a repository</span>'}<span>${state.step === "repositories" ? "1" : "2"} of 2</span></div>` : ""}
            ${state.scenario === "loading" ? `<div class="rp-loading" role="status">${icon("loader-circle")}Loading repositories…${button("Try again", "refresh", "rotate-cw")}</div>` : `<div class="rp-picker-columns" data-step="${state.step}">${repositoryPane()}${worktreePane()}</div>`}
            ${state.running && agent.repo !== state.repo && state.scenario === "ready" && !state.pending ? `<div class="rp-agent-hint">${icon("sparkles")}<span>Agent is working in <strong>${esc(agent.repo)}</strong>.</span>${button("Use this repository", "use-agent", "arrow-up-right")}</div>` : ""}${feedback()}</dialog>`;
    }

    function render(focusId) {
        const active = document.activeElement;
        const focus = focusId || active?.id;
        const selection = active && "selectionStart" in active ? active.selectionStart : null;
        document.getElementById("app").innerHTML = K.template("shell", { sidebar: sidebar(), main: workspace() });
        const overlay = document.getElementById("rp-overlay");
        overlay.innerHTML = state.open ? picker() : state.drawer ? `<dialog class="rp-drawer" id="rp-drawer" aria-label="Conversations">${iconButton("x", "Close conversations", "close-drawer")}${sidebar()}</dialog>` : "";
        const dialog = overlay.querySelector("dialog");
        if (dialog) {
            dialog.addEventListener("cancel", event => { event.preventDefault(); state.open = state.drawer = false; render("rp-opener"); });
            dialog.showModal();
            dialog.addEventListener("click", event => { if (event.target === dialog) { const b = dialog.getBoundingClientRect(); if (event.clientX < b.left || event.clientX > b.right || event.clientY < b.top || event.clientY > b.bottom) { state.open = state.drawer = false; render("rp-opener"); } } });
        }
        const target = focus && document.getElementById(focus);
        if (target && (!state.open || dialog?.contains(target))) {
            target.focus({ preventScroll: true });
            if (selection !== null && ["textarea", "input"].includes(target.localName)) target.setSelectionRange(selection, selection);
        }
    }

    function switchTo(repo, treeId) {
        if (repo === state.repo && (!treeId || treeId === state.tree)) { state.step = "worktrees"; render(phone() ? "rp-worktree-heading" : undefined); return; }
        const request = M.beginSwitch(state, repo, treeId);
        if (request === null) return;
        render();
        setTimeout(() => {
            const success = state.scenario !== "error";
            M.completeSwitch(state, request, success);
            if (success) {
                const url = new URL(location.href);
                url.searchParams.set("repo", state.repo); url.searchParams.set("worktree", state.tree);
                // Atlas static previews have opaque sandbox origins. URL rewriting is
                // optional there; acknowledging a choice must still update the screen.
                try { history.replaceState(null, "", url); }
                catch (error) { if (error.name !== "SecurityError") throw error; }
            }
            render(phone() && state.open && success ? "rp-worktree-heading" : undefined);
        }, 350);
    }

    document.addEventListener("input", event => {
        if (event.target.id === "rp-draft") state.draft = event.target.value;
        if (event.target.id === "rp-repo-search") { state.repoQuery = event.target.value; render("rp-repo-search"); }
        if (event.target.id === "rp-tree-search") { state.treeQuery = event.target.value; render("rp-tree-search"); }
    });
    document.addEventListener("click", event => {
        const repo = event.target.closest("[data-repo-choice]");
        const selectedTree = event.target.closest("[data-tree-choice]");
        if (repo && !repo.disabled) return switchTo(repo.dataset.repoChoice);
        if (selectedTree && !selectedTree.disabled) return switchTo(state.repo, selectedTree.dataset.treeChoice);
        const target = event.target.closest("[data-rp-action]");
        if (!target) return;
        switch (target.dataset.rpAction) {
            case "open": state.open = true; state.drawer = false; state.step = "repositories"; render(); break;
            case "close": state.open = false; render("rp-opener"); break;
            case "back": state.step = "repositories"; render("rp-repository-heading"); break;
            case "drawer": state.drawer = true; render(); break;
            case "conversation": case "close-drawer": state.drawer = false; render("rp-opener"); break;
            case "clear-search": state.treeQuery = ""; render("rp-tree-search"); break;
            case "refresh": state.scenario = "ready"; render(); break;
            case "retry": { const failed = state.failed; state.scenario = "ready"; if (failed) switchTo(failed.repo, failed.tree); break; }
            case "use-agent": switchTo(agentRepo(), state.agentTree); break;
            case "collapse": { const id = target.dataset.repo; state.collapsed = state.collapsed.includes(id) ? state.collapsed.filter(value => value !== id) : [...state.collapsed, id]; render(); break; }
        }
    });
    const agentRepo = () => M.getTree(state.agentTree).repo;
    window.RepositoryPickerPrototype = { state, render };
    render();
})();
