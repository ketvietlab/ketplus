/* Background tasks reported by the Claude Agent SDK (background_tasks_changed,
   task_started/progress/notification). Fixtures are synthetic; `bg` selects the state. */
Object.assign(window.KET_ICONS, {
    // Lucide 1.40.0 (ISC), only the glyphs this surface adds.
    bot: '<path d="M12 8V4H8"/><rect width="16" height="12" x="4" y="8" rx="2"/><path d="M2 14h2"/><path d="M20 14h2"/><path d="M15 13v2"/><path d="M9 13v2"/>',
    "circle-stop": '<circle cx="12" cy="12" r="10"/><rect x="9" y="9" width="6" height="6" rx="1"/>',
    "wifi-off":
        '<path d="M12 20h.01"/><path d="M8.5 16.429a5 5 0 0 1 7 0"/><path d="M5 12.859a10 10 0 0 1 5.17-2.69"/><path d="M19 12.859a10 10 0 0 0-2.007-1.523"/><path d="M2 8.82a15 15 0 0 1 4.177-2.643"/><path d="M22 8.82a15 15 0 0 0-11.288-3.764"/><path d="m2 2 20 20"/>',
});

K.bgState = K.query.get("bg");
K.bgTasks = () => {
    const state = K.bgState;
    if (!state) return [];
    const shell = {
        id: "build",
        type: "shell",
        title: "Rebuild agent tests",
        command: "cmake --build build/mobile-fork --target ketplus_agent_tests -j4",
        elapsed: "3m 12s",
        status: "running",
    };
    const agent = {
        id: "review",
        type: "agent",
        title: "Review line-height diff",
        summary: "Comparing desktop leading with native SwiftUI metrics",
        lastTool: "Read",
        tools: 14,
        tokens: "38k",
        elapsed: "1m 40s",
        status: "running",
    };
    if (state === "stopping") shell.status = "stopping";
    if (state === "completed") Object.assign(shell, { status: "completed", result: "Exit 0", elapsed: "4m 05s" });
    if (state === "failed") Object.assign(shell, { status: "failed", result: "Exit 2", elapsed: "52s" });
    if (state === "stopped") Object.assign(shell, { status: "stopped", result: "Stopped by you", elapsed: "3m 30s" });
    if (state === "orphaned")
        Object.assign(shell, { status: "stopped", result: "KetPlus restarted while it ran", elapsed: "3m 12s" });
    if (state === "agent") return [agent];
    return [shell, agent];
};
K.bgOffline = () => K.current.connection !== "connected";
K.bgMeta = (task) => {
    if (task.status === "stopping") return "Stopping…";
    if (task.result) return `${task.result} · ${task.elapsed}`;
    return task.type === "agent"
        ? `Subagent · ${task.tools} tools · ${task.tokens} tokens · ${task.elapsed}`
        : `Shell · ${task.elapsed}`;
};
K.bgStatusIcon = (task) =>
    // Offline rows are last known: never animate a state nobody is reporting.
    (K.bgOffline() && task.status === "running" ? K.icon("clock", "bg-muted") : null) ||
    ({
        running: K.icon("loader-circle", "bg-spin"),
        stopping: K.icon("loader-circle", "bg-spin"),
        completed: K.icon("circle-check", "bg-ok"),
        failed: K.icon("circle-x", "bg-fail"),
        stopped: K.icon("circle-stop", "bg-muted"),
    })[task.status];
K.bgBar = () => {
    const tasks = K.bgTasks();
    if (!tasks.length) return "";
    const live = tasks.filter((t) => ["running", "stopping"].includes(t.status));
    const offline = K.bgOffline();
    const collapsed = K.query.get("bgCollapsed") === "1";
    const turnDone = !K.isActiveTurn();
    const headline = offline
        ? `${live.length} background ${live.length === 1 ? "task" : "tasks"} · last known`
        : live.length
          ? `${turnDone ? "Turn finished · " : ""}${live.length} ${live.length === 1 ? "task" : "tasks"} running in background`
          : "Background tasks finished";
    const rows = tasks
        .map((task) => {
            const active = task.status === "running";
            const actions = `${K.button(task.type === "shell" ? "Output" : "Progress", "bg-open", null, "bg-row-button", `data-task="${task.id}"`)}${
                active
                    ? `<button type="button" class="bg-row-stop" data-action="bg-stop" data-task="${task.id}" aria-label="Stop ${K.escape(task.title)}" title="${offline ? "Stopping needs a live connection to the daemon" : "Stop task"}" ${offline ? "disabled" : ""}>${K.icon("square")}</button>`
                    : task.status === "stopping"
                      ? ""
                      : `<button type="button" class="bg-dismiss" data-action="bg-dismiss" aria-label="Dismiss ${K.escape(task.title)}">${K.icon("x")}</button>`
            }`;
            return `<div class="bg-row is-${task.status}${K.query.get("task") === task.id ? " current" : ""}">${K.bgStatusIcon(task)}${K.icon(task.type === "shell" ? "terminal" : "bot", "bg-kind")}<span class="bg-title">${K.escape(task.title)}</span><span class="bg-meta">${K.escape(K.bgMeta(task))}</span>${actions}</div>`;
        })
        .join("");
    return `<div class="bg-bar${offline ? " is-offline" : ""}" aria-label="Background tasks"><button type="button" class="bg-bar-header" data-action="bg-toggle" aria-expanded="${!collapsed}">${K.icon(collapsed ? "chevron-right" : "chevron-down")}${offline ? K.icon("wifi-off") : live.length ? K.icon("loader-circle", "bg-spin") : K.icon("circle-check", "bg-ok")}<span>${headline}</span></button>${
        collapsed ? "" : rows
    }${offline && !collapsed ? `<p class="bg-note">The daemon is unreachable. These tasks may have ended; Stop and live output return when it reconnects.</p>` : ""}</div>`;
};

