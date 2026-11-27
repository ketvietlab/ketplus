const test = require("node:test");
const assert = require("node:assert/strict");
const M = require("../../ketplus.ketatlas/screens/repository-picker-model.js");

test("current repository stays first, including while searching another repository", () => {
    const state = M.createState();
    state.repoQuery = "sample-project";
    assert.deepEqual(M.rankedRepositories(state).map(repo => repo.id), ["ketplus-desktop", "sample-project"]);
});
test("search is scoped to the selected repository and re-evaluated after switching", () => {
    const state = M.createState(new URLSearchParams("search=pilot"));
    assert.equal(M.visibleWorktrees(state).length, 0);
    M.completeSwitch(state, M.beginSwitch(state, "sample-project"));
    assert.deepEqual(M.visibleWorktrees(state).map(tree => tree.id), ["sample-project-pilot"]);
});
test("acknowledged repository switch preserves conversation, draft and current provider turn", () => {
    const state = M.createState();
    const original = { id: state.conversationId, title: state.title, draft: state.draft, agentTree: state.agentTree };
    const request = M.beginSwitch(state, "sample-project");
    assert.equal(state.repo, "ketplus-desktop", "request alone does not mislabel the conversation");
    assert.equal(M.completeSwitch(state, request), true);
    assert.equal(state.repo, "sample-project");
    assert.equal(state.tree, "sample-project-pilot");
    assert.equal(M.rankedRepositories(state)[0].id, "sample-project");
    assert.deepEqual({ id: state.conversationId, title: state.title, draft: state.draft, agentTree: state.agentTree }, original);
    assert.equal(state.running, true);
});
test("failed switch keeps all current scope and records a retry target", () => {
    const state = M.createState();
    M.completeSwitch(state, M.beginSwitch(state, "sample-project"), false);
    assert.equal(state.repo, "ketplus-desktop");
    assert.equal(state.tree, "desktop-main");
    assert.equal(state.failed.repo, "sample-project");
    const target = state.failed;
    M.completeSwitch(state, M.beginSwitch(state, target.repo, target.tree));
    assert.equal(state.repo, "sample-project");
    assert.equal(state.failed, null);
});
test("late acknowledgment cannot overwrite a newer repository choice", () => {
    const state = M.createState();
    const old = M.beginSwitch(state, "sample-project");
    const latest = M.beginSwitch(state, "sample-mobile");
    assert.equal(M.completeSwitch(state, old), false);
    assert.equal(state.repo, "ketplus-desktop");
    assert.equal(M.completeSwitch(state, latest), true);
    assert.equal(state.repo, "sample-mobile");
});
test("returning to a repository uses its last selected worktree", () => {
    const state = M.createState();
    M.completeSwitch(state, M.beginSwitch(state, "sample-project", "sample-project-main"));
    M.completeSwitch(state, M.beginSwitch(state, "sample-mobile"));
    M.completeSwitch(state, M.beginSwitch(state, "sample-project"));
    assert.equal(state.tree, "sample-project-main");
});
test("invalid, foreign and unavailable selections do not create a pending switch", () => {
    for (const scenario of ["offline", "loading", "stale"]) {
        const state = M.createState(new URLSearchParams({ scenario }));
        assert.equal(M.beginSwitch(state, "sample-project"), null);
        assert.equal(state.pending, null);
    }
    const state = M.createState();
    assert.equal(M.beginSwitch(state, "missing"), null);
    assert.equal(M.beginSwitch(state, "sample-project", "desktop-main"), null);
});

