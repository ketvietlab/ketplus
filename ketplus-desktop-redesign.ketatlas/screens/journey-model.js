/* Acknowledgments are deterministic demo operations, not production API calls. */
K.suggestConversationTitle = text => {
    const matches = [...String(text || "").matchAll(/\b(?:latest\s+)?user request\s*:\s*/gi)];
    const source = matches.length ? text.slice(matches.at(-1).index + matches.at(-1)[0].length) : text;
    const line = String(source || "").split("\n").find(item => item.trim() && !item.trim().startsWith("<")) || "";
    const title = line.split("<")[0].replace(/^[\s#*>-]+/, "").trim().replace(/\s+/g, " ") || "New conversation";
    return title.length > 56 ? `${title.slice(0, 55).trim()}…` : title;
};
K.queue = (kind, payload = {}) => {
    K.captureDraft();
    K.demo.operation = {
        kind,
        payload,
        page: K.page,
        scope: K.scopeKey(),
        conversation: K.activeConversation,
        due: Date.now() + 900,
        outcome: K.query.get("outcome") || "success",
        epoch: K.demo.next++,
    };
};
K.advanceOperation = () => {
    const op = K.demo.operation;
    if (!op || op.due > Date.now()) return false;
    delete K.demo.operation;
    const scope = K.demo.scopes[op.scope];
    const record = scope?.conversations[op.conversation];
    const failed = op.outcome === "error";
    const sameRecord = record === K.current;
    const samePage = op.page === K.page;
    if (["fork", "new-conversation"].includes(op.kind)) {
        if (failed || !record || (op.kind === "fork" && (K.isActiveTurn(record) || !K.providers[record.provider || "Codex"].fork))) {
            if (sameRecord) K.query.set(op.kind === "fork" ? "fork" : "creation", "error");
            return true;
        }
        const tree = op.kind === "fork" ? scope.tree : op.payload.tree;
        const fresh = !K.demo.scopes[K.scopeKey(tree, scope.repo)];
        const destination = K.ensureScope(tree, scope.repo);
        if (fresh) destination.conversations = {};
        const id = `${op.kind === "fork" ? "fork" : "draft"}-${K.demo.next++}`;
        const created = op.kind === "fork" ? JSON.parse(JSON.stringify(record)) : K.makeConversation(id, "New conversation", "empty");
        Object.assign(created, { id, explicit: true, requestCount: 0, draft: "", attachment: null,
            lastTurnStartedAt: Date.now() });
        if (op.kind === "fork") Object.assign(created, { title: `${record.title} (fork)`, forkedFrom: record.id, ownsWorktree: false });
        else for (const key of ["provider", "model", "effort", "mode"]) created[key] = op.payload[key];
        destination.conversations[id] = created;
        destination.selected = id;
        if (sameRecord) {
            K.demo.createdConversation = { id, tree, repo: scope.repo, state: created.turn };
            K.state = "conversation-created";
            K.query.delete("dialog");
        }
        return true;
    }
    const result = (state) => {
        if (sameRecord && ["workspace", "atlas-builder"].includes(K.page)) K.state = state;
    };
    if (["send", "steer"].includes(op.kind) && record) {
        record.turn = failed
            ? op.payload.previous
            : record.page === "atlas-builder"
              ? "building"
              : "running";
        if (!failed) {
            if (op.kind === "send" && record.requestCount === 0 && record.title === "New conversation")
                record.title = K.suggestConversationTitle(op.payload.input);
            if (op.kind === "send") record.requestCount++;
            if (op.kind === "send" && op.payload.input.trim() === "Create an Atlas for this conversation.") {
                const next = K.demo.next++;
                scope.atlasBundles ||= {};
                scope.atlasBundles[`agent-atlas-${next}`] = `New Atlas ${next}`;
            }
            record.lastTurnStartedAt = Date.now();
            record.latestPrompt = op.payload.input;
            if (record.draft === op.payload.input) record.draft = "";
            record.attachment = null;
            record.scope = op.payload.scope || record.scope;
        }
        result(record.turn);
        if (sameRecord) K.query.set("send", failed ? "failed" : "received");
    }
    if (op.kind === "stop" && record) {
        record.turn = failed ? op.payload.previous : "stopped";
        result(record.turn);
        if (sameRecord) K.query.set("stop", failed ? "failed" : "acknowledged");
    }
    if (op.kind === "reconnect" && record) {
        record.connection = failed ? "offline" : "connected";
        result(failed ? "error" : record.turn);
    }
    if (op.kind === "answer" && record) {
        record.turn = failed ? "answer-error" : "running";
        result(record.turn);
        if (sameRecord) K.query.set("answer", failed ? "failed" : "received");
    }
    if (op.kind === "signin") {
        K.demo.auth = failed ? "signed-out" : "signed-in";
        if (samePage) K.state = failed ? "signin-error" : "ready";
        if (!failed) K.query.delete("dialog");
    }
    if (op.kind === "confirm") {
        if (!failed && K.demo.auth === "signed-in" && K.demo.ticket === "claimed") {
            if (!K.demo.devices.includes("pixel")) K.demo.devices.push("pixel");
            K.demo.ticket = "approved";
            K.query.delete("dialog");
            if (samePage) K.state = "paired";
        } else if (samePage) K.state = "confirm-error";
    }
    if (op.kind === "revoke") {
        if (!failed) {
            K.demo.devices = K.demo.devices.filter((id) => id !== op.payload.device);
            K.query.delete("dialog");
        }
        if (samePage) K.state = failed ? "revoke-error" : "revoked";
    }
    if (op.kind === "connection" && samePage) K.state = failed ? "offline" : "ready";
    if (op.kind === "create") {
        const id = op.payload.tree;
        if (failed) {
            if (samePage) K.state = "create-error";
        } else {
            const created = K.ensureScope(id);
            if (!created.created) {
                created.conversations = {
                    draft: K.makeConversation("draft", "New conversation", "empty"),
                };
                created.selected = "draft";
                created.created = true;
            }
            if (samePage) K.state = "created";
            K.demo.createdTree = id;
        }
    }
    if (["archive", "restore", "delete"].includes(op.kind)) {
        const targetScope = K.demo.scopes[op.payload.scope];
        const targetRecord =
            op.payload.kind === "worktree"
                ? targetScope
                : targetScope?.conversations[op.payload.id];
        const blocked =
            op.kind === "delete" &&
            K.deletionBlocker?.({
                kind: op.payload.kind,
                scope: targetScope,
                record: targetRecord,
            });
        if (failed || !targetRecord || blocked || (op.kind === "restore" && targetScope.deleted)) {
            if (samePage) K.state = "resource-error";
        } else {
            if (op.kind === "archive") targetRecord.archived = true;
            if (op.kind === "restore") targetRecord.archived = false;
            if (op.kind === "delete" && op.payload.kind === "conversation")
                delete targetScope.conversations[op.payload.id];
            if (op.kind === "delete" && op.payload.kind === "worktree") {
                targetScope.deleted = true;
                targetScope.archived = true;
                Object.values(targetScope.conversations).forEach((r) => {
                    r.archived = true;
                    r.available = false;
                });
            }
            if (samePage) K.state = "resource-done";
            K.query.set("saved", op.kind === "restore" ? "restored" : op.kind);
            K.query.delete("dialog");
        }
    }
    return true;
};
K.submitPrompt = () => {
    K.captureDraft();
    if (
        !K.current.available ||
        K.current.connection !== "connected" ||
        K.demo.operation ||
        K.state === "restore" ||
        K.scope.archived ||
        K.scope.deleted
    )
        return;
    if (!K.current.draft.trim() && !K.current.attachment) {
        K.toast("Enter a request or attach a file.");
        return;
    }
    const active = K.isActiveTurn();
    if (active && !K.provider().steer) return;
    K.queue(active ? "steer" : "send", {
        input: K.current.draft,
        previous: K.current.turn,
        scope: K.page === "atlas-builder" ? K.atlasScope() || "entire-atlas" : "",
    });
    K.go(K.page, {
        state: active ? "steering" : "sending",
        panel: K.query.get("panel") === "atlas" ? "atlas" : null,
        requestFlow: K.page === "atlas-builder" ? K.atlasScope() || "entire-atlas" : null,
        brief: K.page === "atlas-builder" ? K.current.draft : null,
        hasAtlas: K.page === "atlas-builder" ? (K.hasAtlasMap() ? "1" : null) : null,
    });
};
K.submitAnswer = () => {
    if (K.query.get("request") === "expired") return;
    const privateAnswer = K.query.get("secret") === "1";
    K.current.answer = privateAnswer
        ? "[Private answer supplied in demo]"
        : document.querySelector("#question-answer")?.value ||
          document.querySelector("input[name=answer]:checked")?.value ||
          document.querySelector("input[name=answer][type=hidden]")?.value ||
          "No additional constraints";
    if (privateAnswer) document.querySelector("#question-answer").value = "";
    if (K.query.get("questionMode") !== "confirm" && (K.query.get("question") || "1") === "1") {
        K.current.firstAnswer = K.current.answer;
        K.current.answer = "";
        K.go("workspace", { state: "question", question: "2" });
        return;
    }
    K.queue("answer");
    K.go("workspace", { state: "answer-submitting", question: "2" });
};
K.signIn = (form) => {
    if (!form.querySelector("#account").value.trim() || !form.querySelector("#password").value) {
        document.querySelector("#signin-error").textContent = "Enter both username and password.";
        return;
    }
    form.querySelector("#password").value = "";
    K.queue("signin");
    K.go("connections", { state: "signing-in", dialog: "signin" });
};
K.createWorktree = (form) => {
    const branch = form.querySelector("#branch").value.trim();
    const invalid =
        !branch ||
        /[\s~^:?*\[\\]/.test(branch) ||
        branch.includes("..") ||
        branch.includes("@{") ||
        branch.includes("//") ||
        branch.startsWith("-") ||
        branch.startsWith("/") ||
        branch.endsWith("/") ||
        branch.endsWith(".") ||
        branch.split("/").some((part) => part.startsWith(".") || part.endsWith(".lock"));
    const tree = branch.replaceAll("/", "-");
    if (invalid || K.demo.scopes[K.scopeKey(tree)]) {
        document.querySelector("#branch-error").textContent = invalid
            ? "Invalid branch name. Try feature/navigation."
            : "This branch or worktree already exists.";
        return;
    }
    K.queue("create", { tree });
    K.go("worktrees", { state: "creating" });
};
