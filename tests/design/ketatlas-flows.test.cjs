// DOM/model tests only: no browser rendering, network, provider, Git mutation or native APIs.
// Install jsdom@26.1.0 into an external test prefix; see the bundle README.
const nodeTest = require("node:test");
const shardSpec = process.env.KETATLAS_TEST_SHARD;
const shardMatch = shardSpec && /^(\d+)\/(\d+)$/.exec(shardSpec);
if (shardSpec && (!shardMatch || Number(shardMatch[2]) < 1 || Number(shardMatch[1]) >= Number(shardMatch[2])))
    throw new Error("KETATLAS_TEST_SHARD must be an index/count pair such as 0/9");
let testOrdinal = 0;
const test = (name, callback) => {
    const ordinal = testOrdinal++;
    if (!shardMatch || ordinal % Number(shardMatch[2]) === Number(shardMatch[1]))
        nodeTest(name, callback);
};
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { screen, root, manifest } = require("./ketatlas-dom.cjs");
const urls = new Set(manifest.screens.map((s) => s.url));
test("sandboxed previews embed KÉT Sans 1.1 and use it for every text token", () => {
    const css = fs.readFileSync(path.join(root, "assets/fonts-inline.css"), "utf8");
    assert.ok(css.startsWith('@charset "UTF-8";'));
    // Released weights only: Light, Regular, Medium (also semibold) and Bold.
    const faces = [...css.matchAll(/@font-face\{font-family:"KÉT Sans";src:url\(data:font\/woff2;base64,[A-Za-z0-9+/=]+\) format\("woff2"\);font-weight:([0-9 ]+);/g)];
    assert.deepEqual(faces.map((face) => face[1]), ["300", "400", "500 600", "700"]);
    assert.doesNotMatch(css, /Inter/);
    assert.match(css, /--kv-font-sans:"KÉT Sans",ui-sans-serif,sans-serif/);
    // Code uses KÉT Sans Mono 0.10.4 Regular only; bold code is synthesized.
    assert.match(css, /@font-face\{font-family:"KÉT Sans Mono";src:url\(data:font\/woff2;base64,[A-Za-z0-9+/=]+\) format\("woff2"\);font-weight:400;/);
    assert.doesNotMatch(css, /font-family:"KÉT Sans Mono";src:url\([^)]*\) format\("woff2"\);font-weight:500;/);
    assert.match(css, /--kv-font-mono:"KÉT Sans Mono",ui-monospace,/);
    for (const licence of ["KET-Sans-OFL-1.1.txt", "KET-Sans-Mono-OFL-1.1.txt", "Ket-Symbol-CC-BY-4.0.txt"])
        assert.ok(fs.existsSync(path.join(root, "licenses", licence)), licence);
    assert.match(css, /KÉT Sans 1\.1 \(tag v1\.1\.0/);
    for (const screen of manifest.screens) {
        const html = fs.readFileSync(path.join(root, screen.url), "utf8");
        assert.match(html, /\.\.\/assets\/fonts-inline\.css/);
    }
});
manifest.flows.forEach((flow) =>
    flow.nodes.forEach((node) =>
        urls.add(node.url || manifest.screens.find((s) => s.id === node.screen)?.url),
    ),
);
for (const url of urls)
    if (url)
        test(`render addressable state: ${url}`, () => {
            const s = screen(url);
            assert.ok(s.document.querySelector("#app").textContent.trim());
            if (s.K.query.has("context")) assert.ok(s.document.querySelector(".desktop-context-menu"), "Direct menu fixture must open its menu");
            assert.deepEqual(
                s.errors.map((e) => e.message),
                [],
            );
            assert.equal(s.document.querySelectorAll('[id="prompt"]').length <= 1, true);
            s.close();
        });
test("the KétPlus shell has no KetSuite application switcher", () => {
    for (const page of ["screens/workspace.html", "screens/connections.html", "screens/repository-picker.html"]) {
        const s = screen(page);
        const shell = s.document.querySelector('[data-ui="app-shell"]');
        assert.ok(shell, page);
        assert.equal(shell.querySelector('[data-ui="app-switcher"]'), null, page);
        assert.equal(shell.firstElementChild.dataset.ui, "app-sidebar", page);
        assert.match(fs.readFileSync(path.join(root, "screens/desktop.css"), "utf8"), /--kv-app-switcher-width: 0px;/);
        s.close();
    }
});

test("agent messages name their sender, and the header shows the conversation number (ADR 0027)", () => {
    const received = screen("screens/workspace.html?conversation=startup");
    assert.match(received.document.querySelector(".agent-message .agent-origin").textContent.trim(),
        /^Message from @agent\/\d{4} · Refine navigation · Codex$/);
    assert.equal(received.document.querySelector(".transcript > .user-message:not(.agent-message)"), null);
    assert.match(received.document.querySelector(".agent-heading .header-number").textContent, /^@agent\/\d{4}$/);
    // The sidebar shows bare numbers; `#1234` would read as an issue or pull request.
    const numbers = [...received.document.querySelectorAll(".sidebar-inner .conversation-number")].map(n => n.textContent.trim());
    assert.ok(numbers.length > 1);
    assert.ok(numbers.every(text => /^\d{4,5}$/.test(text)), numbers.join(", "));
    // Rows do not name the sending agent; the transcript's origin line does.
    assert.equal(received.document.querySelector(".sidebar-inner .conversation-sender"), null);
    assert.doesNotMatch(received.document.querySelector(".sidebar-inner").textContent, /\bfrom \d{4}/);
    received.close();
    const sent = screen("screens/workspace.html?conversation=navigation");
    assert.match(sent.document.querySelector(".agent-note").textContent.trim(),
        /Sent to @agent\/\d{4} · Review the startup flow · started a turn · reply requested\.$/);
    assert.ok(sent.document.querySelector(".agent-tool-row"));
    sent.close();
    const held = screen("screens/workspace.html?conversation=navigation&agent=held");
    assert.match(held.document.querySelector(".agent-note.held").textContent, /is held because the agents exchanged too many messages/);
    held.close();
    const reply = screen("screens/workspace.html?conversation=navigation&agent=reply");
    const turn = reply.document.querySelector(".transcript > .assistant-message");
    const bubble = reply.document.querySelector(".transcript > .agent-message");
    assert.match(bubble.querySelector(".agent-origin").textContent.trim(), /^Reply from @agent\/\d{4} · Review the startup flow/);
    assert.equal(turn.nextElementSibling, bubble, "the reply opens the turn after the sending turn");
    reply.close();
});
test("Copy @agent/ copies the conversation reference", () => {
    const s = screen("screens/workspace.html?context=conversation");
    const copy = s.document.querySelector('[data-context-command="copy-agent-reference"]');
    assert.match(copy.textContent, /^Copy @agent\/\d{4}$/);
    s.click('[data-context-command="copy-agent-reference"]');
    assert.match(s.K.demo.menuClipboard, /^@agent\/\d{4}$/);
    s.close();
});
test("@agent/ in the composer completes another conversation's number; a plain @ does not", () => {
    const s = screen("screens/workspace.html?conversation=navigation");
    const prompt = s.document.querySelector("#prompt");
    prompt.value = "Ask @src";
    prompt.setSelectionRange(8, 8);
    prompt.dispatchEvent(new s.w.Event("input", { bubbles: true }));
    assert.equal(s.document.querySelector(".mention-menu"), null, "a plain @ stays a file mention");
    prompt.value = "Ask @agent/";
    prompt.setSelectionRange(11, 11);
    prompt.dispatchEvent(new s.w.Event("input", { bubbles: true }));
    const own = s.document.querySelector(".header-number").textContent.replace("@agent/", "");
    const options = [...s.document.querySelectorAll(".mention-option")];
    assert.ok(options.length > 0);
    assert.ok(options.every(option => option.dataset.conversationNumber !== own), "a conversation cannot message itself");
    const first = options[0].dataset.conversationNumber;
    prompt.dispatchEvent(new s.w.KeyboardEvent("keydown", { key: "Enter", bubbles: true }));
    assert.equal(prompt.value, `Ask @agent/${first} `);
    assert.equal(s.document.querySelector(".mention-menu"), null);
    s.close();
});
test("sidebar rows: folder-led repositories, label-only tag chips and a trailing status icon", () => {
    const s = screen("screens/workspace.html");
    const chips = [...s.document.querySelectorAll(".conversation-tag")];
    assert.ok(chips.length > 0);
    for (const chip of chips) assert.equal(chip.querySelector(".tag-dot"), null, chip.textContent.trim());
    // The status icon trails each row so titles line up with their repository heading.
    for (const row of s.document.querySelectorAll(".sidebar-inner .conversation:has(.conversation-lines)"))
        assert.ok(row.lastElementChild.classList.contains("conversation-indicator"), row.textContent.trim());
    // Each repository heading leads with a folder that shows whether the group is open.
    const heading = s.document.querySelector(".repository-group:not(.remote-repository) .repository-group-name");
    const folderOpen = /d="([^"]+)"/.exec(s.w.KET_ICONS["folder-open"])[1];
    assert.equal(heading.querySelector(":scope > svg.icon path").getAttribute("d"), folderOpen);
    s.close();
});
test("conversation header is scoped to chat; the detail pane owns its top row", () => {
    for (const panel of [null, "task", "terminal", "file", "atlas"]) {
        const s = screen(`screens/workspace.html?state=running${panel ? `&panel=${panel}` : ""}`);
        const view = s.document.querySelector(".agent-view");
        const chat = view.querySelector(".chat-pane");
        const heading = chat.querySelector(":scope > .agent-heading");
        assert.ok(heading, panel || "chat only");
        assert.equal(view.querySelector(":scope > .agent-heading"), null);
        // Tag chips open the tag editor (ADR 0028); the trailing row holds the conversation actions.
        assert.ok(heading.querySelector(".header-tags [data-action='edit-header-tags']"));
        assert.deepEqual([...heading.querySelectorAll(":scope > .row [data-action]")].map(node => node.dataset.action), ["new-chat", "open-atlas-panel", "terminal"]);
        const detail = view.querySelector(".context-pane");
        assert.equal(Boolean(detail), Boolean(panel));
        if (detail) assert.ok(detail.querySelector(":scope > .context-tabs"));
        s.close();
    }
});
test("Atlas control opens a worktree list and tracks running viewers across conversations", () => {
    const a = screen("screens/workspace.html?conversation=navigation");
    assert.equal(a.document.querySelector('.sidebar [href*="atlas-"]'), null);
    a.click('[data-action="open-atlas-panel"]');
    const list = a.follow();
    assert.equal(list.K.query.get("panel"), "atlas");
    assert.equal(list.document.querySelectorAll("[data-atlas-row]").length, 2);
    assert.equal(list.document.querySelector(".atlas-header-count"), null);
    list.click('[data-action="atlas-open"][data-atlas-id="desktop"]');
    const starting = list.follow();
    assert.equal(starting.document.querySelector('[data-action="atlas-open"][data-atlas-id="desktop"]').disabled, true);
    starting.tick(650);
    const running = starting.follow();
    assert.equal(running.document.querySelector(".atlas-header-count").textContent, "1");
    assert.equal(running.document.querySelectorAll('[data-action="atlas-stop"]').length, 1);
    running.K.go("workspace", { conversation: "startup", panel: "atlas" });
    const other = running.follow();
    assert.equal(other.document.querySelector(".atlas-header-count").textContent, "1");
    other.click('[data-action="atlas-stop"][data-atlas-id="desktop"]');
    const stopping = other.follow();
    assert.equal(stopping.document.querySelector(".atlas-header-count").textContent, "1");
    assert.equal(stopping.document.querySelector('[data-action="atlas-stop"]').disabled, true);
    stopping.tick(650);
    const stopped = stopping.follow();
    assert.equal(stopped.document.querySelector(".atlas-header-count"), null);
    assert.match(stopped.document.querySelector('[role="status"].atlas-panel-notice').textContent, /stopped from KétPlus/);
});
test("Atlas Edit opens the selected bundle in KétPlus and returns to the same conversation", () => {
    const list = screen("screens/workspace.html?conversation=navigation&panel=atlas");
    assert.equal(list.document.querySelector('.sidebar [href*="atlas-"]'), null);
    assert.equal(list.document.querySelectorAll('[data-action="atlas-edit"]').length, 2);
    list.click('[data-action="atlas-edit"][data-atlas-id="desktop"]');
    const editor = list.follow();
    assert.equal(editor.K.page, "atlas-builder");
    assert.equal(editor.K.bundle, "desktop");
    assert.equal(editor.K.query.get("fromConversation"), "navigation");
    assert.match(editor.document.querySelector(".atlas-map-toolbar strong").textContent, /KétPlus Desktop/);
    assert.ok(editor.document.querySelector(".atlas-flow-sidebar"));
    editor.click('[data-action="toggle-atlas-flows"]');
    assert.equal(editor.document.querySelector(".builder-map").classList.contains("flows-open"), true);
    assert.equal(editor.document.querySelector('[data-action="toggle-atlas-flows"]').getAttribute("aria-expanded"), "true");
    editor.click('[data-action="toggle-atlas-flows"]');
    assert.equal(editor.document.querySelector(".builder-map").classList.contains("flows-open"), false);
    assert.equal(editor.document.querySelector('.sidebar [href*="atlas-"]'), null);
    editor.click('[data-action="close-atlas-editor"]');
    const returned = editor.follow();
    assert.equal(returned.K.page, "workspace");
    assert.equal(returned.K.activeConversation, "navigation");
    assert.equal(returned.K.query.get("panel"), "atlas");
    assert.equal(returned.K.query.get("fromConversation"), null);
});
test("Atlas editor preserves the last valid map and recovers to the list if its bundle moves", () => {
    const invalid = screen("screens/atlas-builder.html?bundle=desktop&fromConversation=navigation&bundleState=invalid");
    assert.ok(invalid.document.querySelector(".atlas-flow-sidebar"));
    assert.match(invalid.document.querySelector(".builder-scroll").textContent, /last valid preview/i);
    const missing = screen("screens/atlas-builder.html?bundle=desktop&fromConversation=navigation&bundleState=missing");
    assert.match(missing.document.querySelector(".builder-scroll").textContent, /bundle unavailable/i);
    missing.click('[data-action="choose-bundle"]');
    const recovered = missing.follow();
    assert.equal(recovered.K.page, "workspace");
    assert.equal(recovered.K.activeConversation, "navigation");
    assert.equal(recovered.K.query.get("panel"), "atlas");
    const offline = screen("screens/workspace.html?repo=api-service&machine=linux-build-01&remote=offline&panel=atlas");
    offline.K.scope.atlasBundles = { remote: "Remote design" };
    offline.K.mount();
    assert.equal(offline.document.querySelector('[data-action="atlas-edit"]').disabled, true);
});
test("Atlas badge follows worktree ownership while another worktree remains active", () => {
    const a = screen("screens/workspace.html?panel=atlas");
    a.click('[data-action="atlas-open"][data-atlas-id="desktop"]');
    const starting = a.follow();
    starting.tick(650);
    const running = starting.follow();
    running.K.go("workspace", { tree: "feature-onboarding", panel: "atlas" });
    const otherTree = running.follow();
    assert.equal(otherTree.document.querySelector(".atlas-header-count").textContent, "1");
    assert.match(otherTree.document.querySelector(".atlas-other-worktrees").textContent, /KétPlus Desktop/);
    otherTree.click('.atlas-other-worktrees [data-action="atlas-stop"]');
    const stopping = otherTree.follow();
    assert.equal(stopping.document.querySelector(".atlas-header-count").textContent, "1");
    stopping.tick(650);
    const stopped = stopping.follow();
    assert.equal(stopped.document.querySelector(".atlas-header-count"), null);
    stopped.K.go("workspace", { tree: "develop", panel: "atlas" });
    const original = stopped.follow();
    assert.equal(original.document.querySelector(".atlas-header-count"), null);
});
test("Atlas list has direct empty, loading, stopping, error, multiple and browser-closed states", () => {
    const empty = screen("screens/workspace.html?panel=atlas&atlasList=empty");
    assert.match(empty.document.querySelector(".atlas-panel-empty").textContent, /No Atlases/);
    empty.click('[data-action="atlas-new"]');
    assert.equal(empty.document.querySelector("#prompt").value, "Create an Atlas for this conversation.");
    const loading = screen("screens/workspace.html?panel=atlas&atlasView=starting");
    assert.equal(loading.document.querySelector('[data-action="atlas-open"][data-atlas-id="desktop"]').disabled, true);
    const stopping = screen("screens/workspace.html?panel=atlas&atlasView=stopping");
    assert.equal(stopping.document.querySelector('[data-action="atlas-stop"]').disabled, true);
    const error = screen("screens/workspace.html?panel=atlas&atlasView=error");
    assert.ok(error.document.querySelector('[role="alert"] [data-action="atlas-retry"]'));
    error.click('[data-action="atlas-retry"]');
    assert.equal(error.follow().K.query.get("atlasView"), null);
    const multiple = screen("screens/workspace.html?panel=atlas&atlasView=multiple");
    assert.equal(multiple.document.querySelector(".atlas-header-count").textContent, "2");
    const closed = screen("screens/workspace.html?panel=atlas&atlasView=browser-closed");
    assert.equal(closed.document.querySelector(".atlas-header-count"), null);
    assert.match(closed.document.querySelector(".atlas-panel-notice").textContent, /stopped automatically/);
});
test("Atlas search filters the current worktree without losing recovery", () => {
    const s = screen("screens/workspace.html?panel=atlas");
    s.input("#atlas-conversation-search", "no matching atlas");
    assert.equal([...s.document.querySelectorAll("[data-atlas-row]")].every(row => row.hidden), true);
    assert.equal(s.document.querySelector(".atlas-filter-empty").hidden, false);
    s.input("#atlas-conversation-search", "onboarding");
    assert.equal([...s.document.querySelectorAll("[data-atlas-row]")].filter(row => !row.hidden).length, 1);
});
test("an Agent Atlas request adds a bundle only after its acknowledged turn", () => {
    const empty = screen("screens/workspace.html?repo=api-service&panel=atlas");
    assert.ok(empty.document.querySelector(".atlas-panel-empty"));
    empty.click('[data-action="atlas-new"]');
    empty.submit("#prompt-form");
    const sending = empty.follow();
    assert.ok(sending.document.querySelector(".atlas-panel-empty"));
    sending.tick(920);
    const generated = sending.follow();
    assert.equal(generated.document.querySelectorAll("[data-atlas-row]").length, 1);
    assert.match(generated.document.querySelector("[data-atlas-row]").textContent, /Agent generated/);
});
test("selected conversation renames by sidebar double-click without a header menu", () => {
    const a = screen();
    const row = a.document.querySelector(".conversation.selected");
    row.dispatchEvent(new a.w.MouseEvent("dblclick", { bubbles: true, cancelable: true }));
    const rename = a.follow();
    assert.equal(rename.K.query.get("dialog"), "rename");
    assert.equal(rename.document.querySelector('[role="dialog"] h2').textContent, "Rename conversation");
});
test("a pasted handoff titles a new conversation from its user request", () => {
    const a = screen();
    const handoff = "# Prior context\nMany instructions.\nUser request: Fix the diff gutter\n<ketplus_response_preferences>more";
    assert.equal(a.K.suggestConversationTitle(handoff), "Fix the diff gutter");
    assert.equal(a.K.suggestConversationTitle("Review the current implementation"),
        "Review the current implementation");
});
test("malformed demo fragments recover to a usable signed-out mock", () => {
    const baseline = screen();
    const malformed = JSON.parse(JSON.stringify(baseline.K.demo));
    malformed.auth = "signed-in";
    malformed.devices = null;
    const address = `screens/connections.html#${new URLSearchParams({ demo: JSON.stringify(malformed) })}`;
    const recovered = screen(address);
    assert.equal(recovered.K.demo.auth, "signed-out");
    assert.deepEqual(Array.from(recovered.K.demo.devices), ["iphone"]);
    assert.match(recovered.document.body.textContent, /Sign in/);
    assert.deepEqual(recovered.errors, []);
});
test("restored device IDs and pending operations must match the demo contract", () => {
    const baseline = screen();
    const device = JSON.parse(JSON.stringify(baseline.K.demo));
    device.auth = "signed-in";
    device.devices = ['pixel"><img id="injected">'];
    const deviceUrl = `screens/connections.html#${new URLSearchParams({ demo: JSON.stringify(device) })}`;
    const safeDevice = screen(deviceUrl);
    assert.equal(safeDevice.document.querySelector("#injected"), null);
    assert.equal(safeDevice.K.demo.auth, "signed-out");
    assert.deepEqual(safeDevice.errors, []);

    const operation = JSON.parse(JSON.stringify(baseline.K.demo));
    operation.operation = {
        kind: "create", page: "workspace", scope: "ketplus-desktop:develop",
        conversation: "navigation", payload: { tree: { invalid: true } },
        due: 0, outcome: "success", epoch: 2,
    };
    const operationUrl = `screens/workspace.html#${new URLSearchParams({ demo: JSON.stringify(operation) })}`;
    const safeOperation = screen(operationUrl);
    safeOperation.tick();
    assert.equal(safeOperation.K.demo.operation, undefined);
    assert.equal(safeOperation.K.current.id, "navigation");
    assert.deepEqual(safeOperation.errors, []);
});
test("execution worktree stays in a visible pending state until acknowledgment", () => {
    const s = screen("screens/workspace.html?contextSwitch=ready");
    const dialog = s.document.querySelector(".execution-context-dialog");
    const target = dialog.querySelector(".execution-context-options button:not(:disabled)");
    assert.equal(dialog.open, true);
    target.click();
    assert.equal(dialog.querySelector("[data-close]").disabled, true);
    assert.match(dialog.querySelector('[role="status"]').textContent, /Switching/);
    const cancel = new s.w.Event("cancel", { cancelable: true });
    dialog.dispatchEvent(cancel);
    assert.equal(cancel.defaultPrevented, true);
    assert.equal(s.K.current.executionTree, "develop");
    s.tick(350);
    assert.equal(dialog.open, false);
    assert.equal(s.K.current.executionTree, "feature/atlas-live");
    assert.equal(s.K.current.id, "navigation");
});
test("demo fragments cannot inject a conversation route into the sidebar", () => {
    const baseline = screen();
    const malicious = JSON.parse(JSON.stringify(baseline.K.demo));
    malicious.scopes["ketplus-desktop:develop"].conversations.navigation.page =
        'workspace"><img id="injected">';
    const address = `screens/workspace.html#${new URLSearchParams({ demo: JSON.stringify(malicious) })}`;
    const recovered = screen(address);
    assert.equal(recovered.K.current.page, "workspace");
    assert.equal(recovered.document.querySelector("#injected"), null);
    assert.deepEqual(recovered.errors, []);
});
test("prototype-named URL parameters fall back to declared providers and flows", () => {
    const workspace = screen("screens/workspace.html?provider=constructor&dialog=model&picker=toString&conversation=constructor");
    assert.equal(workspace.K.current.provider, "Codex");
    assert.equal(workspace.K.current.id, "navigation");
    assert.ok(workspace.document.querySelector(".model-context-dialog"));
    assert.equal(workspace.document.querySelector('[role="tab"][aria-selected="true"]').textContent.trim(), "Codex");
    assert.equal(workspace.document.querySelector(".model-options").textContent.includes("toString"), false);
    assert.deepEqual(workspace.errors, []);
    const builder = screen("screens/atlas-builder.html?flow=constructor&bundle=constructor");
    assert.equal(builder.K.activeAtlasFlow(), "getting-started");
    assert.equal(builder.K.query.get("project"), "Unavailable Atlas");
    assert.deepEqual(builder.errors, []);
});
test("global status bar is exclusive to Files & Editor", () => {
    const agent = screen("screens/workspace.html");
    const editor = screen("screens/editor.html");
    const sourceControl = screen("screens/changes.html");
    const atlas = screen("screens/atlas-library.html");
    assert.equal(agent.document.querySelector(".statusbar"), null);
    assert.ok(editor.document.querySelector(".statusbar"));
    assert.equal(sourceControl.document.querySelector(".statusbar"), null);
    assert.equal(atlas.document.querySelector(".statusbar"), null);
});
test("worktree isolation and explicit develop route", () => {
    const a = screen("screens/workspace.html?tree=develop&conversation=startup&state=running");
    a.K.go("workspace", { tree: "feature-onboarding" });
    const b = a.follow();
    assert.equal(b.K.activeConversation, "review");
    assert.notEqual(b.K.current.title, a.K.current.title);
    b.K.go("worktrees");
    const list = b.follow();
    const link = [...list.document.querySelectorAll(".worktree-name")].find((n) =>
        n.textContent.includes("develop"),
    );
    link.click();
    const back = list.follow();
    assert.equal(back.K.tree, "develop");
    assert.equal(back.K.activeConversation, "startup");
});
test("repository groups collapse without switching or stopping conversations", () => {
    const a = screen();
    assert.equal(a.document.querySelectorAll(".repository-group").length, 6);
    assert.equal(a.document.querySelectorAll(".remote-repository").length, 0);
    assert.equal(a.document.querySelector(".conversation-branch"), null);
    for (const row of a.document.querySelectorAll(".conversation")) {
        assert.equal(row.children.length, 2, "one status icon and one title per row");
        assert.equal(row.querySelectorAll(".conversation-title").length, 1);
        assert.ok(row.title.startsWith(row.querySelector(".conversation-title").textContent + "\n"));
        assert.ok(row.title.split("\n")[1], "worktree remains available in the tooltip");
    }
    const count = a.document.querySelectorAll(".conversation").length;
    a.click('[data-action="toggle-repository"][data-repo="ketplus-desktop"]');
    const collapsed = a.follow();
    assert.equal(collapsed.K.repo, "ketplus-desktop");
    assert.ok(collapsed.document.querySelectorAll(".conversation").length < count);
    assert.equal(collapsed.K.scope.conversations.startup.turn, "running");
    collapsed.click('[data-action="toggle-repository"][data-repo="ketplus-desktop"]');
    const restored = collapsed.follow();
    assert.equal(restored.document.querySelectorAll(".conversation").length, count);
    assert.equal(restored.document.querySelectorAll('.repository-groups [data-action="new-chat"][data-repo]').length, 6);
});
test("new conversation brings its repository to the top of the selected machine", () => {
    const a = screen("screens/workspace.html?repo=ketviet");
    const order = () => [...a.document.querySelectorAll(".repository-group-name[data-repo]")]
        .map(node => node.dataset.repo);
    assert.equal(order()[0], "ketplus-desktop");
    a.K.queue("new-conversation", { tree: "develop", base: "develop", branch: "",
        provider: "Codex", model: "GPT-5.6 Luna", effort: "Medium", mode: "workspace-write" });
    a.tick();
    assert.equal(a.K.advanceOperation(), true);
    a.K.mount();
    assert.equal(order()[0], "ketviet");
    assert.equal(a.document.querySelector("#machine-select").value, "local");
});
test("quota appears only in the context panel; identity menu routes to settings", () => {
    const a = screen();
    assert.equal(a.document.querySelector(".sidebar-usage"), null);
    assert.doesNotMatch(a.document.querySelector(".sidebar-inner").textContent, /Weekly usage|32%/);
    a.click('[data-action="user-menu"]');
    const menu = a.follow();
    assert.ok(menu.document.querySelector('.user-menu-item[href*="settings.html"]'));
    assert.ok(menu.document.querySelector('.user-menu-item[href*="connections.html"]'));
    const codex = screen("screens/workspace.html?dialog=usage");
    assert.match(codex.document.querySelector(".usage-popover").textContent, /Weekly usage32%/);
    const cursor = screen("screens/workspace.html?provider=Cursor&dialog=usage");
    assert.match(cursor.document.querySelector(".usage-popover").textContent, /Weekly usageUnavailable/);
    assert.doesNotMatch(cursor.document.querySelector(".usage-popover").textContent, /32%/);
});
function createConversation(a) {
    a.click('[data-action="new-chat"]');
    const dialog = a.follow();
    dialog.click('[data-action="confirm-new-conversation"]');
    const pending = dialog.follow();
    pending.tick();
    return pending.follow();
}
test("draft and attachment metadata survive model dialog, theme and conversation switching", () => {
    const a = screen();
    a.input("#prompt", "Keep this draft");
    a.K.current.attachment = { name: "sample.txt", size: 10 };
    a.click('[data-action="models"]');
    const model = a.follow();
    assert.equal(model.document.querySelector("#prompt").value, "Keep this draft");
    model.click('[data-action="close-dialog"]');
    const back = model.follow();
    back.K.go("workspace", { conversation: "startup" });
    const other = back.follow();
    assert.equal(other.document.querySelector("#prompt").value, "");
    other.K.go("workspace", { conversation: "navigation" });
    const restored = other.follow();
    assert.equal(restored.K.current.attachment.name, "sample.txt");
    assert.equal(restored.document.querySelector("#prompt").value, "Keep this draft");
});
test("new conversations receive independent identities", () => {
    const a = screen();
    const old = a.K.activeConversation;
    const b = createConversation(a);
    assert.notEqual(b.K.activeConversation, old);
    b.input("#prompt", "Draft B");
    const c = createConversation(b);
    assert.notEqual(c.K.activeConversation, b.K.activeConversation);
    assert.equal(c.document.querySelector("#prompt").value, "");
});
test("send acknowledges before clearing draft; steering does not start another turn", () => {
    const a = screen("screens/workspace.html?state=empty");
    a.input("#prompt", "First task");
    a.submit("#prompt-form");
    const pending = a.follow();
    assert.equal(pending.K.current.draft, "First task");
    pending.tick();
    const running = pending.follow();
    assert.equal(running.K.current.draft, "");
    assert.equal(running.K.current.requestCount, 1);
    running.input("#prompt", "Extra guidance");
    running.submit("#prompt-form");
    const steer = running.follow();
    steer.tick();
    const ack = steer.follow();
    assert.equal(ack.K.current.requestCount, 1);
    assert.equal(ack.K.current.turn, "running");
});
test("reconnect does not resend prompt and missing worktree retains Stop", () => {
    const a = screen("screens/workspace.html?state=error&turn=running");
    const count = a.K.current.requestCount;
    a.click('[data-action="retry"]');
    const reconnect = a.follow();
    reconnect.tick();
    const back = reconnect.follow();
    assert.equal(back.K.current.requestCount, count);
    assert.equal(back.K.current.turn, "running");
    assert.equal(back.K.current.connection, "connected");
    const missing = screen("screens/workspace.html?state=missing&turn=running");
    assert.ok(missing.document.querySelector('[data-action="stop"]'));
    assert.equal(missing.document.querySelector("#prompt").disabled, true);
});
test("question submission resumes working, not completed", () => {
    const a = screen("screens/workspace.html?state=question");
    a.submit("#question-form");
    const next = a.follow();
    next.input("#question-answer", "Keep it compact");
    next.submit("#question-form");
    const pending = next.follow();
    assert.equal(pending.K.current.turn, "answer-submitting");
    pending.tick();
    const ack = pending.follow();
    assert.equal(ack.K.current.turn, "running");
});
test("username login permits non-email and never serializes credentials", () => {
    const a = screen("screens/connections.html?state=signed-out");
    assert.equal(a.document.querySelector("#signin-form"), null);
    a.click('[data-action="open-signin"]');
    const form = a.follow();
    form.input("#account", "demo-user");
    form.input("#password", "do-not-persist-this");
    form.submit("#signin-form");
    assert.equal(form.url().includes("do-not-persist-this"), false);
    const pending = form.follow();
    pending.tick();
    const signedIn = pending.follow();
    assert.equal(signedIn.K.demo.auth, "signed-in");
});
test("signed-out state survives navigation; every paired device has revoke", () => {
    const a = screen("screens/connections.html?state=signed-out");
    a.K.go("workspace");
    const workspace = a.follow();
    workspace.K.go("connections");
    const back = workspace.follow();
    assert.equal(back.K.demo.auth, "signed-out");
    const paired = screen("screens/connections.html?state=paired");
    assert.equal(paired.document.querySelectorAll('[data-action="revoke-prompt"]').length, 2);
});
test("close pairing cancels ticket without granting device access", () => {
    const a = screen("screens/connections.html");
    a.click('[data-action="add-connection"]');
    const choice = a.follow();
    choice.click('[data-action="pair-device"]');
    const pair = choice.follow();
    pair.click('[data-action="simulate-scan"]');
    const confirm = pair.follow();
    confirm.click('[data-action="close-dialog"]');
    const back = confirm.follow();
    assert.equal(back.K.demo.ticket, "cancelled");
    assert.deepEqual([...back.K.demo.devices], ["iphone"]);
});
test("failed revoke retains paired device", () => {
    const a = screen(
        "screens/connections.html?state=paired&dialog=revoke&device=iphone&outcome=error",
    );
    a.click('[data-action="revoke-device"]');
    const pending = a.follow();
    assert.ok(pending.K.demo.devices.includes("iphone"));
    pending.tick();
    const failed = pending.follow();
    assert.ok(failed.K.demo.devices.includes("iphone"));
    assert.equal(failed.K.state, "revoke-error");
});
test("Atlas identity is preserved and scope copy does not guarantee isolation", () => {
    const a = screen("screens/atlas-library.html");
    const mobile = [...a.document.querySelectorAll(".atlas-card")].find((n) =>
        n.textContent.includes("KetPlus Mobile"),
    );
    mobile.click();
    const builder = a.follow();
    assert.equal(builder.K.bundle, "mobile");
    assert.match(
        builder.document.querySelector(".builder-conversation-title").textContent,
        /KetPlus Mobile/,
    );
    builder.K.go("atlas-preview");
    const preview = builder.follow();
    assert.equal(preview.K.bundle, "mobile");
    assert.match(
        preview.document.querySelector(".atlas-preview-bar").textContent,
        /KetPlus Mobile/,
    );
});
test("archive conversation is reversible from Settings without rerunning turn", () => {
    const a = screen();
    const id = a.K.activeConversation;
    rightClick(a, ".conversation.selected");
    a.click('[data-context-command="archive"]');
    const dialog = a.follow();
    dialog.click('[data-action="archive-confirm"]');
    const pending = dialog.follow();
    pending.tick();
    const settings = pending.follow();
    assert.equal(settings.K.page, "settings");
    assert.equal(settings.K.query.get("tab"), "archived");
    assert.equal(settings.K.scope.conversations[id].archived, true);
    settings.click('[data-action="archive-restore"][data-kind="conversation"]');
    const restore = settings.follow();
    restore.click('[data-action="archive-restore-confirm"]');
    const restoring = restore.follow();
    restoring.tick();
    const done = restoring.follow();
    assert.equal(done.K.scope.conversations[id].archived, false);
    assert.equal(done.K.scope.conversations[id].requestCount, 0);
    assert.equal(done.K.page, "settings");
});
test("delete guards active conversations and main/dirty/active worktrees", () => {
    const a = screen("screens/workspace.html?state=running");
    assert.match(
        a.K.deletionBlocker({ kind: "conversation", scope: a.K.scope, record: a.K.current }),
        /Stop/,
    );
    assert.match(
        a.K.deletionBlocker({ kind: "worktree", scope: a.K.scope, record: a.K.scope }),
        /main/,
    );
    const scope = a.K.ensureScope("feature-onboarding");
    assert.match(a.K.deletionBlocker({ kind: "worktree", scope, record: scope }), /Stop/);
    Object.values(scope.conversations).forEach((r) => (r.turn = "ready"));
    scope.dirty = true;
    assert.match(a.K.deletionBlocker({ kind: "worktree", scope, record: scope }), /uncommitted/);
});
test("history Latest resumes tail following while command details stay available", () => {
    const a = screen("screens/workspace.html?history=new-content");
    a.K.current.reading.follow = false;
    const latest = a.document.querySelector('[data-action="jump-latest"]');
    assert.equal(latest.getAttribute("aria-label"), "Jump to latest");
    assert.equal(latest.textContent.trim(), "");
    assert.equal(latest.closest(".transcript-viewport"), a.document.querySelector(".chat-scroll").parentElement);
    a.click('[data-action="jump-latest"]');
    assert.equal(a.document.querySelector(".latest-bar").hidden, true);
    assert.equal(a.K.current.reading.follow, true);
    assert.equal(a.document.querySelectorAll(".command-row").length, 2);
    assert.match(a.document.querySelector(".command-row").textContent, /command/);
    assert.equal(a.document.querySelectorAll(".command-detail[open]").length, 0);
    assert.match(a.document.querySelector(".command-output").textContent, /commandExecution/);
});
test("completed transcript separates exact activities from the final answer", () => {
    const a = screen("screens/workspace.html?state=ready&activity=files");
    assert.equal(a.document.querySelectorAll(".assistant-commentary").length, 0);
    assert.equal(a.document.body.textContent.includes("I will review"), false);
    assert.match(a.document.body.textContent, /Edited ActivityTimeline\.qml/);
    assert.match(a.document.body.textContent, /Ran command/);
    assert.ok([...a.document.querySelectorAll(".command-output")]
        .some(output => /cmake --build/.test(output.textContent)));
    assert.ok(a.document.querySelector('[data-transcript-layer="final"]'));
});
test("image activity opens all four reviewed images", () => {
    const a = screen("screens/workspace.html?state=ready&activity=images");
    assert.match(a.document.body.textContent, /Viewed 4 images/);
    assert.equal(a.document.querySelectorAll(".activity-thumb").length, 4);
    a.click(".activity-thumb");
    assert.ok(a.document.querySelector(".activity-preview img"));
    a.click('[data-action="close-activity-preview"]');
    assert.equal(a.document.querySelector(".activity-preview"), null);
});
test("browser open keeps its target in the Open action without a secondary URL", () => {
    const a = screen("screens/workspace.html?state=ready&activity=browser");
    const open = a.document.querySelector(".browser-open-action");
    assert.equal(open.textContent, "Open");
    assert.match(open.href, /doc\.qt\.io\/qt-6/);
    assert.doesNotMatch(a.document.querySelector("[data-browser-tool=browser_open]").textContent, /doc\.qt\.io/);
    assert.equal(a.document.querySelectorAll("[data-browser-tool]").length, 3);
    assert.match(a.document.body.textContent, /Used KetPlus editor and browser/);
});
test("unknown future activity remains visible through a generic fallback", () => {
    const a = screen("screens/workspace.html?state=ready&activity=unknown");
    assert.match(a.document.body.textContent, /newer tool activity/);
    assert.match(a.document.body.textContent, /provider\/customTool\.v2/);
    assert.match(a.document.body.textContent, /future-renderer/);
});
test("typed conversation deletion removes only the acknowledged target", () => {
    const a = screen("screens/workspace.html?dialog=delete&kind=conversation&target=navigation");
    a.click('[data-action="delete-confirm"]');
    assert.equal(a.K.demo.operation, undefined);
    a.input("#delete-confirmation", "DELETE");
    a.click('[data-action="delete-confirm"]');
    const pending = a.follow();
    assert.ok(pending.K.scope.conversations.navigation);
    pending.tick();
    const done = pending.follow();
    assert.equal(done.K.scope.conversations.navigation, undefined);
    assert.ok(done.K.scope.conversations.startup);
});
test("linked-worktree delete retains original history in Archived", () => {
    const a = screen(
        "screens/worktrees.html?fixture=clean&dialog=delete&kind=worktree&targetTree=fix-scroll-anchor",
    );
    a.input("#delete-confirmation", "DELETE");
    a.click('[data-action="delete-confirm"]');
    const pending = a.follow();
    pending.tick();
    const done = pending.follow();
    const scope = done.K.ensureScope("fix-scroll-anchor");
    assert.equal(scope.deleted, true);
    assert.equal(scope.archived, true);
    assert.equal(scope.conversations.checks.archived, true);
    assert.equal(scope.conversations.checks.available, false);
});
test("failed send retains draft and shows a visible error", () => {
    const a = screen("screens/workspace.html?state=empty&outcome=error");
    a.input("#prompt", "Retry this request");
    a.submit("#prompt-form");
    const pending = a.follow();
    pending.tick();
    const failed = pending.follow();
    assert.equal(failed.K.current.requestCount, 0);
    assert.equal(failed.K.current.draft, "Retry this request");
    assert.match(failed.document.body.textContent, /Request not sent/);
});
test("older page prepend retains stable ordering after navigation", () => {
    const a = screen();
    a.click('[data-action="load-older"]');
    a.tick();
    a.click('[data-action="load-older"]');
    a.tick();
    const before = [...a.document.querySelectorAll(".history-entry")].map(
        (n) => n.dataset.messageId,
    );
    a.click('[data-action="models"]');
    const after = a.follow();
    assert.deepEqual(
        [...after.document.querySelectorAll(".history-entry")].map((n) => n.dataset.messageId),
        before,
    );
});
test("new Atlas does not reuse an existing bundle conversation", () => {
    const a = screen("screens/atlas-library.html");
    a.click('[data-action="create-atlas"]');
    const draft = a.follow();
    assert.notEqual(draft.K.bundle, "onboarding");
    assert.equal(draft.K.current.bundleReady, false);
    assert.ok(draft.K.scope.conversations.atlas);
    assert.notEqual(draft.K.current.id, "atlas");
});
test("pending acknowledgment stays attached to its original conversation", () => {
    const a = screen("screens/workspace.html?state=empty");
    const id = a.K.current.id;
    a.input("#prompt", "Task A");
    a.submit("#prompt-form");
    const pending = a.follow();
    pending.K.go("workspace", { tree: "feature-onboarding" });
    const other = pending.follow();
    other.input("#prompt", "Draft B");
    other.tick();
    const refreshed = other.follow();
    assert.equal(refreshed.K.current.draft, "Draft B");
    assert.equal(refreshed.K.current.requestCount, 0);
    assert.equal(refreshed.K.ensureScope("develop").conversations[id].requestCount, 1);
});
test("private answers never enter URL snapshots and expired requests cannot submit", () => {
    const a = screen("screens/workspace.html?state=question&question=2&secret=1");
    a.input("#question-answer", "never-save-this-private-value");
    a.submit("#question-form");
    assert.equal(a.url().includes("never-save-this-private-value"), false);
    assert.equal(JSON.stringify(a.K.demo).includes("never-save-this-private-value"), false);
    const expired = screen("screens/workspace.html?state=question&request=expired");
    assert.equal(expired.document.querySelector("#question-form"), null);
    assert.match(expired.document.body.textContent, /Question expired/);
});
test("completed Atlas turn does not make an unvalidated bundle previewable", () => {
    const a = screen("screens/atlas-library.html");
    a.click('[data-action="create-atlas"]');
    const draft = a.follow();
    draft.K.current.turn = "ready";
    draft.K.go("atlas-preview");
    const preview = draft.follow();
    assert.match(preview.document.body.textContent, /Preview pending validation/);
});
test("new actions use real Lucide assets rather than circle fallbacks", () => {
    const a = screen();
    for (const name of ["archive", "archive-restore", "log-in", "info", "arrow-down", "unplug"])
        assert.notEqual(a.K.icon(name), a.K.icon("circle"));
});

