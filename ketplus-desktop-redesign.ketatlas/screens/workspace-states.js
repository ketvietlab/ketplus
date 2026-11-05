/* Native behavior contract rendered with bounded, synthetic history fixtures. */
K.syncComposerSend = () => {
    const input = document.querySelector("#prompt");
    const send = document.querySelector("#prompt-form button[type=submit]");
    if (input && send && !input.disabled)
        send.disabled = !input.value.trim() && !K.current.attachment;
};
K.composer = (builder = false) => {
    const busy = ["sending", "stopping", "steering", "answer-submitting"].includes(K.current.turn);
    const unavailable = !K.current.available || K.scope.archived || K.scope.deleted;
    const disabled =
        unavailable || K.state === "restore" || K.current.connection !== "connected" || busy;
    const active = K.isActiveTurn();
    const steeringBlocked = active && !K.provider().steer;
    const configurationLocked = disabled || active;
    const attachment = K.current.attachment;
    const label = disabled
        ? K.state === "restore"
            ? "Loading conversation…"
            : "Sending is currently unavailable"
        : active
          ? "Add guidance to this turn…"
          : "What would you like to do next?";
    const mode = K.current.mode || "workspace-write";
    const modes = [
        ["read-only", "Read only", "Inspect files; edits are blocked"],
        ["workspace-write", "Workspace", "Edit project files; network is restricted"],
        ["danger-full-access", "Full access", "Access files and network without sandbox restrictions"],
    ];
    const modeLabel = modes.find((m) => m[0] === mode)?.[1] || "Workspace";
    const modeMenu =
        K.query.get("menu") === "mode"
            ? `<div class="mode-menu" role="menu"><div class="mode-menu-title">Mode</div>${modes
                  .map(
                      ([id, name, text], index) =>
                          `<button type="button" role="menuitemradio" aria-checked="${id === mode}" class="mode-option" data-action="select-mode" data-mode="${id}"><span><b>${name}</b><small>${text}</small></span>${id === "danger-full-access" && id !== mode ? '<span class="mode-enable">Enable</span>' : ""}<span class="mode-check">${id === mode ? "✓" : ""}</span><kbd>${index + 1}</kbd></button>`,
                  )
                  .join("")}</div>`
            : "";
    return `<div class="composer-wrap"><form class="composer" id="prompt-form">${builder ? K.flowComposer() : ""}<div id="attachment-chips" class="attachment-chips">${attachment ? `<span class="attachment-chip">${K.icon("paperclip")}${K.escape(attachment.name)}<button type="button" data-action="remove-attachment" aria-label="Remove attachment">${K.icon("x")}</button></span>` : ""}</div><div class="composer-input"><label class="sr-only" for="prompt">Request for your Agent</label><textarea id="prompt" name="prompt" maxlength="6000" rows="1" ${disabled || steeringBlocked ? "disabled" : ""} placeholder="${K.escape(disabled || active ? label : `Message ${K.current.provider}…`)}">${K.escape(K.current.draft)}</textarea>${active ? K.button("", "stop", "square", "send-button stop", `aria-label="Stop turn" ${busy || K.current.connection !== "connected" ? "disabled" : ""}`) : ""}<button class="btn send-button" type="submit" aria-label="${active ? "Send guidance" : "Send request"}" ${disabled || steeringBlocked || (!K.current.draft.trim() && !attachment) ? "disabled" : ""}>${K.icon("arrow-up")}</button></div><input id="attachment-input" type="file" accept="image/*,.txt,.md,.js,.ts,.json,.css,.html" hidden></form><div class="composer-toolbar composer-foot"><div class="row">${K.ib("plus", "Add images or text files", "attach", configurationLocked ? "disabled" : "")}<span class="mode-anchor">${K.button(modeLabel, "open-mode-menu", "shield", "ghost composer-mode", configurationLocked ? "disabled" : "")}${modeMenu}</span></div><div class="row">${K.button(`${K.escape(K.current.provider)} · ${K.escape(K.current.model)}${K.provider().efforts.length ? `<span class="effort">${K.escape(K.current.effort)}</span>` : ""}`, "models", "", "ghost composer-model", configurationLocked ? "disabled" : "")}<span class="usage-anchor">${K.button("", "usage", null, "usage-ring", `aria-label="Context and limits for ${K.escape(K.current.provider)}" title="Context & limits" aria-expanded="${K.query.get("dialog") === "usage"}"`)}${K.query.get("dialog") === "usage" ? K.usagePanel() : ""}</span></div></div></div>`;
};
K.notice = (title, text, action = "", label = "Retry", tone = "warning") =>
    `<div class="banner ${tone}" role="status">${K.icon(tone === "error" ? "triangle-alert" : "info")}<div><strong>${title}</strong><p>${text}</p></div>${action ? K.button(label, action, "refresh-cw") : ""}</div>`;