const { screen } = require("./ketatlas-dom.cjs");
for (const platform of ["repository-picker", "repository-picker-mobile"]) {
    test(`${platform}: selection updates header, sidebar and scoped results together`, () => {
        const s = screen(`screens/${platform}.html?search=pilot`);
        const original = s.w.RepositoryPickerPrototype.state.conversationId;
        s.click('[data-repo-choice="sample-project"]');
        s.tick(400);
        assert.equal(s.document.querySelector(".rp-scope").textContent.includes("sample-project"), true);
        assert.equal(s.document.querySelector("[data-repository-group]").dataset.repositoryGroup, "sample-project");
        assert.equal(s.document.querySelector("[data-conversation-id]").dataset.conversationId, original);
        assert.equal(s.document.querySelector('[data-repo-choice="sample-project"]').getAttribute("aria-pressed"), "true");
        assert.equal(s.document.querySelectorAll("[data-tree-choice]").length, 1);
        assert.equal(s.document.querySelector("[data-tree-choice]").dataset.treeChoice, "sample-project-pilot");
        const files = new URL(s.document.querySelector('.rp-navigation a[href^="editor"]').href);
        assert.equal(files.searchParams.get("repo"), "sample-project");
        assert.equal(files.searchParams.get("tree"), "feat/sample-preview-pilot");
        assert.equal(s.w.RepositoryPickerPrototype.state.step, "worktrees");
        assert.equal(s.errors.length, 0);
    });
    test(`${platform}: edited draft survives changing repository and closing the picker`, () => {
        const s = screen(`screens/${platform}.html?picker=closed`);
        const draft = s.document.getElementById("rp-draft");
        draft.value = "Keep this exact draft ✨";
        draft.dispatchEvent(new s.w.Event("input", { bubbles: true }));
        s.click('[data-rp-action="open"]');
        s.click('[data-repo-choice="sample-project"]'); s.tick(400);
        s.click('[data-rp-action="close"]');
        assert.equal(s.document.getElementById("rp-draft").value, "Keep this exact draft ✨");
        assert.equal(s.document.querySelector("#rp-dialog"), null);
        assert.equal(s.w.RepositoryPickerPrototype.state.running, true);
        assert.equal(s.errors.length, 0);
    });
    test(`${platform}: error, offline and stale states expose working recovery`, () => {
        for (const scenario of ["error", "offline", "stale", "loading"]) {
            const s = screen(`screens/${platform}.html?scenario=${scenario}`);
            if (scenario === "error") { s.click('[data-rp-action="retry"]'); s.tick(400); }
            else { s.click('[data-rp-action="refresh"]'); s.click('[data-repo-choice="sample-project"]'); s.tick(400); }
            assert.equal(s.w.RepositoryPickerPrototype.state.repo, "sample-project");
            assert.equal(s.errors.length, 0);
        }
    });
    test(`${platform}: closing during acknowledgment does not reopen the picker`, () => {
        const s = screen(`screens/${platform}.html`);
        s.click('[data-repo-choice="sample-project"]');
        s.click('[data-rp-action="close"]');
        s.tick(400);
        assert.equal(s.document.querySelector("#rp-dialog"), null);
        assert.equal(s.w.RepositoryPickerPrototype.state.repo, "sample-project");
        assert.equal(s.document.querySelector("[data-repository-group]").dataset.repositoryGroup, "sample-project");
    });
    test(`${platform}: opaque Atlas history restrictions cannot prevent the UI acknowledgment`, () => {
        const s = screen(`screens/${platform}.html`);
        s.w.history.replaceState = () => { throw new s.w.DOMException("Sandbox", "SecurityError"); };
        s.click('[data-repo-choice="sample-project"]'); s.tick(400);
        assert.equal(s.document.querySelector(".rp-scope").textContent.includes("sample-project"), true);
        assert.equal(s.document.querySelector("[data-repository-group]").dataset.repositoryGroup, "sample-project");
        assert.equal(s.errors.length, 0);
    });
}
test("iPhone back step pins the newly selected repository first", () => {
    const s = screen("screens/repository-picker-mobile.html?search=pilot");
    s.click('[data-repo-choice="sample-project"]'); s.tick(400);
    s.click('[data-rp-action="back"]');
    assert.equal(s.w.RepositoryPickerPrototype.state.step, "repositories");
    assert.equal(s.document.querySelector("[data-repo-choice]").dataset.repoChoice, "sample-project");
    assert.equal(s.document.querySelector('[data-repo-choice="sample-project"]').getAttribute("aria-pressed"), "true");
    assert.equal(s.w.RepositoryPickerPrototype.state.treeQuery, "pilot");
});