const bgComposer = K.composer;
K.composer = (builder = false) => {
    const html = bgComposer(builder);
    return builder ? html : html.replace('<div class="composer-wrap">', `<div class="composer-wrap">${K.bgBar()}`);
};

K.bgTask = () => K.bgTasks().find((t) => t.id === K.query.get("task")) || K.bgTasks()[0];
K.bgPaneTitle = () => {
    const task = K.bgTask();
    return task ? `${K.icon(task.type === "shell" ? "terminal" : "bot")} ${K.escape(task.title)}` : "Background task";
};
K.bgPane = () => {
    const task = K.bgTask();
    if (!task) return `<div class="bg-pane"><p class="bg-note">This task is no longer reported by the agent.</p></div>`;
    const offline = K.bgOffline();
    const live = task.status === "running";
    const output = K.query.get("output");
    const head = `<div class="bg-pane-head">${K.bgStatusIcon(task)}<div><strong>${K.escape(K.bgMeta(task))}</strong>${task.type === "shell" ? `<code>${K.escape(task.command)}</code>` : `<p>${K.escape(task.summary)}</p>`}</div>${
        live
            ? K.button("Stop", "bg-stop", "square", "ghost", `data-task="${task.id}" ${offline ? "disabled" : ""}`)
            : ""
    }</div>`;
    if (task.type === "agent")
        return `<div class="bg-pane">${head}<dl class="bg-facts"><dt>Last tool</dt><dd>${task.lastTool}</dd><dt>Tool calls</dt><dd>${task.tools}</dd><dt>Tokens</dt><dd>${task.tokens}</dd><dt>Elapsed</dt><dd>${task.elapsed}</dd></dl><p class="bg-note">Subagents report progress, not a terminal log. Their final summary appears in the conversation when they finish.</p></div>`;
    const tail =
        output === "empty"
            ? `<p class="bg-empty">No output yet.</p>`
            : `<pre class="bg-output"><code>[1/4] Automatic MOC and UIC for target ketplus_agent_tests
[2/4] Building CXX object tests/CMakeFiles/ketplus_agent_tests.dir/AgentWorkspaceRenderingTest.cpp.o
${task.status === "running" || task.status === "stopping" ? "[3/4] Linking CXX executable ketplus_agent_tests" : task.status === "failed" ? `tests/AgentWorkspaceRenderingTest.cpp:1015:13: error: use of undeclared identifier 'size'
ninja: build stopped: subcommand failed.` : task.status === "completed" ? `[3/4] Linking CXX executable ketplus_agent_tests
[4/4] Copying test resources
build=0` : "[3/4] Linking CXX executable ketplus_agent_tests\n^C ninja: build stopped: interrupted by user."}</code></pre>`;
    const caption = offline
        ? `${K.icon("wifi-off")} Showing the last tail received before the daemon disconnected.`
        : output === "empty"
          ? ""
          : `${K.icon("file-text")} Last 8 KiB of 1.4 MB${live ? " · refreshes every 2 s" : ""}`;
    return `<div class="bg-pane">${head}<div class="bg-output-wrap">${tail}${caption ? `<p class="bg-caption">${caption}</p>` : ""}</div><div class="bg-pane-foot">${K.button("Copy output", "bg-copy", "copy", "ghost", output === "empty" ? "disabled" : "")}${K.button("Open full log", "bg-full-log", "external-link", "ghost")}</div></div>`;
};

const bgDialog = K.productDialog;
K.productDialog = () => {
    if (K.query.get("dialog") !== "stop-task") return bgDialog?.() || "";
    const task = K.bgTask();
    const shell = task?.type === "shell";
    return K.modal(
        "Stop background task?",
        `<p><strong>${K.escape(task?.title || "")}</strong></p><p class="muted">${
            shell
                ? "The command is terminated on this Mac. Output written so far stays available."
                : "The subagent stops after its current step. Its partial result is not added to the conversation."
        }</p>`,
        `${K.button("Keep running", "close-dialog", null, "ghost")}${K.button("Stop task", "bg-confirm-stop", "square", "danger")}`,
        "bg-stop-dialog",
    );
};

const bgQuery = (changes) => {
    const params = Object.fromEntries(K.query);
    Object.entries(changes).forEach(([key, value]) => (value === null ? delete params[key] : (params[key] = value)));
    return params;
};
document.addEventListener("click", (event) => {
    const button = event.target.closest("[data-action^='bg-']");
    if (!button || button.disabled) return;
    event.preventDefault();
    const task = button.dataset.task;
    const go = (changes) => K.go(K.page, bgQuery(changes));
    ({
        "bg-toggle": () => go({ bgCollapsed: K.query.get("bgCollapsed") === "1" ? null : "1" }),
        "bg-open": () => go({ panel: "task", task }),
        "bg-stop": () => go({ dialog: "stop-task", task }),
        "bg-confirm-stop": () => go({ dialog: null, bg: "stopping" }),
        "bg-dismiss": () => go({ bg: "agent", panel: null, task: null }),
        "bg-copy": () => K.toast("Output copied."),
        "bg-full-log": () => K.toast("Opens the full log file in the editor."),
    })[button.dataset.action]?.();
});
