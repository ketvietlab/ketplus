/* Local prototype actions. No Git, provider, account or network calls. */
Object.assign(handlers, {
    "retry-group-fixture": () => reload({ groupDetails: "updated" }),
    "provider-tab": button => reload({ picker: button.dataset.provider }),
    "apply-provider": () => {
        if (K.compactWindow.modelBlocked()) return;
        const provider = K.query.get("picker") || K.current.provider;
        const compact = K.validateCompactDraft();
        if (!compact.valid || (provider !== "Cursor" && K.compactWindow.blocker(provider))) return;
        K.current.compactWindow = provider === "Cursor" ? null : compact.value;
        K.current.provider = provider;
        K.current.model = document.querySelector('input[name="model"]:checked').value;
        K.current.effort = document.querySelector("#effort-output")?.value || "";
        reload({ provider, model: K.current.model, effort: K.current.effort, compactWindow: K.current.compactWindow ?? "auto", compactState: null, dialog: null, picker: null });
    },
    "select-mode": button => {
        if (K.configurationLocked()) return;
        K.current.mode = button.dataset.mode;
        reload({ mode: K.current.mode, permission: K.current.mode, menu: null });
    },
    usage: button => {
        if (document.querySelector(".usage-popover")) { handlers["close-usage"](); return; }
        button.closest(".usage-anchor").insertAdjacentHTML("beforeend", K.usagePanel());
        button.setAttribute("aria-expanded", "true");
        K.query.set("dialog", "usage");
    },
    "close-usage": () => {
        document.querySelector(".usage-popover")?.remove();
        const ring = document.querySelector(".usage-ring");
        ring?.setAttribute("aria-expanded", "false");
        K.query.delete("dialog");
        ring?.focus();
    },
    "compact-session": () => {
        if (K.current.provider !== "Codex" || K.configurationLocked() || K.query.get("usage") === "unknown") return;
        K.query.set("compact", "done");
        document.querySelector(".usage-popover").outerHTML = K.usagePanel();
    },
    "toggle-repository": button => {
        K.demo.collapsedRepositories ||= [];
        const repo = button.dataset.repo;
        K.demo.collapsedRepositories = K.demo.collapsedRepositories.includes(repo)
            ? K.demo.collapsedRepositories.filter(id => id !== repo)
            : [...K.demo.collapsedRepositories, repo];
        reload();
    },
    "expand-conversations": button => {
        K.demo.expandedRepositories ||= [];
        const repo = button.dataset.repo;
        K.demo.expandedRepositories = K.demo.expandedRepositories.includes(repo)
            ? K.demo.expandedRepositories.filter(id => id !== repo)
            : [...K.demo.expandedRepositories, repo];
        reload();
    },
    "new-chat": button => {
        const repo = button?.dataset.repo || K.repo;
        if (K.repositoryMachine(repo) !== "local" && K.repositoryHost(repo)?.status !== "connected") return;
        K.go("workspace", { repo, dialog: "new-conversation" });
    },
    "open-atlas-panel": () => reload({ panel: K.query.get("panel") === "atlas" ? null : "atlas" }),
    "atlas-edit": button => {
        if (K.repositoryMachine(K.repo) !== "local" && K.current.connection !== "connected") return;
        const id = button.dataset.atlasId;
        const atlas = K.atlasEntries().find(item => item.id === id);
        if (!atlas) return;
        K.go("atlas-builder", { bundle: id, project: atlas.title, fromConversation: K.activeConversation, panel: null, chat: "collapsed" });
    },
    "close-atlas-editor": () => K.go("workspace", {
        conversation: K.query.get("fromConversation"), panel: "atlas",
        fromConversation: null, chat: null, bundle: null, project: null,
    }),
    "atlas-open": button => {
        if (K.repositoryMachine(K.repo) !== "local" && K.current.connection !== "connected") return;
        const id = button.dataset.atlasId;
        if (!K.atlasEntries().some(item => item.id === id)) return;
        if (K.activeAtlasSessions().includes(id)) {
            K.toast("Open the existing Atlas browser window · demo.");
            return;
        }
        K.scope.atlasStarting = id;
        K.scope.atlasEvent = "";
        reload({ panel: "atlas", atlasView: "starting" });
    },
    "atlas-stop": button => {
        const id = button.dataset.atlasId;
        const scope = button.dataset.atlasScope
            ? K.demo.scopes[button.dataset.atlasScope]
            : K.scope;
        if (!scope || !(scope === K.scope ? K.activeAtlasSessions() : scope.atlasSessions || []).includes(id)) return;
        scope.atlasStopping = id;
        reload({ panel: "atlas", atlasView: scope === K.scope ? "stopping" : null });
    },
    "atlas-retry": () => reload({ panel: "atlas", atlasView: null }),
    "atlas-new": () => {
        const prompt = document.querySelector("#prompt");
        if (!prompt) return;
        prompt.value = "Create an Atlas for this conversation.";
        prompt.dispatchEvent(new Event("input", { bubbles: true }));
        prompt.focus();
    },
    "confirm-new-conversation": () => {
        if (K.demo.operation) return;
        const base = document.querySelector("#base-worktree").value;
        const branch = document.querySelector("#new-branch").value.trim();
        const tree = branch.replaceAll("/", "-") || base;
        const invalid = branch && (!/^[a-zA-Z0-9][a-zA-Z0-9/_-]*$/.test(branch) || branch.includes("//") || branch.endsWith("/"));
        if (invalid || (branch && K.demo.scopes[K.scopeKey(tree)])) {
            document.querySelector("#new-conversation-error").textContent = invalid ? "Enter a valid branch, such as feature/navigation." : "That branch or worktree already exists.";
            return;
        }
        K.queue("new-conversation", { tree, base, branch, provider: K.current.provider, model: K.current.model, effort: K.current.effort, mode: K.current.mode });
        reload({ creation: "pending" });
    },
    "rename-prompt": () => reload({ dialog: "rename" }),
    "rename-conversation": () => {
        const title = document.querySelector("#conversation-title").value.trim();
        if (!title) return;
        K.current.title = title;
        reload({ dialog: null });
    },
    "fork-prompt": () => reload({ dialog: "fork" }),
    "confirm-fork": () => {
        if (K.forkBlocker() || K.demo.operation) return;
        K.queue("fork");
        reload({ fork: "pending" });
    },
    "save-provider-settings": () => {
        K.demo.browserEnabled = document.querySelector("#browser-enabled").checked;
        K.toast("Browser preference saved for the next provider turn · demo.");
    },
});
document.addEventListener("input", event => {
    if (event.target.id === "atlas-conversation-search") {
        const query = event.target.value.trim().toLocaleLowerCase();
        let visible = 0;
        for (const row of document.querySelectorAll("[data-atlas-row]")) {
            row.hidden = !row.dataset.atlasTitle.includes(query);
            if (!row.hidden) visible += 1;
        }
        const empty = document.querySelector(".atlas-filter-empty");
        if (empty) empty.hidden = visible > 0;
    }
    if (event.target.id === "prompt" && K.isActiveTurn()) {
        const send = document.querySelector('#prompt-form button[type="submit"]');
        if (send) send.hidden = !K.provider().steer || K.current.turn === "question" || !event.target.value.trim();
    }
    if (event.target.id === "effort") {
        const catalog = K.providers[K.query.get("picker") || K.current.provider];
        document.querySelector("#effort-output").value = catalog.efforts[Number(event.target.value)];
    }
});


