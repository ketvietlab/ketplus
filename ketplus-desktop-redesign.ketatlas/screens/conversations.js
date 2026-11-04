/* URL-only synthetic state: portable in opaque iframes, never credentials or file bytes. */
window.KET_ICONS["pause-circle"] = '<circle cx="12" cy="12" r="10"/><path d="M10 15V9"/><path d="M14 15V9"/>';
K.repo = K.query.get("repo") || "ketplus-desktop";
K.tree = K.query.get("tree") || "develop";
K.scopeKey = (tree = K.tree, repo = K.repo) => `${repo}:${tree}`;
K.demo = {
    version: 1,
    scopes: {},
    auth: "signed-out",
    devices: ["iphone"],
    ticket: "none",
    next: 1,
};
K.remoteSampleHost = Object.freeze({
    id: "linux-build-01",
    name: "build-node-01",
    os: "Ubuntu 24.04 LTS",
    status: "connected",
    lastSeen: "Just now",
});
const isDemoObject = value => value !== null && typeof value === "object" && !Array.isArray(value);
const isDemoText = (value, limit = 256) => typeof value === "string" && value.length <= limit;
const isDemoList = (value, limit = 64) => Array.isArray(value) && value.length <= limit && value.every(item => isDemoText(item));
const isDemoId = value => isDemoText(value, 100) && /^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(value);
const validDemoOperation = (op, scopes) => {
    if (!isDemoObject(op) || !isDemoObject(op.payload) ||
        !["workspace", "atlas-builder", "worktrees", "settings", "connections"].includes(op.page) ||
        !isDemoText(op.scope) || !Object.hasOwn(scopes, op.scope) ||
        !isDemoId(op.conversation) ||
        !Object.hasOwn(scopes[op.scope].conversations, op.conversation) ||
        !Number.isFinite(op.due) || !["success", "error"].includes(op.outcome) ||
        !Number.isSafeInteger(op.epoch) || op.epoch < 1 || op.epoch > 1000000) return false;
    const p = op.payload;
    if (["send", "steer"].includes(op.kind))
        return isDemoText(p.input, 6000) && isDemoText(p.previous, 40) && isDemoText(p.scope);
    if (op.kind === "stop") return isDemoText(p.previous, 40);
    if (op.kind === "revoke") return ["iphone", "pixel"].includes(p.device);
    if (["create", "new-conversation"].includes(op.kind)) {
        if (!isDemoText(p.tree) || !p.tree) return false;
        if (op.kind === "create") return true;
        return isDemoText(p.base) && isDemoText(p.branch) &&
            ["Codex", "Claude", "Cursor"].includes(p.provider) &&
            isDemoText(p.model, 100) && isDemoText(p.effort, 40) && isDemoText(p.mode, 40);
    }
    if (["archive", "restore", "delete"].includes(op.kind))
        return ["worktree", "conversation"].includes(p.kind) &&
            isDemoText(p.scope) && Object.hasOwn(scopes, p.scope) &&
            isDemoText(p.id) &&
            (p.kind === "worktree" || Object.hasOwn(scopes[p.scope].conversations, p.id));
    return ["fork", "reconnect", "answer", "confirm", "signin", "connection"].includes(op.kind);
};
const validDemoSnapshot = saved => {
    if (!isDemoObject(saved) || saved.version !== 1 || !isDemoObject(saved.scopes) ||
        !Number.isSafeInteger(saved.next) || saved.next < 1 || saved.next > 1000000 ||
        !["signed-in", "signed-out"].includes(saved.auth) ||
        !["none", "pending", "claimed", "approved", "expired", "cancelled", "cancellation-pending"].includes(saved.ticket) ||
        !isDemoList(saved.devices, 2) || new Set(saved.devices).size !== saved.devices.length ||
        !saved.devices.every(id => ["iphone", "pixel"].includes(id))) return false;
    if (saved.bundles !== undefined && (!isDemoObject(saved.bundles) ||
        Object.entries(saved.bundles).length > 64 ||
        !Object.entries(saved.bundles).every(([id, title]) => isDemoId(id) && isDemoText(title)))) return false;
    if (saved.remoteHosts !== undefined && (!Array.isArray(saved.remoteHosts) ||
        saved.remoteHosts.length > 32 ||
        !saved.remoteHosts.every(host => isDemoObject(host) && isDemoId(host.id) &&
            isDemoText(host.name, 120) && isDemoText(host.os, 120) &&
            isDemoText(host.lastSeen, 80) &&
            ["connected", "offline"].includes(host.status)) ||
        new Set(saved.remoteHosts.map(host => host.id)).size !== saved.remoteHosts.length)) return false;
    for (const key of ["collapsedRepositories", "expandedRepositories"])
        if (saved[key] !== undefined && !isDemoList(saved[key])) return false;
    if (saved.createdTree !== undefined && !isDemoText(saved.createdTree)) return false;
    if (saved.createdConversation !== undefined && (!isDemoObject(saved.createdConversation) ||
        !isDemoId(saved.createdConversation.id) || !isDemoText(saved.createdConversation.tree) ||
        !isDemoText(saved.createdConversation.repo) ||
        !isDemoText(saved.createdConversation.state, 40))) return false;
    const scopes = Object.entries(saved.scopes);
    if (scopes.length > 64) return false;
    for (const [key, scope] of scopes) {
        if (!isDemoObject(scope) || !isDemoText(scope.repo) || !isDemoText(scope.tree) ||
            key !== `${scope.repo}:${scope.tree}` || !isDemoText(scope.selected) ||
            !isDemoObject(scope.conversations)) return false;
        for (const flag of ["archived", "deleted", "dirty", "created"])
            if (scope[flag] !== undefined && typeof scope[flag] !== "boolean") return false;
        if (scope.atlasSessions !== undefined &&
            (!isDemoList(scope.atlasSessions, 8) ||
                !scope.atlasSessions.every(isDemoId) ||
                new Set(scope.atlasSessions).size !== scope.atlasSessions.length)) return false;
        if (scope.atlasEvent !== undefined &&
            !["", "owner-stop", "browser-closed"].includes(scope.atlasEvent)) return false;
        if (scope.atlasBundles !== undefined && (!isDemoObject(scope.atlasBundles) ||
            Object.entries(scope.atlasBundles).length > 64 ||
            !Object.entries(scope.atlasBundles).every(([id, title]) => isDemoId(id) && isDemoText(title)))) return false;
        for (const field of ["atlasStarting", "atlasStopping"])
            if (scope[field] !== undefined && scope[field] !== "" && !isDemoId(scope[field])) return false;
        const conversations = Object.entries(scope.conversations);
        if (conversations.length > 200) return false;
        for (const [id, record] of conversations) {
            if (!isDemoId(id) || !isDemoObject(record) ||
                record.id !== id || !isDemoText(record.title, 200) ||
                !["workspace", "atlas-builder"].includes(record.page) ||
                !isDemoText(record.turn, 40) || !["Codex", "Claude", "Cursor"].includes(record.provider) ||
                !isDemoText(record.model, 100) || !isDemoText(record.effort, 40) ||
                !isDemoText(record.mode, 40) || !isDemoText(record.connection, 40) ||
                typeof record.available !== "boolean" || !isDemoText(record.draft, 6000) ||
                (record.attachment !== null && (!isDemoObject(record.attachment) ||
                    !isDemoText(record.attachment.name))) || !isDemoObject(record.reading) ||
                !Number.isFinite(record.reading.top) || typeof record.reading.follow !== "boolean" ||
                !Number.isSafeInteger(record.reading.older) || record.reading.older < 0 ||
                !isDemoText(record.reading.page, 40)) return false;
            for (const [field, limit] of [["bundle", 100], ["latestPrompt", 6000],
                ["answer", 6000], ["firstAnswer", 6000], ["scope", 256],
                ["submitted", 6000], ["forkedFrom", 100], ["executionTree", 256]])
                if (record[field] !== undefined && !isDemoText(record[field], limit)) return false;
            for (const flag of ["archived", "explicit", "ownsWorktree", "bundleReady", "needsInput"])
                if (record[flag] !== undefined && typeof record[flag] !== "boolean") return false;
            if (record.requestCount !== undefined && (!Number.isSafeInteger(record.requestCount) ||
                record.requestCount < 0 || record.requestCount > 1000000)) return false;
            if (record.lastTurnStartedAt !== undefined && (!Number.isSafeInteger(record.lastTurnStartedAt) ||
                record.lastTurnStartedAt < 0)) return false;
            if (record.contextRevision !== undefined && (!Number.isSafeInteger(record.contextRevision) ||
                record.contextRevision < 1)) return false;
            if (record.compactWindow !== undefined && record.compactWindow !== null &&
                (!Number.isSafeInteger(record.compactWindow) || record.compactWindow < 1)) return false;
        }
    }
    if (saved.operation !== undefined && !validDemoOperation(saved.operation, saved.scopes)) return false;
    return true;
};
try {
    const snapshot = K.query.get("demo") || "";
    if (snapshot.length <= 131072) {
        const saved = JSON.parse(snapshot || "null");
        if (validDemoSnapshot(saved)) K.demo = saved;
    }
} catch (_) {
    /* A malformed demo URL starts a fresh synthetic snapshot. */
}
if (!K.query.has("demo") && K.page === "workspace" && !K.query.has("remote")) {
    K.demo.auth = "signed-in";
    K.demo.remoteHosts = [{ ...K.remoteSampleHost }];
}
K.makeConversation = (id, title, turn = "ready", page = "workspace", bundle = "") => ({
    id,
    title,
    turn,
    page,
    bundle,
    connection: "connected",
    available: true,
    draft: "",
    attachment: null,
    reading: { top: 0, follow: true, older: 0, page: "latest" },
    requestCount: 0,
    lastTurnStartedAt: 0,
    latestPrompt: "",
    answer: "",
    scope: "",
    submitted: "",
    archived: false,
    provider: "Codex",
    model: "GPT-5.6 Luna",
    effort: "Medium",
    mode: "workspace-write",
});
K.ensureScope = (tree = K.tree, repo = K.repo) => {
    const key = K.scopeKey(tree, repo);
    if (!Object.hasOwn(K.demo.scopes, key)) {
        const main = tree === "develop";
        K.demo.scopes[key] = {
            tree,
            repo,
            selected: repo === "api-service" && main ? "linux-build" : main ? "navigation" : "review",
            archived: false,
            deleted: false,
            dirty: tree === "develop" && repo !== "api-service",
            atlasSessions: [],
            atlasEvent: "",
            atlasBundles: {},
            atlasStarting: "",
            atlasStopping: "",
            conversations: repo === "api-service" && main
                ? {
                      "linux-build": K.makeConversation("linux-build", "Check the Linux build"),
                  }
                : repo === "api-service"
                ? {}
                : !["ketplus-desktop", "ketviet"].includes(repo)
                ? {}
                : main
                ? {
                      navigation: K.makeConversation("navigation", "Refine navigation"),
                      startup: K.makeConversation("startup", "Review the startup flow", "running"),
                      atlas: K.makeConversation(
                          "atlas",
                          "Atlas: Onboarding experience",
                          "ready",
                          "workspace",
                          "onboarding",
                      ),
                  }
                : {
                      review: K.makeConversation(
                          "review",
                          `Review ${tree.replaceAll("-", " ")}`,
                          "running",
                      ),
                      checks: K.makeConversation("checks", "Check this worktree"),
                  },
        };
        if (main) {
            const activity = repo === "ketplus-desktop"
                ? { navigation: 3000, startup: 2000, atlas: 1000 }
                : repo === "ketviet"
                  ? { navigation: 1800, startup: 1600, atlas: 1400 }
                  : repo === "api-service"
                    ? { "linux-build": 2400 }
                    : {};
            for (const [id, started] of Object.entries(activity))
                K.demo.scopes[key].conversations[id].lastTurnStartedAt = started;
        }
    }
    return K.demo.scopes[key];
};
K.scope = K.ensureScope();
K.bundleTitles = Object.assign(Object.create(null), {
    desktop: "KétPlus Desktop",
    onboarding: "Onboarding experience",
    mobile: "KetPlus Mobile",
    workspace: "Workspace",
    ...(K.demo.bundles || {}),
    ...(K.scope.atlasBundles || {}),
});
K.bundle =
    K.query.get("bundle") ||
    Object.keys(K.bundleTitles).find((id) => K.bundleTitles[id] === K.query.get("project")) ||
    "onboarding";
