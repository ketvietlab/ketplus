/* Deterministic prototype state. No repository, daemon or provider operations. */
((root) => {
    const repositories = [
        { id: "ketplus-desktop", path: "/workspace/ketplus-desktop", preferred: "desktop-main", conversations: ["Linux Daemon", "Entitlement"] },
        { id: "sample-project", path: "/workspace/sample-project", preferred: "sample-project-pilot", conversations: ["Product Handoff", "Phone integration"] },
        { id: "sample-mobile", path: "/workspace/sample-mobile", preferred: "mobile-main", conversations: ["Question sheet", "iPhone connection"] },
        { id: "ketplus", path: "/workspace/ketplus", preferred: "cm-main", conversations: ["Mermaid theme"] },
        { id: "ketchat-desktop", path: "/workspace/ketchat-desktop", preferred: "chat-main", conversations: ["Mac PP", "iOS PP"] },
        { id: "fusetter", path: "/workspace/fusetter", preferred: "fusetter-main", conversations: ["114", "116"] },
    ];
    const worktrees = [
        { id: "desktop-main", repo: "ketplus-desktop", branch: "develop", path: "/workspace/ketplus-desktop", main: true },
        { id: "desktop-status", repo: "ketplus-desktop", branch: "fix/compaction-status", path: "/workspace/ketplus-desktop-compaction" },
        { id: "sample-project-main", repo: "sample-project", branch: "integration", path: "/workspace/sample-project", main: true },
        { id: "sample-project-pilot", repo: "sample-project", branch: "feat/ketview-production-pilot", path: "/workspace/sample-project-ketview-pilot", used: true },
        { id: "sample-project-phone", repo: "sample-project", branch: "feat/ketphone-recording", path: "/workspace/sample-project-ketphone-g1" },
        { id: "mobile-main", repo: "sample-mobile", branch: "develop", path: "/workspace/sample-mobile", main: true },
        { id: "mobile-questions", repo: "sample-mobile", branch: "fix/question-sheet", path: "/workspace/sample-mobile-questions" },
        { id: "cm-main", repo: "ketplus", branch: "develop", path: "/workspace/ketplus", main: true },
        { id: "chat-main", repo: "ketchat-desktop", branch: "develop", path: "/workspace/ketchat-desktop", main: true },
        { id: "fusetter-main", repo: "fusetter", branch: "develop", path: "/workspace/fusetter", main: true },
    ];
    const getRepo = id => repositories.find(repo => repo.id === id);
    const getTree = id => worktrees.find(tree => tree.id === id);
    function createState(params = new URLSearchParams()) {
        const repo = getRepo(params.get("repo")) || repositories[0];
        const requestedTree = getTree(params.get("worktree"));
        return {
            conversationId: "conversation-ketviewer-core", title: "KétViewer Core",
            repo: repo.id, tree: requestedTree?.repo === repo.id ? requestedTree.id : repo.preferred,
            agentTree: "sample-project-pilot", running: params.get("turn") !== "idle",
            draft: "Check the pilot deployment and keep the current changes.",
            repoQuery: "", treeQuery: params.get("search") || "", lastTrees: {},
            scenario: ["error", "offline", "loading", "stale"].includes(params.get("scenario")) ? params.get("scenario") : "ready",
            open: params.get("picker") !== "closed", step: params.get("step") === "worktrees" ? "worktrees" : "repositories",
            drawer: false, pending: null,
            failed: params.get("scenario") === "error" ? { repo: "sample-project", tree: "sample-project-pilot" } : null,
            request: 0, receipt: "", collapsed: [],
        };
    }
    function rankedRepositories(state) {
        const query = state.repoQuery.trim().toLowerCase();
        return repositories.filter(repo => repo.id === state.repo || `${repo.id} ${repo.path}`.toLowerCase().includes(query))
            .sort((a, b) => Number(b.id === state.repo) - Number(a.id === state.repo));
    }
    function visibleWorktrees(state) {
        const terms = state.treeQuery.trim().toLowerCase().split(/\s+/).filter(Boolean);
        return worktrees.filter(tree => tree.repo === state.repo && terms.every(term => `${tree.branch} ${tree.path}`.toLowerCase().includes(term)))
            .sort((a, b) => Number(b.id === state.tree) - Number(a.id === state.tree) || Number(Boolean(b.used)) - Number(Boolean(a.used)));
    }
    function beginSwitch(state, repoId, treeId) {
        const repo = getRepo(repoId);
        const tree = getTree(treeId || state.lastTrees[repoId] || repo?.preferred);
        if (!repo || !tree || tree.repo !== repo.id || ["offline", "loading", "stale"].includes(state.scenario)) return null;
        const request = ++state.request;
        state.failed = null;
        state.pending = { request, repo: repo.id, tree: tree.id };
        return request;
    }
    function completeSwitch(state, request, successful = true) {
        if (!state.pending || state.pending.request !== request) return false;
        const target = state.pending;
        state.pending = null;
        if (!successful) { state.failed = target; return false; }
        state.lastTrees[state.repo] = state.tree;
        state.repo = target.repo;
        state.tree = target.tree;
        state.lastTrees[state.repo] = state.tree;
        state.repoQuery = "";
        state.step = "worktrees";
        state.collapsed = state.collapsed.filter(id => id !== state.repo);
        state.receipt = `Conversation moved to ${state.repo}.`;
        return true;
    }
    const api = { repositories, worktrees, getRepo, getTree, createState, rankedRepositories, visibleWorktrees, beginSwitch, completeSwitch };
    if (typeof module !== "undefined" && module.exports) module.exports = api;
    else root.RepositoryPickerModel = api;
})(typeof window === "undefined" ? globalThis : window);