document.addEventListener("click", event => {
    if (document.querySelector(".usage-popover") && !event.target.closest(".usage-anchor")) {
        document.querySelector(".usage-popover").remove();
        document.querySelector(".usage-ring")?.setAttribute("aria-expanded", "false");
        K.query.delete("dialog");
    }
});
document.addEventListener("keydown", event => {
    if (event.key === "Escape" && document.querySelector(".usage-popover")) handlers["close-usage"]();
});
if (K.page === "workspace" && K.query.get("atlasView") === "starting" && K.scope.atlasStarting) {
    setTimeout(() => {
        const id = K.scope.atlasStarting;
        K.scope.atlasStarting = "";
        K.scope.atlasSessions = [...new Set([...(K.scope.atlasSessions || []), id])];
        reload({ panel: "atlas", atlasView: null });
    }, 650);
}
if (K.page === "workspace" && Object.values(K.demo.scopes).some(scope => scope.atlasStopping)) {
    setTimeout(() => {
        for (const scope of Object.values(K.demo.scopes)) {
            if (!scope.atlasStopping) continue;
            const id = scope.atlasStopping;
            scope.atlasStopping = "";
            scope.atlasSessions = (scope.atlasSessions || []).filter(item => item !== id);
            scope.atlasEvent = "owner-stop";
        }
        reload({ panel: "atlas", atlasView: null });
    }, 650);
}
