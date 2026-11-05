/* Archive is reversible visibility; delete is a separate, acknowledged operation. */
K.ensureScope("feature-onboarding");
K.ensureScope("fix-scroll-anchor");
if (!K.query.has("demo") && ["clean", "dirty"].includes(K.query.get("fixture"))) {
    const scope = K.ensureScope("fix-scroll-anchor");
    scope.conversations.review.turn = "ready";
    scope.dirty = K.query.get("fixture") === "dirty";
}
if (!K.query.has("demo") && K.query.get("fixture") === "archived") {
    const scope = K.ensureScope("fix-scroll-anchor");
    scope.archived = true;
    scope.conversations.review.turn = "ready";
    scope.conversations.checks.archived = true;
}
K.resourceTarget = () => {
    const scope = K.ensureScope(K.query.get("targetTree") || K.tree, K.query.get("targetRepo") || K.repo);
    const kind = K.query.get("kind") || "conversation";
    const id = K.query.get("target") || K.activeConversation;
    return { kind, id, scope, record: kind === "worktree" ? scope : scope.conversations[id] };
};
K.deletionBlocker = ({ kind, scope, record }) => {
    if (!record) return "This item no longer exists. Refresh the list.";
    if (kind === "worktree" && scope.deleted)
        return "This worktree folder has already been removed.";
    if (kind === "conversation")
        return K.isActiveTurn(record)
            ? "Stop this conversation’s active turn before deleting it."
            : "";
    if (scope.tree === "develop") return "The main worktree cannot be deleted here.";
    if (Object.values(scope.conversations).some(K.isActiveTurn))
        return "Stop all active turns in this worktree before deleting it.";
    return scope.dirty
        ? "Commit or preserve uncommitted changes before deleting this worktree. Force deletion is not available here."
        : "";
};
K.resourceDialog = () => {
    const stage = K.query.get("dialog");
    if (!["manage", "archive", "delete", "restore-archive"].includes(stage)) return "";
    const target = K.resourceTarget();
    const name =
        target.kind === "worktree"
            ? target.scope.tree
            : target.record?.title || "Unavailable conversation";
    if (stage === "manage")
        return K.modal(
            `Manage ${target.kind}`,
            `<h3>${K.escape(name)}</h3><div class="manage-actions">${target.kind === "conversation" ? `${K.button("Rename…", "rename-prompt", "pencil", "ghost")}${K.button("Fork this conversation…", "fork-prompt", "git-branch", "ghost", K.forkBlocker?.() ? `disabled title="${K.forkBlocker()}"` : "")}` : ""}${K.button("Archive", "archive-prompt", "archive", "ghost")}${K.button("Delete…", "delete-prompt", "trash-2", "ghost danger")}</div><p class="help">Archived items can be restored in Settings → Archived.</p>`,
        );
    const pending = K.state === "resource-pending";
    const failure =
        K.state === "resource-error"
            ? K.notice(
                  "Operation not completed",
                  "Nothing has been removed or restored. You can retry safely.",
                  "",
                  "",
                  "error",
              )
            : "";
    if (stage === "archive")
        return K.modal(
            `Archive ${target.kind}?`,
            `<p><b>${K.escape(name)}</b> will be hidden from the main lists. Files and history are retained.</p><p class="help">Running turns continue. You can monitor and restore archived items from Settings → Archived.</p>${failure}`,
            `${K.button("Cancel", "close-dialog")}${K.button(pending ? "Archiving…" : "Archive", "archive-confirm", "archive", "primary", pending ? "disabled" : "")}`,
        );
    if (stage === "restore-archive")
        return K.modal(
            `Restore ${target.kind}?`,
            `<p>Restore <b>${K.escape(name)}</b> to its original workspace.</p>${target.scope.deleted ? K.notice("Original worktree is missing", "Restore its folder first. This action will not attach history to a different worktree.") : '<p class="help">Restoring does not start or rerun a turn. You stay in Settings after restoration.</p>'}${failure}`,
            `${K.button("Cancel", "close-dialog")}${K.button(pending ? "Restoring…" : "Restore", "archive-restore-confirm", "archive-restore", "primary", pending || target.scope.deleted ? "disabled" : "")}`,
        );
    const blocker = K.deletionBlocker(target);
    return K.modal(
        `Delete ${target.kind}?`,
        `<p><b>${K.escape(name)}</b></p><p>${target.kind === "worktree" ? "Remove the linked worktree folder. Its branch is retained. Conversations are archived with their original ownership; they are not deleted." : "Permanently remove this conversation from KetPlus after deletion is acknowledged. Provider-owned history is not deleted by this action."}</p>${blocker ? K.notice("Deletion unavailable", blocker) : '<p class="error-text">This is not Archive. Deleted content cannot be restored from Settings.</p><label for="delete-confirmation">Type DELETE to confirm</label><input id="delete-confirmation" autocomplete="off" placeholder="DELETE">'}${failure}`,
        `${K.button("Cancel", "close-dialog")}${K.button(pending ? "Deleting…" : "Delete", "delete-confirm", "trash-2", "danger", blocker || pending ? "disabled" : "")}`,
    );
};
K.archiveRows = () =>
    Object.values(K.demo.scopes)
        .filter((scope) => scope.repo === K.repo)
        .flatMap((scope) => {
            const row = (kind, record, id, archived) =>
                `<div class="archive-item"><span>${K.icon(kind === "worktree" ? "git-branch" : "messages-square")}</span><div class="archive-description"><b>${K.escape(kind === "worktree" ? scope.tree : record.title)}</b><small>${K.escape(scope.repo)} · ${K.escape(scope.tree)}${scope.deleted ? " · Folder missing" : ""}</small>${kind === "conversation" ? K.statusLabel(record.turn) : K.worktreeActivity(scope.tree)}</div>${K.button(archived ? "Restore…" : "Restore worktree first", "archive-restore", "archive-restore", "ghost", `data-kind="${kind}" data-target="${K.escape(id)}" data-tree="${K.escape(scope.tree)}" ${!archived ? "disabled" : ""}`)}${K.button("Delete…", "archive-delete", "trash-2", "ghost danger", `data-kind="${kind}" data-target="${K.escape(id)}" data-tree="${K.escape(scope.tree)}"`)}${kind === "conversation" && K.isActiveTurn(record) ? K.button("Stop turn", "archive-stop", "square", "ghost", `data-target="${K.escape(id)}" data-tree="${K.escape(scope.tree)}"`) : ""}</div>`;
            return [
                scope.archived ? row("worktree", scope, scope.tree, true) : "",
                ...Object.values(scope.conversations)
                    .filter((r) => r.archived || scope.archived)
                    .map((r) => row("conversation", r, r.id, r.archived && !scope.archived)),
            ];
        })
        .join("");
