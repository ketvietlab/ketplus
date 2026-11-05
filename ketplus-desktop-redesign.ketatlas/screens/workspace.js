K.changedFiles = () =>
    `<div class="changed-files"><div class="changed-heading">${K.icon("git-compare-arrows")}<strong>3 files changed</strong><span class="spacer"></span><span class="positive">+48</span><span class="negative">−16</span></div>${[
        ["NavigationRail.qml", "modules/ai/src/agent/qml", "+28", "−9"],
        ["WorktreeItem.qml", "modules/ai/src/agent/qml", "+14", "−5"],
        ["ThemeManager.cpp", "apps/desktop/src/ui", "+6", "−2"],
    ]
        .map(([name, path, plus, minus]) =>
            K.link(
                "workspace",
                `<span class="file-label"><b>${name}</b><small>${path}</small></span><span class="positive">${plus}</span><span class="negative">${minus}</span>${K.icon("arrow-up-right")}`,
                "file-code",
                { panel: "file", file: name },
                "changed-file",
            ),
        )
        .join(
            "",
        )}<div class="changed-footer">${K.link("changes", "View all changes", "git-branch", {}, "text-link")}<span>Saved in worktree</span></div></div>`;
K.activityGroup = (icon, label, rows, options = {}) =>
    `<details class="activity-group ${options.kind || ""}" ${options.open ? "open" : ""}><summary>${K.icon(icon)}<span>${label}</span><span class="spacer"></span>${K.icon("chevron-down")}</summary><div class="activity-children">${rows.join("")}</div></details>`;
K.fileActivity = (verb, path) => {
    const slash = path.lastIndexOf("/");
    const directory = slash > -1 ? path.slice(0, slash) : "";
    const name = slash > -1 ? path.slice(slash + 1) : path;
    return K.link("workspace", `<span><b>${verb} ${K.escape(name)}</b><small>${K.escape(directory)}</small></span>${K.icon("chevron-right")}`, verb === "Edited" ? "pencil" : "file-code", { panel: "file", file: name }, `activity-child${verb === "Edited" ? " changed-file-row" : ""}`);
};
K.commandActivity = (status, command) =>
    `<details class="command-detail"><summary class="activity-row command-row"><span class="activity-prefix">${status} command</span>${K.icon("chevron-right")}</summary><div class="command-output"><details class="command-input-detail"><summary><code>${K.escape(command)}</code><span class="command-show-more">Show more</span><span class="command-show-less">Show less</span></summary></details><pre class="command-result"><code>${status === "Failed" ? "Build failed: sample diagnostic. Inspect the reported file and retry." : command.startsWith("rg") ? "ActivityTimeline.qml:42: commandExecution\nActivityTimeline.qml:58: fileChange\nActivityTimeline.qml:76: mcpToolCall" : "Build completed. Focused tests passed.\nThis is deterministic sample output."}</code></pre></div></details>`;
K.imageActivity = (open = false) =>
    K.activityGroup(
        "image",
        "Viewed 4 images",
        [
            `<div class="activity-gallery">${[
                ["workspace-light.svg", "Workspace · light"],
                ["workspace-dark.svg", "Workspace · dark"],
                ["activity-expanded.svg", "Expanded activities"],
                ["image-gallery.svg", "Image gallery"],
            ]
                .map(
                    ([file, label], index) =>
                        `<button ${index > 2 ? "hidden" : ""} type="button" class="activity-thumb" data-action="preview-activity-image" data-src="../assets/activity/${file}" data-label="${label}" aria-label="Open ${label}"><img src="../assets/activity/${file}" alt=""><span>${label}</span></button>`,
                )
                .join("")}<button type="button" class="activity-more" data-action="expand-activity-images">+1</button></div>`,
        ],
        { open, kind: "image-activity" },
    );