K.query.set("bundle", K.bundle);
K.query.set("project", K.bundleTitles[K.bundle] || "Unavailable Atlas");
K.bundleConversation = (scope, bundle) => {
    const id = bundle === "onboarding" ? "atlas" : `atlas-${bundle}`;
    return (
        scope.conversations[id] ||
        K.makeConversation(
            id,
            `Atlas: ${K.bundleTitles[bundle] || bundle}`,
            "ready",
            "atlas-builder",
            bundle,
        )
    );
};
K.activeConversation = K.query.get("conversation") || K.scope.selected;
if (K.page === "atlas-builder") {
    const record = K.bundleConversation(K.scope, K.bundle);
    K.scope.conversations[record.id] = record;
    K.activeConversation = record.id;
} else if (
    !Object.hasOwn(K.scope.conversations, K.activeConversation) ||
    K.scope.conversations[K.activeConversation].page !== "workspace"
) {
    K.activeConversation = Object.keys(K.scope.conversations).find(
        (id) =>
            K.scope.conversations[id].page === "workspace" && !K.scope.conversations[id].archived,
    );
}
if (!K.activeConversation) {
    K.activeConversation = `draft-${K.demo.next++}`;
    K.scope.conversations[K.activeConversation] = K.makeConversation(
        K.activeConversation,
        "New conversation",
        "empty",
    );
}
K.current = K.scope.conversations[K.activeConversation];
if (K.page === "workspace" && K.query.get("atlasView") === "elsewhere")
    K.ensureScope(K.tree === "develop" ? "feature-onboarding" : "develop", K.repo).atlasSessions = ["desktop"];
