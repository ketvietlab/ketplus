const reload = (query = {}) =>
    K.go(K.page, { ...Object.fromEntries(K.query), state: K.state, ...query });
const resourceQuery = (button) => ({
    kind: button?.dataset.kind || "conversation",
    target: button?.dataset.target || K.activeConversation,
    targetTree: button?.dataset.tree || K.tree,
    targetRepo: button?.dataset.repo || K.repo,
});
const queueResource = (kind) => {
    const target = K.resourceTarget();
    if (kind === "delete") {
        if (K.deletionBlocker(target)) return;
        if (document.querySelector("#delete-confirmation")?.value !== "DELETE") {
            K.toast("Type DELETE to confirm this action.");
            return;
        }
    }
    if (kind === "restore" && target.scope.deleted) return;
    K.queue(kind, {
        kind: target.kind,
        id: target.id,
        scope: K.scopeKey(target.scope.tree, target.scope.repo),
    });
    reload({ state: "resource-pending" });
};
Object.assign(handlers, {
    "open-mode-menu": () => reload({ menu: K.query.get("menu") === "mode" ? null : "mode" }),
    "select-mode": (button) => reload({ mode: button.dataset.mode, menu: null }),
    "new-chat": () => K.newConversation(),
    "create-atlas": () => {
        const id = `draft-atlas-${K.demo.next++}`;
        K.demo.bundles ||= {};
        K.demo.bundles[id] = `New Atlas ${K.demo.next - 1}`;
        K.go("atlas-builder", {
            bundle: id,
            project: K.demo.bundles[id],
            state: "empty",
            hasAtlas: null,
            scope: null,
            requestFlow: null,
            brief: null,
        });
    },
    "choose-bundle": () => K.query.get("fromConversation")
        ? K.go("workspace", { conversation: K.query.get("fromConversation"), panel: "atlas", fromConversation: null, bundleState: null, bundle: null, project: null, chat: null })
        : K.go("atlas-library", { bundleState: null }),
    "validate-bundle": () => {
        K.current.bundleReady = true;
        reload({ hasAtlas: "1", bundleState: null });
    },
    terminal: () => {
        if (!K.current.available || K.scope.deleted ||
            (K.repositoryMachine(K.repo) !== "local" && K.current.connection !== "connected")) {
            K.toast("The original worktree is unavailable.");
            return;
        }
        K.go(K.page === "workspace" ? "workspace" : "editor", {
            panel: "terminal",
            file: K.query.get("file"),
            preview: K.query.get("preview"),
        });
    },
    stop: () => {
        if (!K.isActiveTurn()) return;
        K.queue("stop", { previous: K.current.turn });
        reload({ state: "stopping" });
    },
    retry: () => {
        if (K.page === "atlas-builder" && K.current.connection === "connected") {
            K.submitPrompt();
            return;
        }
        K.queue("reconnect");
        reload({ state: "reconnecting" });
    },
    "complete-turn": () => {
        K.current.turn = "ready";
        reload({ state: "ready", answer: null });
    },
    "ask-question": () => reload({ state: "question", question: "1" }),
    "refresh-question": () => reload({ state: "running", request: null, answer: "received" }),
    "question-back": () => {
        K.current.answer = K.current.firstAnswer || "";
        reload({ state: "question", question: "1" });
    },
    "retry-answer": () => {
        K.queue("answer");
        reload({ state: "answer-submitting", question: "2" });
    },
    "retry-steer": () => K.submitPrompt(),
    "open-signin": () => reload({ dialog: "signin" }),
    "open-remote-connections": () => K.go("connections", { remote: K.demo.remoteHosts?.length ? "offline" : "empty" }),
    "add-connection": () => reload({ dialog: "add-connection" }),
    "pair-device": () => {
        if (K.demo.auth !== "signed-in") return;
        K.demo.ticket = "pending";
        reload({ state: "ready", dialog: "pair" });
    },
    "new-pair-code": () => {
        K.demo.ticket = "pending";
        reload({ state: "ready", dialog: "pair" });
    },
    "simulate-scan": () => {
        if (K.demo.ticket !== "pending" && K.query.has("demo")) return;
        K.demo.ticket = "claimed";
        reload({ dialog: "confirm" });
    },
    "confirm-device": () => {
        if (K.demo.auth !== "signed-in") return;
        if (!K.query.has("demo")) K.demo.ticket = "claimed";
        K.queue("confirm");
        reload({ state: "confirming", dialog: "confirm" });
    },
    "expire-pair": () => {
        K.demo.ticket = "expired";
        reload({ dialog: "expired" });
    },
    "device-details": (button) => reload({ device: button.dataset.device, dialog: "details" }),
    "iphone-details": () => reload({ device: "iphone", dialog: "details" }),
    "revoke-prompt": (button) =>
        reload({ device: button?.dataset.device || K.deviceId(), dialog: "revoke" }),
    "revoke-device": () => {
        K.queue("revoke", { device: K.deviceId() });
        reload({ state: "revoking", dialog: "revoke" });
    },
    reconnect: () => {
        K.queue("connection");
        reload({ state: "reconnecting" });
    },
    signout: () => reload({ dialog: "signout" }),
    "confirm-signout": () => {
        K.demo.auth = "signed-out";
        K.demo.ticket = "cancelled";
        for (const host of K.demo.remoteHosts || []) host.status = "offline";
        delete K.demo.operation;
        reload({ state: "signed-out", dialog: null });
    },
    "cancel-pair-retry": () => {
        K.demo.ticket = "cancelled";
        delete K.demo.operation;
        reload({ state: "ready", dialog: null, outcome: null });
    },
    "close-dialog": () => {
        const stage = K.query.get("dialog");
        if (K.demo.operation && !["confirm", "signin", "pair"].includes(stage)) {
            K.toast("Waiting for the operation acknowledgment.");
            return;
        }
        if (K.page === "connections" && ["pair", "confirm", "expired"].includes(stage)) {
            delete K.demo.operation;
            K.demo.ticket =
                K.query.get("outcome") === "error" ? "cancellation-pending" : "cancelled";
            if (K.demo.ticket === "cancellation-pending") {
                reload({ dialog: "cancel-error", state: "ready" });
                return;
            }
        }
        if (stage === "signin") delete K.demo.operation;
        reload({ dialog: null, state: K.page === "connections" ? "ready" : K.state });
    },
    "manage-worktree": (button) =>
        reload({
            dialog: "manage",
            kind: "worktree",
            target: button.dataset.tree,
            targetTree: button.dataset.tree,
            targetRepo: K.repo,
        }),
    "archive-prompt": () => reload({ dialog: "archive" }),
    "delete-prompt": () => reload({ dialog: "delete" }),
    "archive-confirm": () => queueResource("archive"),
    "delete-confirm": () => queueResource("delete"),
    "archive-restore": (button) => reload({ ...resourceQuery(button), dialog: "restore-archive" }),
    "archive-delete": (button) => reload({ ...resourceQuery(button), dialog: "delete" }),
    "archive-restore-confirm": () => queueResource("restore"),
    "archive-stop": (button) => {
        const scope = K.ensureScope(button.dataset.tree);
        const record = scope.conversations[button.dataset.target];
        if (!record || !K.isActiveTurn(record)) return;
        K.queue("stop", { previous: record.turn });
        K.demo.operation.scope = K.scopeKey(scope.tree);
        K.demo.operation.conversation = record.id;
        record.turn = "stopping";
        reload();
    },
});
// Generated anchors capture fresh synthetic state at click time, not at initial render.
document.addEventListener(
    "click",
    (event) => {
        const anchor = event.target.closest("a[href]");
        if (
            !anchor ||
            event.button !== 0 ||
            event.metaKey ||
            event.ctrlKey ||
            event.shiftKey ||
            event.altKey
        )
            return;
        const href = anchor.getAttribute("href");
        const destination = new URL(href, location.href);
        const match = /^([a-z-]+)\.html$/.exec(destination.pathname.split("/").at(-1));
        if (!match) return;
        event.preventDefault();
        event.stopImmediatePropagation();
        const query = Object.fromEntries(destination.searchParams);
        delete query.demo;
        K.go(match[1], query);
    },
    true,
);
document.addEventListener("input", (event) => {
    if (event.target.id === "prompt") {
        K.current.draft = event.target.value;
        K.syncComposerSend();
    }
    if (event.target.id === "question-answer" && event.target.type !== "password")
        K.current.answer = event.target.value;
});
document.addEventListener("change", (event) => {
    if (event.target.id !== "machine-select") return;
    const machine = event.target.value;
    if (machine !== "local" && !K.demo.remoteHosts?.some(host => host.id === machine)) return;
    if (!["workspace", "atlas-builder"].includes(K.page)) {
        K.go(K.page, { ...Object.fromEntries(K.query), machine });
        return;
    }
    const repo = K.repositoryCatalog.filter(item => K.repositoryMachine(item.id) === machine)
        .sort((left, right) => K.repositoryRecency(right.id) - K.repositoryRecency(left.id)
            || left.label.localeCompare(right.label))[0];
    if (!repo) {
        K.go(K.page, { machine });
        return;
    }
    const latest = K.repositoryRecords(repo.id)[0];
    K.go(latest?.record.page || "workspace", { machine, repo: repo.id,
        tree: latest?.scope.tree || "develop", conversation: latest?.record.id || null,
        state: latest?.record.turn || "empty" });
});
const finishOperation = () => {
    K.captureDraft();
    if (!K.advanceOperation()) return;
    const prompt = document.querySelector("#prompt");
    if (prompt) prompt.value = K.current.draft;
    if (K.state === "conversation-created") {
        const created = K.demo.createdConversation;
        K.go("workspace", { conversation: created.id, tree: created.tree, repo: created.repo, state: created.state });
        return;
    }
    if (K.state === "created") {
        K.go("workspace", { tree: K.demo.createdTree, state: "empty", conversation: "draft" });
        return;
    }
    if (K.state === "resource-done") {
        K.go("settings", { tab: "archived", saved: K.query.get("saved") });
        return;
    }
    if (K.page === "connections" && K.demo.auth === "signed-in" &&
        K.query.get("remote") === "auth-required") {
        reload({ remote: "empty", dialog: null, outcome: null, play: null });
        return;
    }
    reload({ outcome: null, play: null });
};
if (K.demo.operation)
    setTimeout(finishOperation, Math.max(0, K.demo.operation.due - Date.now() + 20));
if (K.state === "resource-done") K.go("settings", { tab: "archived", saved: K.query.get("saved") });
if (K.state === "created")
    K.go("workspace", { tree: K.demo.createdTree, state: "empty", conversation: "draft" });
if (K.state === "restore") setTimeout(() => reload({ state: K.current.turn }), 900);