K.browserGroupFixture = () => {
    const state = K.query.get("groupDetails") || "collapsed";
    let rows = ["open"].map(operation =>
        `<div class="activity-child" data-browser-tool="browser_${operation}"><span>Used browser · ${operation}</span>${operation === "open" ? '<a class="text-link browser-open-action" href="https://doc.qt.io/qt-6/qml-qtquick-listview.html" target="_blank" rel="noopener noreferrer" aria-label="Open visited website">Open</a>' : ""}</div>`);
    if (state === "loading") rows = ['<div class="activity-child" role="status">Loading activities…</div>'];
    if (state === "error") rows = ['<div class="activity-child" role="alert">Could not load activities.</div>', '<button type="button" class="text-link" data-action="retry-group-fixture">Retry</button>'];
    if (state === "updated") rows.push('<div class="activity-child" data-browser-tool="visual_compare"><span>Used browser · compare</span></div>');
    return K.activityGroup("globe", "Used the browser", rows, { open: state !== "collapsed" });
};
K.browserActivities = () =>
    K.browserGroupFixture() +
    `<div class="activity-row" data-browser-tool="browser_capture">${K.icon("image")}<span>Used browser · capture</span></div><div class="activity-gallery" aria-label="Browser screenshot evidence"><button type="button" class="activity-thumb" data-action="preview-activity-image" data-src="../assets/activity/workspace-light.svg" data-label="Browser screenshot evidence" aria-label="Open browser screenshot evidence"><img src="../assets/activity/workspace-light.svg" alt="Browser screenshot evidence"></button></div>` +
    `<div class="activity-row" data-browser-tool="visual_compare">${K.icon("image")}<span>Used browser · visual compare</span></div><div class="activity-gallery" aria-label="Visual comparison evidence"><button type="button" class="activity-thumb" data-action="preview-activity-image" data-src="../assets/activity/workspace-dark.svg" data-label="Visual comparison evidence" aria-label="Open visual comparison evidence"><img src="../assets/activity/workspace-dark.svg" alt="Visual comparison evidence"></button></div>` +
    K.activityGroup("monitor", "Used KetPlus editor and browser", [
        `<div class="activity-child"><span>Reviewed the current workspace and browser result.</span></div>`,
    ]);
K.unknownActivity = () =>
    K.activityGroup(
        "circle-help",
        "Used a newer tool activity",
        [
            `<div class="activity-child generic-activity">${K.icon("circle-help")}<span><b>Activity type: provider/customTool.v2</b><small>Safe metadata is preserved so this event is never silently dropped.</small><code>{ "tool": "future-renderer", "status": "completed" }</code></span></div>`,
        ],
        { open: true, kind: "unknown-activity" },
    );
K.activityFeed = () => {
    const focus = K.query.get("activity") || "mixed";
    const running = K.state === "running";
    const fileRows = [
        K.fileActivity("Read", "modules/ai/src/agent/qml/AgentWorkspace.qml"),
        K.fileActivity("Read", "modules/ai/src/agent/AgentSessionManager.cpp"),
        K.fileActivity("Read", "apps/daemon/src/providers/CodexAdapter.cpp"),
    ];
    const editRows = [
        K.fileActivity("Edited", "modules/ai/src/agent/qml/ActivityTimeline.qml"),
        K.fileActivity("Edited", "modules/ai/src/agent/qml/ActivityRow.qml"),
    ];
    const activities = [];
    if (["mixed", "files"].includes(focus) || running) {
        activities.push(K.activityGroup("file-code", "Read 3 files", fileRows));
        activities.push(
            K.commandActivity(
                "Ran",
                'rg -n "commandExecution|fileChange|mcpToolCall" modules/ai apps/daemon',
            ),
        );
        if (!running) {
            activities.push(K.activityGroup("pencil", "Edited 2 files", editRows));
            activities.push(
                K.commandActivity(
                    K.query.get("history") === "tool-errors" ? "Failed" : "Ran",
                    "cmake -S . -B build/dev -DKETPLUS_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build/dev --target ketplus_agent_tests -j8",
                ),
            );
        }
    }
    if (["mixed", "images"].includes(focus) && !running)
        activities.push(K.imageActivity(focus === "images"));
    if (["mixed", "browser"].includes(focus) && !running) {
        activities.push(
            `<div class="activity-row">${K.icon("search")}<span><b>Searched code for transcript event types</b><small>7 matching event families</small></span></div>`,
        );
        activities.push(K.browserActivities());
    }
    if (focus === "unknown" && !running) activities.push(K.unknownActivity());
    if (running)
        activities.push(
            `<div class="current-activity" role="status">${K.icon("loader-circle")}<span>Working…</span></div>`,
        );
    return `<section class="activity-stream" aria-label="Agent activity">${activities.join("")}</section>`;
};
K.finalAnswer = () =>
    `<div class="answer final-answer" data-transcript-layer="final"><h3>Transcript activity is explicit and easier to scan.</h3><p>The conversation now keeps the result separate from the work performed:</p><ul><li><strong>Exact file and command actions</strong> remain visible and expandable.</li><li><strong>Images, browser visits, searches, and app usage</strong> have dedicated activity treatments.</li><li><strong>Unknown future events</strong> fall back to a safe generic row instead of disappearing.</li></ul><p>Intermediate reasoning and progress narration are not included in the transcript.</p></div>`;