if (K.page === "atlas-builder" && K.current.bundleReady === undefined)
    K.current.bundleReady = K.query.get("state") !== "empty";
if (K.page === "atlas-builder" && K.query.get("hasAtlas") === "1") K.current.bundleReady = true;
if (K.page === "workspace" && K.query.get("state") === "empty" && K.current.turn !== "empty") {
    const id = `draft-${K.demo.next++}`;
    K.current = K.scope.conversations[id] = K.makeConversation(id, "New conversation", "empty");
    K.activeConversation = id;
}
if (["workspace", "atlas-builder"].includes(K.page)) {
    K.scope.selected = K.activeConversation;
    K.state = K.query.get("state") || K.current.turn;
    if (
        [
            "empty",
            "running",
            "building",
            "question",
            "ready",
            "stopped",
            "stopping",
            "steering",
            "sending",
            "answer-submitting",
            "answer-error",
        ].includes(K.state)
    )
        K.current.turn = K.state;
    if (K.state === "error" || K.state === "reconnecting")
        K.current.connection = K.state === "error" ? "offline" : "reconnecting";
    if (K.query.has("turn")) K.current.turn = K.query.get("turn");
    if (K.query.has("pendingInput")) K.current.needsInput = K.query.get("pendingInput") === "true";
    if (K.state === "missing") K.current.available = false;
    if (K.query.has("history")) K.current.reading.page = K.query.get("history");
    if (K.query.has("scope")) K.current.scope = K.query.get("scope");
    else if (K.current.scope) K.query.set("scope", K.current.scope);
}
if (K.page === "workspace" && K.repo === "api-service") {
    const host = K.demo.remoteHosts?.find(item => item.id === K.remoteSampleHost.id);
    if (host?.status !== "connected") K.current.connection = "offline";
    else if (!["error", "reconnecting"].includes(K.state)) K.current.connection = "connected";
}
K.isActiveTurn = (record = K.current) =>
    [
        "running",
        "building",
        "question",
        "stopping",
        "steering",
        "answer-submitting",
        "answer-error",
    ].includes(record.turn);