test("context ring follows the model on the lower toolbar, separate from send", () => {
    const a = screen();
    const ring = a.document.querySelector('[data-action="usage"].usage-ring');
    assert.ok(ring.closest(".composer-toolbar"));
    assert.equal(ring.parentElement.previousElementSibling.dataset.action, "models");
    assert.equal(a.document.querySelector(".composer-input .usage-ring"), null);
    a.input("#prompt", "Do not lose this draft");
    ring.click();
    assert.equal(a.url(), undefined, "Context opens in place without navigation");
    assert.equal(a.document.querySelector(".modal-backdrop"), null);
    assert.ok(a.document.querySelector(".usage-anchor .usage-popover"));
    assert.equal(a.document.querySelector("#prompt").value, "Do not lose this draft");
    assert.match(a.document.querySelector(".usage-popover").textContent, /Context & limits/);
    a.click('[data-action="close-usage"]');
    assert.equal(a.document.querySelector(".usage-popover"), null);
    assert.equal(a.document.activeElement, ring);
    const url = a.K.url("workspace");
    assert.ok(url.split("#")[0].length < 12000, "Preview state must not overflow the HTTP request or Referer");
    assert.equal(new URL(url, "https://atlas.invalid/").searchParams.has("demo"), false);
    assert.ok(url.includes("#demo="));
});
test("provider changes retain draft and restore separately per conversation", () => {
    const a = screen();
    a.input("#prompt", "Keep this draft");
    a.click('[data-action="models"]');
    const picker = a.follow();
    picker.click('[data-provider="Claude"]');
    const claude = picker.follow();
    claude.click('[data-action="apply-provider"]');
    const applied = claude.follow();
    assert.equal(applied.K.current.provider, "Claude");
    assert.equal(applied.K.current.model, "Sonnet");
    assert.equal(applied.document.querySelector("#prompt").value, "Keep this draft");
    applied.K.go("workspace", {conversation: "startup"});
    const other = applied.follow();
    assert.equal(other.K.current.provider, "Codex");
    other.K.go("workspace", {conversation: "navigation"});
    assert.equal(other.follow().K.current.provider, "Claude");
});
test("Cursor has Stop but no steering, effort or other provider quota", () => {
    const a = screen("screens/workspace.html?provider=Cursor&state=running");
    assert.equal(a.document.querySelector("#prompt").disabled, true);
    assert.ok(a.document.querySelector('[data-action="stop"]'));
    assert.equal(a.document.querySelector('[data-action="send-guidance"]'), null);
    assert.equal(a.document.querySelector(".composer-model .effort"), null);
    a.K.current.draft = "Do not steer";
    a.K.submitPrompt();
    assert.equal(a.K.demo.operation, undefined);
});
test("active Claude has text guidance and locks model and attachments", () => {
    const a = screen("screens/workspace.html?provider=Claude&state=running");
    const send = a.document.querySelector('#prompt-form button[type="submit"]');
    assert.equal(send.hidden, false);
    assert.equal(send.disabled, true);
    assert.equal(a.document.querySelector(".simulation-actions"), null);
    assert.equal(a.document.querySelector(".steering-action"), null);
    assert.equal(a.document.querySelector('[data-action="attach"]').disabled, true);
    assert.equal(a.document.querySelector('[data-action="models"]').disabled, true);
    a.input("#prompt", "Keep the current direction");
    assert.equal(send.hidden, false);
    assert.equal(send.disabled, false);
    assert.equal(send.getAttribute("aria-label"), "Send guidance");
    assert.ok(a.document.querySelector('#prompt-form [data-action="stop"]'));
    a.click('#prompt-form button[type="submit"]');
    const pending = a.follow();
    assert.equal(pending.K.demo.operation.kind, "steer");
    pending.tick();
    assert.equal(pending.follow().K.current.requestCount, 0);
});
test("unknown context and unsupported providers cannot compact", () => {
    for (const query of ["usage=unknown", "provider=Claude", "provider=Cursor", "state=running"]) {
        const a = screen(`screens/workspace.html?dialog=usage&${query}`);
        assert.equal(a.document.querySelector('[data-action="compact-session"]').disabled, true);
    }
});
test("new conversation validates branches and selects the acknowledged worktree", () => {
    const a = screen("screens/workspace.html?dialog=new-conversation&provider=Claude");
    a.input("#new-branch", "bad branch");
    a.click('[data-action="confirm-new-conversation"]');
    assert.match(a.document.querySelector("#new-conversation-error").textContent, /valid branch/);
    assert.equal(a.K.demo.operation, undefined);
    a.input("#new-branch", "feature/desktop-design");
    a.click('[data-action="confirm-new-conversation"]');
    const pending = a.follow();
    assert.equal(pending.K.current.id, "navigation");
    pending.tick();
    const created = pending.follow();
    assert.equal(created.K.tree, "feature-desktop-design");
    assert.equal(created.K.current.provider, "Claude");
    assert.equal(created.K.current.turn, "empty");
    assert.equal(created.K.current.requestCount, 0);
    assert.ok(created.document.querySelector(`[data-conversation="${created.K.current.id}"]`));
});
test("fork copies context after acknowledgment without owning files or starting a turn", () => {
    const a = screen("screens/workspace.html?dialog=fork&provider=Claude");
    const original = a.K.activeConversation;
    a.click('[data-action="confirm-fork"]');
    const pending = a.follow();
    assert.equal(pending.K.activeConversation, original);
    pending.tick();
    const fork = pending.follow();
    assert.notEqual(fork.K.activeConversation, original);
    assert.equal(fork.K.current.forkedFrom, original);
    assert.equal(fork.K.current.ownsWorktree, false);
    assert.equal(fork.K.current.requestCount, 0);
    assert.equal(fork.K.current.provider, "Claude");
    assert.match(fork.K.current.title, /\(fork\)$/);
    assert.ok(fork.K.scope.conversations[original]);
});
test("fork failure preserves the original and unsupported or busy forks are blocked", () => {
    const a = screen("screens/workspace.html?dialog=fork&outcome=error");
    const count = Object.keys(a.K.scope.conversations).length;
    a.click('[data-action="confirm-fork"]');
    const pending = a.follow();
    pending.tick();
    const failed = pending.follow();
    assert.equal(Object.keys(failed.K.scope.conversations).length, count);
    assert.match(failed.document.querySelector(".modal").textContent, /Fork failed/);
    for (const query of ["provider=Cursor", "state=running", "state=empty"]) {
        const blocked = screen(`screens/workspace.html?dialog=fork&${query}`);
        assert.equal(blocked.document.querySelector('[data-action="confirm-fork"]').disabled, true);
    }
});
test("background fork acknowledgment never changes the selected conversation", () => {
    const a = screen("screens/workspace.html?dialog=fork");
    a.click('[data-action="confirm-fork"]');
    const pending = a.follow();
    pending.K.go("workspace", {conversation: "startup", dialog: null});
    const other = pending.follow();
    other.tick();
    const refreshed = other.follow();
    assert.equal(refreshed.K.activeConversation, "startup");
    assert.ok(Object.values(refreshed.K.scope.conversations).some(c => c.forkedFrom === "navigation"));
});
test("settings save applies conversation width and browser preference", () => {
    const a = screen("screens/settings.html?tab=typography");
    a.input("#conversation-width", "wide");
    a.click('[data-action="save-appearance"]');
    const workspace = a.follow();
    assert.equal(workspace.w.document.body.style.getPropertyValue("--kp-conversation-width"), "864px");
    const settings = screen("screens/settings.html?tab=providers");
    settings.document.querySelector("#browser-enabled").checked = false;
    settings.click('[data-action="save-provider-settings"]');
    assert.equal(settings.K.demo.browserEnabled, false);
});