K.userBubble = text => `<div class="user-bubble"><div class="user-bubble-text" id="user-message-text">${K.escape(text)}</div><button type="button" class="user-bubble-more" data-action="toggle-user-message" aria-controls="user-message-text" aria-expanded="false" hidden>Show more</button></div>`;
K.transcript = function () {
    if (K.state === "empty")
        return `<div class="welcome"><div class="welcome-mark">${K.mark("large")}</div><div class="eyebrow">YOUR WORKSPACE</div><h1>Start with an idea.</h1><p>Explore code, solve problems, and work with your Agent<br>to bring your ideas to life.</p><div class="suggestions">${[
            [
                "code-xml",
                "Explore the codebase",
                "Understand the structure and where to start",
            ],
            [
                "git-compare-arrows",
                "Review changes",
                "Review diffs, find issues, and suggest fixes",
            ],
            [
                "network",
                "Design with Atlas",
                "Turn a brief into interactive screens",
            ],
        ]
            .map(
                ([icon, title, desc]) =>
                    `<button data-action="suggest" data-prompt="${title}" class="suggestion">${K.icon(icon)}<span><b>${title}</b><small>${desc}</small></span>${K.icon("arrow-up-right")}</button>`,
            )
            .join(
                "",
            )}</div><div class="welcome-context">${K.icon("folder-open")} ketplus-desktop <span>·</span> ${K.icon("git-branch")} ${K.escape(K.query.get("tree") || "develop")}</div></div>`;
    if (K.state === "restore")
        return `<div class="restore-state" role="status">${K.mark("large")}<span class="sr-only">Restoring conversation</span></div>`;
    const stateContent =
        K.state === "question"
            ? K.question()
            : K.state === "error"
              ? `<div class="banner error">${K.icon("unplug")}<div><strong>Agent connection interrupted</strong><p>Your history is preserved. Try continuing this turn.</p></div>${K.button("Retry", "retry", "refresh-cw")}</div>`
              : K.state === "stopped"
                ? `<div class="banner">${K.icon("square")}<div><strong>Turn stopped</strong><p>Send a new request to continue working.</p></div></div>`
                : K.state === "missing"
                  ? `<div class="banner warning">${K.icon("folder")}<div><strong>Worktree folder was deleted</strong><p>The agent keeps working from the repository's main worktree. If it recreates a worktree, the conversation moves there.</p></div></div>${K.link("worktrees", "Choose another worktree", "git-branch", {}, "btn")}`
                  : K.state === "running"
                    ? ""
                    : `${K.finalAnswer()}<div class="response-actions">${K.ib("copy", "Copy response", "copy-response")}</div>`;
    return `<div class="transcript"><div class="day-divider"><span>Today, 09:41</span></div><article class="user-message"><div class="message-label"><span>You</span><span class="avatar">AN</span></div>${K.userBubble(K.activeConversation === "startup" ? "Review the desktop startup flow and clarify how conversation activity is restored." : "Refactor the transcript so it shows exact actions and the final answer without intermediate reasoning.")}</article><article class="assistant-message transcript-turn"><div class="assistant-body">${K.activityFeed()}${stateContent}</div></article></div>`;
};
K.views.workspace = () => {
    const panel = K.query.get("panel");
    const remoteRepository = K.repositoryCatalog.find(repo => repo.id === K.repo && repo.location === "remote");
    const scopeLabel = remoteRepository
        ? `${K.repositoryHost(K.repo)?.name || "Remote machine"} · ${K.repo}/${K.tree}`
        : `${K.repo} · ${K.tree}`;
    const remoteUnavailable = remoteRepository && K.current.connection !== "connected";
    const heading = `<header class="agent-heading"><div class="agent-header-context"><h2>${K.state === "empty" ? "New conversation" : K.escape(K.current.title)}</h2>${K.statusLabel(K.state, "heading-status")}<span class="agent-scope-chip" title="${K.escape(scopeLabel)}">${K.icon(remoteRepository ? "monitor" : "git-branch")} ${K.escape(scopeLabel)}</span></div><div class="row">${K.ib("square-pen", "New conversation", "new-chat", remoteUnavailable ? "disabled" : "")}${K.atlasHeaderButton()}${K.ib("terminal", "Open terminal", "terminal", remoteUnavailable ? "disabled" : "")}</div></header>`;
    return `<section class="view agent-view"><div class="agent-layout ${panel ? "with-context" : ""}"><div class="chat-pane">${heading}<div class="chat-scroll">${K.transcript()}</div>${K.composer()}</div>${panel ? `<aside class="context-pane"><div class="context-tabs"><span>${panel === "task" ? K.bgPaneTitle() : panel === "atlas" ? `${K.icon("network")} Atlases` : `${K.icon(panel === "terminal" ? "terminal" : "file-code")} ${panel === "terminal" ? "Terminal" : K.escape(K.query.get("file") || "NavigationRail.qml")}`}</span>${K.ib("x", "Close context panel", "close-panel")}</div>${panel === "task" ? K.bgPane() : panel === "atlas" ? K.atlasPanel(Boolean(remoteUnavailable)) : panel === "terminal" ? K.terminal() : K.diff(true)}</aside>` : ""}</div></section>${
        K.query.get("dialog") === "model"
            ? K.modelDialog()
            : K.query.get("dialog") === "permissions"
              ? K.modal(
                    "Agent permissions",
                    `<p class="muted">Applies to the next turn in this conversation.</p><div class="permission-options">${[
                        [
                            "read-only",
                            "Read-only",
                            "Explore and analyze content.",
                        ],
                        [
                            "workspace-write",
                            "Workspace",
                            "Read and edit files in this worktree.",
                        ],
                        [
                            "full-access",
                            "Full access",
                            "Allow access outside the workspace.",
                        ],
                    ]
                        .map(
                            ([value, label, help]) =>
                                `<label class="choice"><input type="radio" name="permission" value="${value}" ${(K.query.get("permission") || "workspace-write") === value ? "checked" : ""}><span><b>${label}</b><small>${help}</small></span></label>`,
                        )
                        .join("")}</div>`,
                    K.button("Apply", "apply-permission", "check", "primary"),
                )
              : ""
    }`;
};
K.views.worktrees = () =>
    `<section class="view"><div class="page-content"><div class="page-heading"><div><div class="eyebrow">ONE PROJECT · MULTIPLE WORKSPACES</div><h1>Worktrees</h1><p>Each branch has its own files, conversations, and context.</p></div>${K.button("Create worktree", "new-worktree", "plus", "primary")}</div><div class="worktree-summary"><span>${K.icon("folder-open")} <b>ketplus-desktop</b></span><span>3 worktrees</span><span class="positive">1 Agent working</span></div><div class="worktree-list">${[
        ["develop", "Main workspace", "Selected", "3 conversations", "08:30"],
        [
            "feature/onboarding",
            "Independent workspace",
            "Agent working",
            "2 conversations",
            "09:38",
        ],
        [
            "fix/scroll-anchor",
            "Independent workspace",
            "Ready",
            "1 conversation",
            "Yesterday",
        ],
    ]
        .map(
            ([name, desc, status, count, time], i) =>
                `<a class="worktree-record" href="${K.url("workspace", i === 0 ? {} : { tree: i === 1 ? "feature-onboarding" : "fix-scroll-anchor" })}"><span class="worktree-glyph">${K.icon("git-branch")}</span><span class="worktree-name"><b>${name}</b><small>${desc}</small></span>${K.badge(status, i === 0 ? "brand" : i === 1 ? "success" : "")}<span class="spacer"></span><span class="muted">${count}</span><time>${time}</time>${K.icon("arrow-up-right")}</a>`,
        )
        .join(
            "",
        )}</div><div class="worktree-explainer">${K.icon("layers")}<div><h3>Keep each task in the right context.</h3><p>Switch worktrees to continue another task. Agents in other worktrees keep running, and your open files stay in place.</p></div></div></div></section>${K.query.get("dialog") === "create" ? K.modal("Create worktree", `<p class="muted">Start a new task on its own branch.</p><form id="worktree-form" class="new-worktree-form"><label for="branch">Branch name</label><input id="branch" name="branch" placeholder="feature/navigation" value="${K.state === "invalid" ? "feature navigation" : ""}" autocomplete="off" aria-describedby="branch-help branch-error"><p id="branch-help" class="help">KetPlus derives the worktree folder name from your branch.</p><p id="branch-error" class="error-text" role="alert">${K.state === "invalid" ? "Branch names cannot contain spaces." : ""}</p><div class="branch-preview">${K.icon("folder")}<span>ketplus-desktop-feature-navigation</span></div></form>`, `${K.button("Cancel", "close-dialog")}<button class="btn primary" form="worktree-form" type="submit">${K.icon("plus")} Create & open worktree</button>`) : ""}`;