const appearanceView = K.views.settings;
K.views.settings = () => {
    if (K.query.get("tab") !== "archived")
        return appearanceView().replace(
            '</nav><div class="settings-main">',
            `${K.link("settings", "Archived", "archive", { tab: "archived" })}</nav><div class="settings-main">`,
        );
    const rows = K.archiveRows();
    return `<section class="view"><div class="page-content settings-page"><div class="page-heading"><h1>Settings</h1></div><div class="settings-layout"><nav class="settings-nav" aria-label="Settings categories">${K.link("settings", "Appearance", "palette")}${K.link("settings", "Typography", "type", { tab: "typography" })}${K.link("connections", "Account & devices", "unplug")}${K.link("settings", "Archived", "archive", { tab: "archived" }, "active")}</nav><div class="settings-main"><h2>Archived</h2><p class="help">Restore worktrees and conversations to their original context. Archive does not stop Agents or delete files.</p>${K.query.get("saved") === "restored" ? K.notice("Restored", "The item is back in its original workspace. No new turn was started.", "", "", "") : K.query.get("saved") === "delete" ? K.notice("Deletion acknowledged", "The requested item was removed. A deleted worktree’s conversations remain archived; its branch is retained.", "", "", "") : K.query.get("saved") === "archive" ? K.notice("Archived", "History and files are retained. Running turns continue.", "", "", "") : ""}<div class="archive-list">${rows || '<div class="empty-state"><h3>No archived items</h3><p>Items you archive will appear here.</p></div>'}</div>${K.link("worktrees", "View worktrees", "git-branch", {}, "btn")}</div></div></div></section>${K.resourceDialog()}`;
};
const oldWorktreeView = K.views.worktrees;
K.views.worktrees = () => {
    const records = Object.values(K.demo.scopes).filter(
        (s) => s.repo === K.repo && !s.archived && !s.deleted,
    );
    const rows = records
        .map(
            (s) =>
                `<div class="worktree-record"><a class="worktree-name" href="${K.url("workspace", { tree: s.tree })}">${K.icon("git-branch")}<b>${K.escape(s.tree)}</b><small>${Object.values(s.conversations).filter((r) => !r.archived).length} conversations</small></a>${K.worktreeActivity(s.tree)}${s.dirty ? K.badge("Uncommitted changes", "warning") : K.badge("Clean")}<span class="spacer"></span>${K.button("Manage", "manage-worktree", "ellipsis", "ghost", `data-tree="${K.escape(s.tree)}"`)}</div>`,
        )
        .join("");
    const create =
        K.query.get("dialog") === "create"
            ? oldWorktreeView().slice(oldWorktreeView().indexOf('<div class="modal-backdrop">'))
            : "";
    return `<section class="view"><div class="page-content"><div class="page-heading"><div><h1>Worktrees</h1><p>Independent files, conversations and drafts.</p></div>${K.button("Create worktree", "new-worktree", "plus", "primary")}</div>${K.state === "creating" ? K.notice("Creating worktree…", "Checking Git result before opening the new workspace.") : K.state === "create-error" ? K.notice("Worktree could not be created", "The branch or folder may already exist. Retry checks the previous result before creating anything.", "new-worktree", "Retry", "error") : ""}<div class="worktree-list">${rows}</div>${K.link("settings", "Archived worktrees and conversations", "archive", { tab: "archived" }, "btn ghost")}</div></section>${create}${K.resourceDialog()}`;
};
const manageableWorkspace = K.views.workspace;
K.views.workspace = () => manageableWorkspace() + K.resourceDialog();