test("Atlas sidebar changes the map while preserving the draft and tagged context", () => {
    const a = screen("screens/atlas-builder.html");
    a.input("#prompt", "Keep this draft while exploring flows");
    const scope = a.K.atlasScope();
    assert.equal(a.document.querySelector("#atlas-flow-select"), null);
    a.click('.atlas-flow-sidebar [data-flow="workspace-recovery"]');
    assert.equal(a.K.activeAtlasFlow(), "workspace-recovery");
    assert.match(a.document.querySelector(".atlas-flow-label h2").textContent, /Reconnect/);
    assert.equal(a.document.querySelector('.atlas-flow-item[aria-current="true"]').dataset.flow, "workspace-recovery");
    assert.equal(a.document.querySelector("#prompt").value, "Keep this draft while exploring flows");
    assert.equal(a.K.atlasScope(), scope);
    a.click('.atlas-map-node a[aria-label="Preview Project unavailable"]');
    const preview = a.follow();
    assert.equal(preview.K.query.get("step"), "missing");
    assert.equal(preview.K.query.get("flow"), "workspace-recovery");
});

function rightClick(a, selector) {
    const target = a.document.querySelector(selector);
    assert.ok(target, selector);
    target.dispatchEvent(new a.w.MouseEvent("contextmenu", { bubbles: true, cancelable: true, clientX: 350, clientY: 240 }));
    return a.document.querySelector('[role="menu"].desktop-context-menu');
}
const menuCommands = a => [...a.document.querySelectorAll('[data-context-command]')].map(n => n.dataset.contextCommand);
test("right-click preserves active conversation and archives the clicked repository target", () => {
    const a = screen();
    a.input("#prompt", "Keep my active draft");
    const current = a.K.current;
    rightClick(a, '.conversation[href*="repo=ketviet"]');
    assert.equal(a.K.current, current);
    assert.deepEqual(menuCommands(a), ["tags", "copy-agent-reference", "fork", "archive", "delete"]);
    const target = a.K.openContext;
    a.click('[data-context-command="archive"]');
    const confirm = a.follow();
    assert.equal(confirm.K.resourceTarget().scope.repo, "ketviet");
    assert.equal(confirm.K.resourceTarget().id, target.id);
    assert.equal(confirm.K.current.draft, "Keep my active draft");
    confirm.click('[data-action="archive-confirm"]');
    const pending = confirm.follow(); pending.tick();
    const done = pending.follow();
    assert.equal(done.K.demo.scopes['ketviet:develop'].conversations[target.id].archived, true);
    assert.equal(done.K.current.archived, false);
});
test("transcript offers only fork and changed-file menu takes precedence", () => {
    const a = screen();
    rightClick(a, '.chat-scroll');
    assert.deepEqual(menuCommands(a), ["fork"]);
    rightClick(a, '.changed-file-row');
    assert.deepEqual(menuCommands(a), ["copy-path", "reveal", "open-file"]);
    a.click('[data-context-command="copy-path"]');
    assert.match(a.K.demo.menuClipboard, /\/modules\/ai\/src\/agent\/qml\/ActivityTimeline.qml$/);
    assert.equal(a.document.querySelector('[role="dialog"]'), null);
});
test("context menus enforce busy, provider, missing-file and worktree guards", () => {
    for (const query of ['state=running', 'provider=Cursor']) {
        const a = screen(`screens/workspace.html?context=conversation&${query}`);
        assert.equal(a.document.querySelector('[data-context-command="fork"]').disabled, true);
        assert.equal(a.document.querySelector('[data-context-command="archive"]').disabled, false);
    }
    const running = screen('screens/workspace.html?context=conversation&state=running');
    assert.equal(running.document.querySelector('[data-context-command="delete"]').disabled, true);
    const missing = screen('screens/workspace.html?context=changed-file&contextFixture=missing');
    assert.equal(missing.document.querySelector('[data-context-command="copy-path"]').disabled, false);
    assert.equal(missing.document.querySelector('[data-context-command="open-file"]').disabled, true);
    for (const fixture of ['locked', 'unsaved']) {
        const a = screen(`screens/worktrees.html?context=worktree&fixture=clean&targetTree=fix-scroll-anchor&contextFixture=${fixture}`);
        assert.equal(a.document.querySelector('[data-context-command="delete"]').disabled, true);
    }
    const main = screen('screens/worktrees.html?context=worktree');
    assert.equal(main.document.querySelector('[data-context-command="archive"]').disabled, true);
});
test("context menu keyboard navigation skips disabled actions and restores focus", () => {
    const a = screen('screens/workspace.html?provider=Cursor');
    const node = a.document.querySelector('.conversation.selected'); node.focus();
    node.dispatchEvent(new a.w.KeyboardEvent('keydown', {key:'F10', shiftKey:true, bubbles:true}));
    assert.equal(a.document.activeElement.dataset.contextCommand, 'tags');
    a.document.activeElement.dispatchEvent(new a.w.KeyboardEvent('keydown', {key:'ArrowDown', bubbles:true}));
    assert.equal(a.document.activeElement.dataset.contextCommand, 'copy-agent-reference');
    a.document.activeElement.dispatchEvent(new a.w.KeyboardEvent('keydown', {key:'ArrowDown', bubbles:true}));
    assert.equal(a.document.activeElement.dataset.contextCommand, 'archive', 'disabled Fork is skipped');
    a.w.dispatchEvent(new a.w.Event('resize'));
    assert.ok(a.document.querySelector('.desktop-context-menu'), 'Viewer resizing must retain the menu fixture');
    a.document.activeElement.dispatchEvent(new a.w.KeyboardEvent('keydown', {key:'ArrowDown', bubbles:true}));
    assert.equal(a.document.activeElement.dataset.contextCommand, 'delete');
    a.document.activeElement.dispatchEvent(new a.w.KeyboardEvent('keydown', {key:'Escape', bubbles:true}));
    assert.equal(a.document.querySelector('.desktop-context-menu'), null);
    assert.equal(a.document.activeElement, node);
});
test("Explorer root omits destructive actions; create validates and preserves failed input", () => {
    const root = screen('screens/editor.html?context=explorer-root');
    assert.equal(menuCommands(root).includes('trash'), false);
    assert.equal(menuCommands(root).includes('rename-file'), false);
    const a = screen('screens/editor.html?context=explorer-file&outcome=error');
    a.click('[data-context-command="new-file"]');
    a.input('#context-name', '../outside'); a.click('[data-action="context-confirm"]');
    assert.match(a.document.querySelector('.context-command-dialog').textContent, /without slashes/);
    a.input('#context-name', 'new-notes.md'); a.click('[data-action="context-confirm"]');
    assert.match(a.document.querySelector('.context-command-dialog').textContent, /Nothing changed/);
    assert.equal(a.document.querySelector('#context-name').value, 'new-notes.md');
    a.K.query.delete('outcome'); a.click('[data-action="context-confirm"]');
    assert.equal(a.document.querySelector('.context-command-dialog'), null);
    assert.ok([...a.document.querySelectorAll('.file-row > span')].some(n => n.textContent === 'new-notes.md'));
});
test("staged Git menu retains diff identity and offers no mutation commands", () => {
    const a = screen('screens/changes.html?context=git-file&group=staged');
    assert.deepEqual(menuCommands(a), ['view-diff','open-file','copy-path']);
    a.click('[data-context-command="view-diff"]');
    const b = a.follow();
    assert.equal(b.K.query.get('group'), 'staged');
    assert.equal(b.K.query.get('file'), 'ThemeManager.cpp');
});
test("Markdown tab and detail tab expose their own menus and unsaved close can cancel", () => {
    const a = screen('screens/editor.html?context=editor-tab&file=design-system.md&preview=markdown');
    assert.equal(a.document.querySelector('[data-context-command="markdown-preview"]').getAttribute('aria-checked'), 'true');
    const detail = screen('screens/workspace.html?panel=file&context=detail-tab');
    assert.deepEqual(menuCommands(detail), ['close-tab','close-others','close-all']);
    assert.equal(detail.document.querySelector('[data-context-command="close-others"]').disabled, true);
    const dirty = screen('screens/editor.html?context=editor-tab&contextFixture=unsaved');
    dirty.click('[data-context-command="close-tab"]');
    assert.match(dirty.document.querySelector('[role="dialog"]').textContent, /Unsaved changes/);
    dirty.click('[data-action="context-dismiss"]');
    assert.equal(dirty.document.querySelectorAll('.file-tabs > a').length, 2);
});
test("editor selection, demo clipboard and undo round trip are interactive", () => {
    const a = screen('screens/editor.html?context=editor&contextFixture=selection');
    const before = a.document.querySelector('.source-code').innerHTML;
    a.click('[data-context-command="cut"]');
    assert.ok(a.K.demo.menuClipboard);
    rightClick(a, '.source-code');
    assert.equal(a.document.querySelector('[data-context-command="undo"]').disabled, false);
    a.click('[data-context-command="undo"]');
    assert.equal(a.document.querySelector('.source-code').innerHTML, before);
});