K.question = () => {
    if (K.query.get("request") === "expired")
        return K.notice(
            "Question expired",
            "This request no longer accepts answers. Refresh its state without resending your prompt.",
            "refresh-question",
            "Refresh state",
        );
    if (K.query.get("questionMode") === "confirm")
        return `<div class="question-card"><b>Confirmation</b><h3>Lịch sử đã hiện và trạng thái là Đã kết nối chưa?</h3><form id="question-form"><input type="hidden" name="answer" value="Có"><div class="row"><button class="btn primary" type="submit">Có</button><button class="btn" type="submit" onclick="this.form.elements.answer.value='Không'">Không</button></div></form></div>`;
    const secret = K.query.get("secret") === "1";
    const q = K.query.get("question") || "1";
    const pending = K.current.turn === "answer-submitting";
    return `<div class="question-card"><div class="row">${K.icon("circle-help")}<b>Your input is needed</b><span class="spacer"></span><small>${q} / 2</small></div><h3>${q === "1" ? "How much detail should each status show?" : "Any additional constraints?"}</h3><form id="question-form">${q === "1" ? `<label class="choice"><input type="radio" name="answer" value="Status and last update" checked><span>Status and last update</span>${K.badge("Recommended", "brand")}</label><label class="choice"><input type="radio" name="answer" value="Status only"><span>Status only</span></label>` : ""}<input id="question-answer" name="other" type="${secret ? "password" : "text"}" autocomplete="off" value="${secret ? "" : K.escape(K.current.answer)}" placeholder="${q === "1" ? "Or enter another answer…" : "Optional constraints…"}" aria-label="Free-form answer" ${pending ? "disabled" : ""}><div class="row">${q === "2" ? K.button("Back", "question-back", "arrow-left", "ghost") : ""}<span class="spacer"></span><button class="btn primary" type="submit" ${pending ? "disabled" : ""}>${pending ? "Sending…" : q === "1" ? "Next" : "Submit answers"}</button></div></form>${secret ? '<p class="demo-note">Synthetic private input: value is discarded, never serialized. Do not enter a real secret.</p>' : ""}<p class="help">A reply resumes work; it does not mark the turn completed.</p>${K.current.turn === "answer-error" ? K.notice("Reply not sent", "Your answers are preserved. Retry or stop the turn.", "retry-answer", "Retry reply", "error") : ""}</div>`;
};
K.historyControls = () => {
    const state = K.current.reading.page;
    const messages = {
        loading: ["Loading earlier messages…", "Your current reading position is preserved."],
        "page-error": [
            "Earlier messages could not be loaded",
            "Visible messages remain available.",
            "load-older",
            "Retry page",
        ],
        partial: [
            "This turn continues",
            "More items belong to this turn. The conversation is not complete yet.",
            "continue-page",
            "Load remaining items",
        ],
        retention: [
            "Earlier history needs this Mac",
            "The cloud keeps recent content for up to 24 hours. Cached messages remain readable while this Mac is offline.",
            "history-reconnect",
            "Check Mac connection",
        ],
        "rich-loading": [
            "Loading preview…",
            "Only this attachment is loading. You can keep reading.",
        ],
        "rich-error": [
            "Preview unavailable",
            "The message is preserved. Retry loading this preview.",
            "rich-retry",
            "Retry preview",
        ],
    };
    const msg =
        messages[state] ||
        (K.current.reading.older >= 9
            ? [
                  "End of sample history",
                  "This bounded fixture demonstrates pagination; it is not a production history limit.",
              ]
            : null);
    return `<div class="history-controls">${msg ? K.notice(...msg) : K.button("Load earlier messages", "load-older", "arrow-up", "ghost")}<span class="help">Latest 3 turns · items are loaded in bounded pages</span></div>`;
};
K.historyItem = (index) =>
    `<article class="history-entry" data-message-id="older-${index}"><small>Earlier turn ${index + 1}</small><h3>Review checkpoint ${index + 1}</h3><p>This is an earlier saved message in ${K.escape(K.current.title)}. Its identity stays stable when live content arrives.</p><pre><code>git diff --check</code></pre></article>`;