K.newConversation = () => {
    const id = `draft-${K.demo.next++}`;
    K.scope.conversations[id] = K.makeConversation(id, "New conversation", "empty");
    Object.assign(K.scope.conversations[id], { provider: K.current.provider, model: K.current.model, effort: K.current.effort, mode: K.current.mode, explicit: true });
    K.go("workspace", { conversation: id, state: "empty" });
};
K.captureDraft = () => {
    if (!["workspace", "atlas-builder"].includes(K.page)) return;
    const prompt = document.querySelector("#prompt");
    if (prompt) K.current.draft = prompt.value;
    if (K.query.has("scope")) K.current.scope = K.query.get("scope");
    const scroll = document.querySelector(".chat-scroll,.builder-scroll");
    if (scroll) {
        K.current.reading.top = scroll.scrollTop;
        K.current.reading.follow =
            scroll.scrollHeight - scroll.clientHeight - scroll.scrollTop < 40;
    }
};
K.conversationStatus = (state) => {
    const statuses = {
        empty: ["Not started", "message-square", "neutral"],
        running: ["Working", "loader-circle", "working"],
        building: ["Working", "loader-circle", "working"],
        question: ["Needs input", "pause-circle", "waiting"],
        "answer-error": ["Reply failed", "triangle-alert", "error"],
        "answer-submitting": ["Sending reply", "loader-circle", "working"],
        sending: ["Sending request", "loader-circle", "working"],
        steering: ["Sending input", "loader-circle", "working"],
        stopping: ["Stopping", "loader-circle", "waiting"],
        ready: ["Completed", "circle-check", "completed"],
        stopped: ["Stopped", "square", "neutral"],
        error: ["Disconnected", "unplug", "error"],
        reconnecting: ["Reconnecting", "loader-circle", "waiting"],
        missing: ["Worktree missing", "folder", "error"],
        restore: ["Loading history", "loader-circle", "neutral"],
    };
    const [label, icon, tone] = statuses[state] || statuses.ready;
    return { label, icon, tone, detail: label };
};
K.statusLabel = (state, cls = "") => {
    const s = K.conversationStatus(state);
    return `<span class="conversation-status status-${s.tone} ${cls}">${K.icon(s.icon)}<span>${s.label}</span></span>`;
};
K.repositoryCatalog = [
    { id: "benchmark-mongodb", label: "benchmark-mongodb", location: "local" },
    { id: "ketviet", label: "ketviet", location: "local" },
    { id: "ketplus-desktop", label: "ketplus-desktop", location: "local" },
    { id: "ketplus-app", label: "ketplus-app", location: "local" },
    { id: "ketatlas", label: "ketatlas", location: "local" },
    { id: "flow", label: "flow", location: "local" },
    { id: "api-service", label: "api-service", location: "remote", hostId: "linux-build-01" },
];
K.repositoryHost = repo => {
    const hostId = K.repositoryCatalog.find(item => item.id === repo)?.hostId;
    return hostId ? K.demo.remoteHosts?.find(host => host.id === hostId) : null;
};
K.repositoryMachine = repo => K.repositoryCatalog.find(item => item.id === repo)?.hostId || "local";
K.selectedMachine = () => {
    const machine = K.query.get("machine") || K.repositoryMachine(K.repo);
    return machine === "local" || K.demo.remoteHosts?.some(host => host.id === machine)
        ? machine : "local";
};
K.repositoryRecords = (repo = K.repo) => {
    if (!Object.values(K.demo.scopes).some((scope) => scope.repo === repo) && ["ketviet", "ketplus-desktop", "api-service"].includes(repo))
        K.ensureScope("develop", repo);
    return Object.values(K.demo.scopes)
        .filter((scope) => scope.repo === repo && !scope.archived && !scope.deleted)
        .flatMap((scope) =>
            Object.values(scope.conversations)
                .filter((record) => !record.archived)
                .map((record) => ({ record, scope })),
        )
        .sort((left, right) => (right.record.lastTurnStartedAt || 0) - (left.record.lastTurnStartedAt || 0)
            || left.record.title.localeCompare(right.record.title)
            || left.record.id.localeCompare(right.record.id));
};
K.repositoryRecency = repo => K.repositoryRecords(repo)[0]?.record.lastTurnStartedAt || 0;
K.repositoryActivity = (repo = K.repo) => {
    const records = K.repositoryRecords(repo).map(({ record }) => record);
    if (records.some((record) => ["running", "building", "sending", "steering"].includes(record.turn)))
        return `<span class="repository-indicator status-working" aria-label="Working" title="Working">${K.icon("loader-circle")}</span>`;
    if (records.some((record) => ["question", "answer-error"].includes(record.turn)))
        return `<span class="repository-indicator status-waiting" aria-label="Needs input" title="Needs input">${K.icon("pause-circle")}</span>`;
    return "";
};
K.conversationActivity = (record) => {
    // Async questions/confirmations leave the turn running, but need the reader's attention.
    const status = K.conversationStatus(record.needsInput && K.isActiveTurn(record) ? "question" : record.turn);
    return `<span class="conversation-indicator status-${status.tone}" aria-label="${status.label}" title="${status.label}">${K.icon(status.icon)}</span>`;
};
K.conversationParams = (page, query, params) => {
    const scope = K.ensureScope(params.get("tree") || K.tree, params.get("repo") || K.repo);
    if (!["workspace", "atlas-builder"].includes(page)) return;
    let id = query.conversation || scope.selected;
    if (page === "atlas-builder" && params.get("bundle") !== K.bundle) {
        for (const key of ["scope", "flow", "requestFlow", "brief", "hasAtlas", "bundleState"])
            params.delete(key);
    }
    if (page === "atlas-builder")
        id = K.bundleConversation(scope, params.get("bundle") || K.bundle).id;
    let record = Object.hasOwn(scope.conversations, id) ? scope.conversations[id] : null;
    if (page === "workspace" && (record?.page === "atlas-builder" || record?.archived)) {
        id = Object.keys(scope.conversations).find(
            (key) =>
                scope.conversations[key].page === "workspace" && !scope.conversations[key].archived,
        );
        record = scope.conversations[id];
    }
    if (record) for (const key of ["provider", "model", "effort", "mode"]) {
        if (!Object.hasOwn(query, key)) params.set(key, record[key] || ({provider: "Codex", model: "GPT-5.6 Luna", effort: "Medium", mode: "workspace-write"})[key]);
    }
    if (id && record) params.set("conversation", id);
    else params.delete("conversation");
    if (!Object.hasOwn(query, "state")) params.set("state", record?.turn || "ready");
};
K.conversationCatalog = K.scope.conversations;
K.conversationRows = (repo = K.repo) => {
    const records = K.repositoryRecords(repo).filter(({ record }) => (!record.id.startsWith("draft-") || record.explicit) && (K.isTagFiltered?.(record) ?? true));
    const expanded = (K.demo.expandedRepositories || []).includes(repo);
    const visible = expanded ? records : records.filter(({record}, index) => index < 5 || K.isActiveTurn(record) || (repo === K.repo && record.id === K.activeConversation));
    if (records.length === 0) return "";
    return `<div class="conversation-list" aria-label="Conversations in ${K.escape(repo)}">${visible
        .map(({ record, scope }) => {
            const selected =
                repo === K.repo &&
                record.id === K.activeConversation &&
                ["workspace", "atlas-builder"].includes(K.page);
            const host = K.repositoryHost(repo);
            const available = !host || host.status === "connected";
            const content = K.conversationRowContent?.(record, scope) ?? `${K.conversationActivity(record)}<span class="conversation-title">${K.escape(record.title)}</span>`;
            const tagNames = (K.recordTags?.(record) || []).map(tag => tag.label).join(", ");
            const title = `${K.escape(record.title)}${tagNames ? `&#10;Tags: ${K.escape(tagNames)}` : ""}&#10;${K.escape(scope.tree)}${host ? `&#10;${K.escape(host.name)}` : ""}`;
            return available
                ? `<a class="conversation ${selected ? "selected" : ""}" data-conversation="${K.escape(record.id)}" title="${title}" ${selected ? 'aria-current="page"' : ""} href="${K.url(record.page, { repo: scope.repo, tree: scope.tree, conversation: record.id, state: record.turn, bundle: record.bundle || K.bundle })}">${content}</a>`
                : `<span class="conversation unavailable" aria-disabled="true" title="${title}&#10;Remote machine offline">${content}</span>`;
        })
        .join("")}${records.length > 5 ? K.button(expanded ? "Show less" : "Show more", "expand-conversations", null, "ghost conversation-more", `data-repo="${K.escape(repo)}"`) : ""}</div>`;
};
K.worktreeActivity = (tree = K.tree) => {
    const records = Object.values(K.ensureScope(tree).conversations);
    const working = records.filter((r) =>
        ["running", "building", "steering"].includes(r.turn),
    ).length;
    const waiting = records.filter((r) => ["question", "answer-error"].includes(r.turn)).length;
    return `<small class="tree-activity">${working} working${waiting ? ` · ${waiting} needs input` : ""}</small>`;
};