function viewerTheme(screen, theme, source = screen.w.parent, type = "ketatlas:theme") {
    screen.w.dispatchEvent(new screen.w.MessageEvent("message", { source, data: { type, theme } }));
}

for (const entry of manifest.screens) {
    test(`viewer theme is temporary and preserves DOM state: ${entry.id}`, () => {
        const s = screen(`${entry.url}?theme=dark`, { embedded: true });
        assert.deepEqual(JSON.parse(JSON.stringify(s.parentMessages)), [
            { message: { type: "ketatlas:theme-ready" }, origin: "*" },
        ]);
        const app = s.document.querySelector("#app");
        const content = app.innerHTML;
        const url = s.w.location.href;
        for (const theme of ["light", "dark", "light", null, null]) {
            viewerTheme(s, theme);
            assert.equal(s.document.documentElement.dataset.theme, theme ?? "dark");
            assert.equal(s.document.documentElement.style.colorScheme, theme ?? "dark");
            assert.equal(s.document.querySelector("#app"), app, "do not remount");
            assert.equal(app.innerHTML, content);
            assert.equal(s.w.location.href, url);
            assert.equal(s.K.query.get("theme"), "dark", "do not persist viewer overrides");
        }
        assert.deepEqual(s.errors, []);
    });
}

test("theme bridge rejects unrelated senders, message types and invalid themes", () => {
    const s = screen("screens/workspace.html", { embedded: true });
    viewerTheme(s, "dark", s.w);
    viewerTheme(s, "dark", null);
    viewerTheme(s, "dark", s.w.parent, "unrelated:theme");
    for (const value of [undefined, "system", "", 0, {}, ["dark"]]) viewerTheme(s, value);
    assert.equal(s.document.documentElement.dataset.theme, "light");
    viewerTheme(s, "dark");
    assert.equal(s.document.documentElement.dataset.theme, "dark");
    const standalone = screen();
    viewerTheme(standalone, "dark");
    assert.equal(standalone.document.documentElement.dataset.theme, "light");
    assert.deepEqual(standalone.parentMessages, []);
});