K.olderFixture = () =>
    Array.from({ length: K.current.reading.older || 0 }, (_, index) =>
        K.historyItem(K.current.reading.older - index - 1),
    ).join("");
const basicTranscript = K.transcript;
K.remoteTranscript = () => `<div class="transcript"><div class="day-divider"><span>Today, 09:41</span></div><article class="user-message"><div class="message-label"><span>You</span><span class="avatar">AN</span></div><div class="user-bubble">Check the Linux build on build-node-01.</div></article><article class="assistant-message transcript-turn"><div class="assistant-body"><div class="activity-stream">${K.commandActivity("Ran", "cmake --build build/linux --target api-service -j4")}${K.commandActivity("Ran", "ctest --test-dir build/linux --output-on-failure")}</div>${K.current.turn === "running" ? `<div class="current-activity">${K.icon("loader-circle")} Working on build-node-01…</div>` : `<div class="answer final-answer" data-transcript-layer="final"><h3>Linux build completed</h3><p>The sample build and focused tests finished on <strong>build-node-01</strong>. Files and running processes remain on that Linux machine.</p></div>`}</div></article></div>`;
K.transcript = () => {
    if (K.state === "empty")
        return (
            (K.query.get("send") === "failed"
                ? K.notice(
                      "Request not sent",
                      "Your draft and attachment are preserved. Retry when ready.",
                      "",
                      "",
                      "error",
                  )
                : "") + basicTranscript()
        );
    if (K.state === "restore")
        return `<div class="restore-state" role="status">${K.mark("large")}<p>Loading conversation…</p><p class="help">Other workspaces and Settings remain available.</p>${K.link("settings", "Open Settings", "settings", {}, "btn ghost")}</div>`;
    const state = K.state;
    K.state = ["running", "question", "ready", "stopped"].includes(K.current.turn)
        ? K.current.turn
        : "running";
    let body = K.repo === "api-service" ? K.remoteTranscript() : basicTranscript();
    K.state = state;
    const prompt =
        K.current.latestPrompt ||
        (K.repo === "api-service"
            ? "Check the Linux build on build-node-01."
            : K.activeConversation === "navigation"
            ? "Refine navigation in this worktree."
            : `Continue: ${K.current.title}`);
    body = body.replace(/<div class="user-bubble">[\s\S]*?<\/div><\/article>/,
        `${K.userBubble(K.query.get("message") === "long" ? Array(24).fill(prompt).join("\n") : prompt)}</article>`);
    let notice = "";
    if (K.query.get("stop") === "failed")
        notice += K.notice(
            "Stop not confirmed",
            "The turn may still be running. Retry Stop after reconnecting.",
            "stop",
            "Retry Stop",
            "error",
        );
    if (!K.current.available)
        notice += K.notice(
            "Worktree folder was deleted",
            "The agent keeps working from the repository's main worktree. If it recreates a worktree, the conversation moves there.",
        );
    if (K.repo === "api-service" && K.current.connection !== "connected")
        notice += K.notice(
            "Linux machine offline",
            "This repository stays on Linux. Reconnect the machine in Connections before sending another request.",
            "open-remote-connections",
            "Open Connections",
            "warning",
        );
    else if (K.current.connection !== "connected")
        notice += K.notice(
            K.current.connection === "offline" ? "Connection interrupted" : "Reconnecting…",
            "The turn may still be running. Reconnect to reconcile its state; your prompt will not be resent.",
            K.current.connection === "offline" ? "retry" : "",
            "Reconnect",
        );
    if (K.current.turn === "stopping")
        notice += K.notice(
            "Stopping turn…",
            "Waiting for acknowledgment. The turn is not yet confirmed stopped.",
        );
    if (K.current.turn === "sending")
        notice += K.notice(
            "Sending request…",
            "Your draft is retained until the request is acknowledged.",
        );
    if (K.current.turn === "steering")
        notice += K.notice(
            "Sending guidance…",
            "Your draft is retained until the active turn acknowledges it.",
        );
    if (["answer-submitting", "answer-error"].includes(K.current.turn)) notice += K.question();
    if (K.query.get("answer") === "received")
        notice += K.notice(
            "Answers received",
            "The Agent has resumed work. Completion is reported separately.",
            "complete-turn",
            "Simulate completion",
            "",
        );
    if (K.query.get("answer") === "resolved")
        notice += K.notice(
            "Already answered on another device",
            "This request is closed. The latest turn state has been refreshed.",
            "complete-turn",
            "Simulate completion",
            "",
        );
    if (K.query.get("send") === "failed")
        notice += K.notice(
            "Guidance not sent",
            "Your draft is preserved. Sending again targets the same turn.",
            "retry-steer",
            "Retry guidance",
            "error",
        );
    return `${notice}${K.historyControls()}<div id="history-items">${K.olderFixture()}</div>${body}${K.repo === "api-service" ? "" : `<div class="rich-preview"><b>Referenced design</b><p class="help">Code, diff, table, diagram and media detail are loaded on demand.</p>${K.button("Open preview", "rich-open", "panels-top-left", "ghost")}</div>`}`;
};
const builderView = K.views["atlas-builder"];
K.views["atlas-builder"] = () => {
    const state = K.query.get("bundleState");
    const banner =
        state === "missing"
            ? K.notice(
                  "Atlas bundle unavailable",
                  "It may have moved or been deleted. Choose the original bundle; do not silently attach another.",
                  "choose-bundle",
                  "Choose bundle",
              )
            : state === "invalid"
              ? K.notice(
                    "New files are not valid yet",
                    "The last valid preview is preserved while the Agent updates this bundle.",
                    "validate-bundle",
                    "Simulate valid files",
                )
              : "";
    return builderView().replace(
        '<div class="builder-scroll">',
        '<div class="builder-scroll">' + banner,
    );
};
const previewView = K.views["atlas-preview"];
K.views["atlas-preview"] = () =>
    K.scope.conversations[K.bundleConversation(K.scope, K.bundle).id]?.bundleReady === false
        ? '<section class="view"><div class="empty-state"><h2>Preview pending validation</h2><p>Turn completion does not guarantee a valid bundle.</p>' +
          K.link("atlas-builder", "Continue designing", "messages-square", {}, "btn") +
          "</div></section>"
        : K.query.get("bundleState") === "missing" || !K.bundleTitles[K.bundle]
          ? '<section class="view"><div class="empty-state"><h2>Atlas bundle unavailable</h2><p>Choose the original bundle to resume.</p>' +
            K.link("atlas-library", "Choose bundle", "folder", {}, "btn") +
            "</div></section>"
          : previewView();
const workspaceView = K.views.workspace;
K.views.workspace = () =>
    workspaceView()
        .replace(
            '<div class="chat-scroll">',
            '<div class="transcript-viewport"><div class="chat-scroll" tabindex="0" aria-label="Conversation history">',
        )
        .replace(
            '</div><div class="composer-wrap">',
            `</div><div class="latest-bar" ${K.current.reading.follow && K.current.reading.page !== "new-content" ? "hidden" : ""}>${K.button("", "jump-latest", "arrow-down", "latest-button", 'aria-label="Jump to latest" title="Jump to latest"')}</div></div><div class="composer-wrap">`,
        );