test("draft, attachment and navigation survive viewer theme changes", () => {
    const s = screen("screens/workspace.html?theme=dark", { embedded: true });
    s.input("#prompt", "Keep this draft through theme changes");
    s.K.current.attachment = { name: "example.txt", size: 10 };
    const input = s.document.querySelector("#prompt");
    const state = JSON.stringify(s.K.demo);
    for (const theme of ["light", "dark", "light", null]) viewerTheme(s, theme);
    assert.equal(s.document.querySelector("#prompt"), input);
    assert.equal(input.value, "Keep this draft through theme changes");
    assert.equal(JSON.stringify(s.K.demo), state);
    viewerTheme(s, "light");
    s.click('[data-action="models"]');
    const next = s.follow();
    viewerTheme(next, "light");
    assert.ok(next.document.querySelector('[role="dialog"]'));
    assert.equal(next.document.querySelector("#prompt").value, input.value);
    assert.equal(next.K.current.attachment.name, "example.txt");
    viewerTheme(next, null);
    assert.equal(next.document.documentElement.dataset.theme, "dark");
});

test("sync off restores the live system preference and save never stores the override", () => {
    const s = screen("screens/settings.html?theme=light&system=true", { embedded: true, systemDark: true });
    assert.equal(s.document.documentElement.dataset.theme, "dark");
    viewerTheme(s, "light");
    s.systemTheme(false);
    s.systemTheme(true);
    assert.equal(s.document.documentElement.dataset.theme, "light");
    viewerTheme(s, null);
    assert.equal(s.document.documentElement.dataset.theme, "dark");
    s.systemTheme(false);
    assert.equal(s.document.documentElement.dataset.theme, "light");
    viewerTheme(s, "dark");
    s.click('[data-action="save-appearance"]');
    const next = s.follow();
    assert.equal(next.K.query.get("theme"), "light");
    assert.equal(next.K.query.get("system"), "true");
    const explicit = screen("screens/settings.html?theme=dark", { embedded: true });
    viewerTheme(explicit, "light");
    explicit.click('[data-action="save-appearance"]');
    assert.equal(explicit.follow().K.query.get("theme"), "dark");
});

test("Follow system changes and explicit theme choices preserve product preferences under sync", () => {
    const s = screen("screens/settings.html?theme=dark&system=true", { embedded: true });
    viewerTheme(s, "light");
    s.click("#follow-system");
    assert.equal(s.document.documentElement.dataset.theme, "light", "viewer still controls appearance");
    viewerTheme(s, null);
    assert.equal(s.document.documentElement.dataset.theme, "dark", "restore the chosen product theme");
    s.click('[data-action="save-appearance"]');
    assert.equal(s.follow().K.query.get("system"), "false");
    const choice = new URL(s.document.querySelector('[aria-label="Light theme"]').href);
    assert.equal(choice.searchParams.get("theme"), "light");
    assert.equal(choice.searchParams.get("system"), "false");
});

test("update lifecycle advances only after actions and restart keeps running turns", () => {
    const current = screen("screens/settings.html?tab=updates");
    assert.match(current.document.querySelector(".settings-nav .active").textContent, /Updates/);
    current.tick(5000);
    assert.equal(current.url(), undefined, "a fixture must not advance by itself");
    current.click('[data-action="check-updates"]');
    const checking = current.follow();
    assert.ok(checking.document.querySelector('[data-action="check-updates"]').disabled);
    checking.tick(1500);
    const available = checking.follow();
    assert.equal(available.K.update.state(), "available");
    available.click('[data-action="install-update"]');
    const downloading = available.follow();
    downloading.tick(1500);
    const ready = downloading.follow();
    assert.match(ready.document.querySelector(".update-card").textContent, /keep running in the background service/);
    ready.click('[data-action="later-update"]');
    const later = ready.follow();
    assert.ok(later.document.querySelector(".update-pill"), "deferred update stays visible in the sidebar");
    const resumed = screen("screens/settings.html?tab=updates&update=ready");
    resumed.click('[data-action="restart-update"]');
    assert.match(resumed.url(), /saved=updated/);
    assert.doesNotMatch(resumed.url(), /update=ready/);
});

test("update feed failures and unconfigured builds never offer an install", () => {
    const failed = screen("screens/settings.html?tab=updates&update=checking&pending=1&result=error");
    failed.tick(1500);
    const error = failed.follow();
    assert.ok(error.document.querySelector(".banner.error"));
    assert.equal(error.document.querySelector('[data-action="install-update"]'), null);
    const disabled = screen("screens/settings.html?tab=updates&update=disabled");
    assert.ok(disabled.document.querySelector('[data-action="check-updates"]').disabled);
    assert.ok(disabled.document.querySelector("#update-auto-check").disabled);
    const skipped = screen("screens/settings.html?tab=updates&update=available");
    skipped.click('[data-action="skip-update"]');
    assert.match(skipped.url(), /saved=skipped/);
});


test("compact picker keeps the 1M model capacity while applying 272k per conversation", () => {
    const a = screen("screens/workspace.html?dialog=model&provider=Claude&model=Opus%205.5&compactWindow=272000");
    assert.match(a.document.querySelector("#model-capacity").textContent, /1M/);
    assert.equal(a.document.querySelector('input[name="compact-window"]:checked').value, "272000");
    a.click('[data-action="apply-provider"]');
    const applied = a.follow();
    assert.equal(applied.K.current.compactWindow, 272000);
    assert.equal(applied.K.current.model, "Opus 5.5");
    assert.equal(applied.document.querySelector(".model-context-dialog"), null);
    assert.equal(applied.K.current.provider, "Claude");
});
test("compact custom validation rejects malformed, fractional and out-of-range values", () => {
    const a = screen("screens/workspace.html?dialog=model&provider=Claude&model=Opus%205.5&compactWindow=272000");
    a.click('input[name="compact-window"][value="custom"]');
    for (const value of ["", "99000", "1000001", "272000.5", "2.72e5", "-272000", "272k"]) {
        a.input("#compact-tokens", value);
        assert.equal(a.document.querySelector('[data-action="apply-provider"]').disabled, true, value);
        assert.equal(a.document.querySelector("#compact-error").hidden, false, value);
    }
    for (const value of ["100000", "272000", "500000", "1000000"]) {
        a.input("#compact-tokens", value);
        assert.equal(a.document.querySelector('[data-action="apply-provider"]').disabled, false, value);
    }
    a.click('[data-action="apply-provider"]');
    assert.equal(a.follow().K.current.compactWindow, 1000000);
});
test("cancel leaves the saved compact window intact, Auto resets it", () => {
    const a = screen("screens/workspace.html?dialog=model&provider=Claude&compactWindow=272000");
    a.click('input[name="compact-window"][value="500000"]');
    a.click('[data-action="close-dialog"]');
    const cancelled = a.follow();
    assert.equal(cancelled.K.current.compactWindow, 272000);
    cancelled.K.go("workspace", {dialog: "model"});
    const opened = cancelled.follow();
    opened.click('input[name="compact-window"][value="auto"]');
    opened.click('[data-action="apply-provider"]');
    assert.equal(opened.follow().K.current.compactWindow, null);
});
test("busy, background, disconnected, read-only and pending compact settings cannot apply", () => {
    for (const state of ["running", "background", "offline", "denied", "loading", "saving"]) {
        const a = screen(`screens/workspace.html?dialog=model&provider=Claude&compactWindow=272000&compactState=${state}`);
        assert.equal(a.document.querySelector(".compact-presets").disabled, true, state);
        assert.equal(a.document.querySelector('[data-action="apply-provider"]').disabled, true, state);
        assert.ok(a.document.querySelector(".compact-notice"), state);
        assert.equal(a.K.current.compactWindow, 272000);
    }
});
test("unsupported Cursor exposes Auto and clears an inherited explicit override on apply", () => {
    const a = screen("screens/workspace.html?dialog=model&provider=Cursor&compactWindow=272000");
    assert.equal(a.document.querySelector(".compact-presets").disabled, true);
    assert.equal(a.document.querySelector('input[name="compact-window"]:checked').value, "auto");
    a.click('[data-action="apply-provider"]');
    assert.equal(a.follow().K.current.compactWindow, null);
});
test("retry retains the compact selection and clears the simulated apply error", () => {
    const a = screen("screens/workspace.html?dialog=model&provider=Claude&compactWindow=500000&compactState=error");
    assert.equal(a.document.querySelector(".compact-feedback").hidden, false);
    assert.equal(a.document.querySelector('[data-action="apply-provider"]').textContent.trim(), "Retry");
    a.click('[data-action="apply-provider"]');
    const retried = a.follow();
    assert.equal(retried.K.current.compactWindow, 500000);
    assert.equal(retried.K.query.has("compactState"), false);
});
test("no-result model search keeps the compact draft and recovers when cleared", () => {
    const a = screen("screens/workspace.html?dialog=model&provider=Claude&compactWindow=272000");
    a.input("#model-search", "unknown model name");
    assert.equal(a.document.querySelector("#model-search-empty").hidden, false);
    assert.equal(a.K.compactDraft, "272000");
    a.input("#model-search", "");
    assert.equal(a.document.querySelector("#model-search-empty").hidden, true);
});
test("connections artwork shows real desktop and KetPlus Mobile captures for each theme", () => {
    const s = screen("screens/connections.html?state=signed-out");
    for (const [frame, size] of [[".connection-desktop", [1280, 760]], [".connection-phone", [786, 1704]]]) {
        const shots = [...s.document.querySelectorAll(`${frame} > img.connection-screen`)];
        assert.deepEqual(shots.map((img) => img.dataset.themeVariant), ["light", "dark"]);
        for (const img of shots) {
            const png = fs.readFileSync(path.join(root, "screens", img.getAttribute("src")));
            assert.equal(png.toString("latin1", 1, 4), "PNG");
            assert.deepEqual([png.readUInt32BE(16), png.readUInt32BE(20)], size);
        }
    }
    assert.equal(s.document.querySelector(".connection-mini-chat, .connection-phone-question"), null);
});
test("mono text never renders below 13px", () => {
    const screens = path.join(root, "screens");
    const atLeast13 = size => /^max\(13px,/.test(size) || /^round\(max\(13px,/.test(size) ||
        (/^\d+px$/.test(size) && parseInt(size, 10) >= 13) || /^var\(--kp-(editor|terminal)-size/.test(size);
    for (const file of fs.readdirSync(screens).filter((name) => name.endsWith(".css"))) {
        const css = fs.readFileSync(path.join(screens, file), "utf8").replace(/\/\*[\s\S]*?\*\//g, "");
        for (const [, selector, body] of css.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
            if (!/font(-family)?\s*:[^;]*mono/i.test(body)) continue;
            const shorthand = /font\s*:\s*(?:(?:italic|normal|bold|[1-9]00)\s+)*([^\s/;]+)/.exec(body)?.[1];
            const size = /font-size\s*:\s*([^;]+)/.exec(body)?.[1].trim() || shorthand;
            assert.ok(size && atLeast13(size), `${file}: ${selector.trim()} → ${size}`);
        }
    }
    assert.match(fs.readFileSync(path.join(screens, "typography.css"), "utf8"), /\[data-ui="code-block"\][\s\S]*max\(13px, var\(--kv-text-xs\)\)/);
});
test("font sizes are whole pixels, including configuration defaults", () => {
    const screens = path.join(root, "screens");
    for (const file of fs.readdirSync(screens).filter((name) => name.endsWith(".css"))) {
        const css = fs.readFileSync(path.join(screens, file), "utf8");
        for (const [declaration] of css.matchAll(/font-size:[^;}]+|--kp-(?:ui|editor|terminal|markdown)-size:[^;}]+/g)) {
            assert.doesNotMatch(declaration, /\d\.\d/, `${file}: ${declaration}`);
            // A size derived from the scale must round to a whole pixel.
            if (/--kp-font-scale|vw/.test(declaration)) assert.match(declaration, /round\(/, `${file}: ${declaration}`);
        }
        for (const [token] of css.matchAll(/--kp-text-[a-z]+:[^;}]+/g))
            if (/--kp-font-scale/.test(token)) assert.match(token, /round\(/, `${file}: ${token}`);
    }
    const core = fs.readFileSync(path.join(screens, "core.js"), "utf8");
    for (const [, size] of core.matchAll(/\["\w+Size", "\w+", ([\d.]+)\]/g)) assert.ok(Number.isInteger(Number(size)), size);
    const settings = fs.readFileSync(path.join(screens, "settings.js"), "utf8");
    for (const [, size] of settings.matchAll(/K\.fontSizes\("\w+", ([\d.]+)\)/g)) assert.ok(Number.isInteger(Number(size)), size);
});
