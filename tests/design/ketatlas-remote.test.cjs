// Focused synthetic Linux pairing checks; no network, daemon or provider calls.
const test = require("node:test");
const assert = require("node:assert/strict");
const { screen, manifest } = require("./ketatlas-dom.cjs");
const flow = manifest.flows.find((item) => item.id === "remote-linux-journey");
const remoteUrls = new Set([manifest.screens.find((item) => item.id === "connections").url,
    ...flow.nodes.map((node) => node.url)]);
for (const url of remoteUrls) test(`render addressable state: ${url}`, () => {
    const s = screen(url);
    assert.ok(s.document.querySelector("#app").textContent.trim());
    assert.deepEqual(s.errors.map((error) => error.message), []);
    s.close();
});
test("machine select filters repositories while Connections stays one navigation entry", () => {
    const workspace = screen();
    const nav = workspace.document.querySelector('nav[aria-label="Main navigation"]');
    assert.equal(nav.textContent.includes("Remote machines"), false);
    assert.ok(nav.textContent.includes("Connections"));
    const picker = workspace.document.querySelector("#machine-select");
    assert.equal(picker.value, "local");
    assert.match(picker.textContent, /This Mac/);
    assert.match(picker.textContent, /Linux: build-node-01/);
    assert.equal(workspace.document.querySelectorAll(".repository-group").length, 6);
    picker.value = "linux-build-01";
    picker.dispatchEvent(new workspace.w.Event("change", { bubbles: true }));
    const remote = workspace.follow();
    assert.equal(remote.K.repo, "api-service");
    assert.equal(remote.document.querySelector("#machine-select").value, "linux-build-01");
    assert.equal(remote.document.querySelectorAll(".repository-group").length, 1);
    assert.match(remote.document.querySelector(".remote-repository").textContent, /api-service/);
    assert.equal(remote.document.querySelectorAll(".remote-repository .conversation").length, 1);
    assert.match(remote.document.querySelector(".user-bubble").textContent, /Linux build on build-node-01/);
    const offline = screen("screens/connections.html?remote=offline&machine=linux-build-01");
    assert.match(offline.document.querySelector("#machine-select").title, /build-node-01 · Offline/);
    assert.equal(offline.document.querySelector(".remote-repository .conversation.unavailable")?.getAttribute("aria-disabled"), "true");
    assert.equal(offline.document.querySelector(".remote-repository [data-action='new-chat']").disabled, true);
    const remoteWorkspace = screen(`screens/${offline.K.url("workspace", {
        repo: "api-service", tree: "develop", conversation: "linux-build", state: "ready",
    })}`);
    assert.equal(remoteWorkspace.K.current.connection, "offline");
    assert.equal(remoteWorkspace.document.querySelector("#prompt").disabled, true);
    assert.equal(remoteWorkspace.document.querySelector('.agent-heading [data-action="new-chat"]').disabled, true);
    assert.equal(remoteWorkspace.document.querySelector('.agent-heading [data-action="terminal"]').disabled, true);
    assert.match(remoteWorkspace.document.querySelector(".chat-scroll").textContent, /Linux machine offline/);
    workspace.close();
    remote.close();
    offline.close();
    remoteWorkspace.close();
});
test("Add connection chooses phone or Linux within the same Connections page", () => {
    const connections = screen("screens/connections.html");
    connections.click('[data-action="add-connection"]');
    const chooser = connections.follow();
    assert.ok(chooser.document.querySelector('[data-action="pair-device"]'));
    chooser.click('[data-action="remote-add"]');
    const pairing = chooser.follow();
    assert.equal(pairing.K.page, "connections");
    assert.equal(pairing.K.remoteState(), "add");
    assert.ok(pairing.document.querySelector("#remote-code-form"));
    assert.ok(pairing.document.querySelector(".devices-list"));
    assert.ok(pairing.document.querySelector(".remote-host-row"), "existing Linux machines stay visible during pairing");
});
test("changing the machine filter in Connections keeps the open host details", () => {
    const details = screen("screens/connections.html?remote=details");
    const picker = details.document.querySelector("#machine-select");
    picker.value = "linux-build-01";
    picker.dispatchEvent(new details.w.Event("change", { bubbles: true }));
    const after = details.follow();
    assert.equal(after.K.remoteState(), "details");
    assert.equal(after.document.querySelector("#machine-select").value, "linux-build-01");
    details.close();
    after.close();
});
test("remote Linux sign-in returns to machines after acknowledgment", () => {
    const gate = screen("screens/connections.html?state=signed-out&remote=auth-required");
    assert.equal(gate.K.demo.auth, "signed-out");
    gate.click('[data-action="open-signin"]');
    const connections = gate.follow();
    connections.input("#account", "demo-user");
    connections.input("#password", "demo-only-secret");
    connections.submit("#signin-form");
    assert.equal(connections.url().includes("demo-only-secret"), false);
    const pending = connections.follow();
    pending.tick();
    const remote = pending.follow();
    assert.equal(remote.K.page, "connections");
    assert.equal(remote.K.demo.auth, "signed-in");
    assert.match(remote.document.body.textContent, /No Linux machines yet/);
});
test("remote pairing validates code locally, confirms identity, then adds trust", () => {
    const empty = screen("screens/connections.html?remote=empty");
    empty.click('[data-action="remote-add"]');
    const form = empty.follow();
    form.input("#remote-code", "bad");
    form.submit("#remote-code-form");
    assert.equal(form.url(), undefined);
    assert.equal(form.document.querySelector("#remote-code").getAttribute("aria-invalid"), "true");
    assert.equal(form.document.activeElement.id, "remote-code");
    form.input("#remote-code", "ketp-4826");
    form.submit("#remote-code-form");
    assert.equal(form.url().includes("KETP-4826"), false);
    const checking = form.follow();
    assert.equal(checking.K.demo.remoteHosts.length, 0);
    checking.tick();
    const verify = checking.follow();
    assert.equal(verify.document.querySelector('[data-action="remote-confirm"]').disabled, true);
    verify.click("#remote-code-matched");
    assert.equal(verify.document.querySelector('[data-action="remote-confirm"]').disabled, false);
    verify.click('[data-action="remote-confirm"]');
    const connecting = verify.follow();
    assert.equal(connecting.K.demo.remoteHosts.length, 0);
    connecting.tick();
    const connected = connecting.follow();
    assert.equal(connected.K.demo.remoteHosts.length, 1);
    assert.equal(connected.K.demo.remoteHosts[0].status, "connected");
    assert.equal(connected.w.location.href.includes("KETP-4826"), false);
});
test("remote pairing errors and mismatch never add a machine", () => {
    const expiredForm = screen("screens/connections.html?remote=add");
    expiredForm.input("#remote-code", "KETP-0000");
    expiredForm.submit("#remote-code-form");
    const checking = expiredForm.follow();
    checking.tick();
    const expired = checking.follow();
    assert.equal(expired.K.remoteState(), "code-expired");
    assert.equal(expired.K.demo.remoteHosts.length, 0);

    const verify = screen("screens/connections.html?remote=verify");
    verify.click('[data-action="remote-mismatch"]');
    const mismatch = verify.follow();
    assert.equal(mismatch.document.querySelector('[data-action="remote-confirm"]'), null);
    assert.equal(mismatch.K.demo.remoteHosts.length, 0);

    const failed = screen("screens/connections.html?remote=verify&outcome=error");
    failed.click("#remote-code-matched");
    failed.click('[data-action="remote-confirm"]');
    const pending = failed.follow();
    pending.tick();
    const error = pending.follow();
    assert.equal(error.K.remoteState(), "connect-error");
    assert.equal(error.K.demo.remoteHosts.length, 0);
});
test("remote pairing cancellation prevents a late simulated acknowledgment", () => {
    const checking = screen("screens/connections.html?remote=checking&auto=1");
    checking.click('[data-action="remote-cancel"]');
    const cancelledUrl = checking.url();
    checking.tick();
    assert.equal(checking.url(), cancelledUrl);
    const empty = checking.follow();
    assert.equal(empty.K.demo.remoteHosts.length, 0);
    assert.equal(empty.K.remoteState(), "empty");
});
test("remote states keep one primary action in each decision group", () => {
    for (const state of ["auth-required", "empty", "connected", "offline", "reconnect-error", "remove-error", "denied"]) {
        const s = screen(`screens/connections.html?remote=${state}`);
        assert.ok(s.document.querySelectorAll(".remote-page .btn.primary").length <= 1, state);
        s.close();
    }
});
test("remote disconnect, reconnect and failed reconnect preserve trusted identity", () => {
    const connected = screen("screens/connections.html?remote=connected");
    connected.click('[data-action="remote-disconnect"]');
    const offline = connected.follow();
    assert.equal(offline.K.demo.remoteHosts[0].status, "offline");
    offline.click('[data-action="remote-reconnect"]');
    const pending = offline.follow();
    assert.equal(pending.K.demo.remoteHosts[0].status, "offline");
    pending.tick();
    const back = pending.follow();
    assert.equal(back.K.demo.remoteHosts[0].status, "connected");

    const failure = screen("screens/connections.html?remote=offline&outcome=error");
    failure.click('[data-action="remote-reconnect"]');
    const retryPending = failure.follow();
    retryPending.tick();
    const retry = retryPending.follow();
    assert.equal(retry.K.remoteState(), "reconnect-error");
    assert.equal(retry.K.demo.remoteHosts[0].status, "offline");
});
test("remote forget retains trust until acknowledgment and leaves it after failure", () => {
    const details = screen("screens/connections.html?remote=details");
    details.click('[data-action="remote-forget"]');
    const confirm = details.follow();
    assert.ok(confirm.document.querySelector(".remote-forget-dialog"));
    confirm.click('[data-action="remote-confirm-forget"]');
    const removing = confirm.follow();
    assert.equal(removing.K.demo.remoteHosts.length, 1);
    removing.tick();
    const removed = removing.follow();
    assert.equal(removed.K.demo.remoteHosts.length, 0);

    const failed = screen("screens/connections.html?remote=details&outcome=error");
    failed.click('[data-action="remote-forget"]');
    const failConfirm = failed.follow();
    failConfirm.click('[data-action="remote-confirm-forget"]');
    const pending = failConfirm.follow();
    pending.tick();
    const error = pending.follow();
    assert.equal(error.K.remoteState(), "remove-error");
    assert.equal(error.K.demo.remoteHosts.length, 1);
});
test("remote host snapshot rejects invalid identity and escapes valid display text", () => {
    const base = screen("screens/connections.html?remote=empty");
    const snapshot = JSON.parse(JSON.stringify(base.K.demo));
    snapshot.remoteHosts = [{ id: 'bad"><img id="injected">', name: "Host", os: "Linux", status: "connected", lastSeen: "Now" }];
    const malformed = screen(`screens/connections.html#${new URLSearchParams({ demo: JSON.stringify(snapshot) })}`);
    assert.equal(malformed.K.demo.remoteHosts.length, 0);
    assert.equal(malformed.document.querySelector("#injected"), null);
    snapshot.remoteHosts[0].id = "safe-id";
    snapshot.remoteHosts[0].name = '<img id="injected">';
    const escaped = screen(`screens/connections.html#${new URLSearchParams({ demo: JSON.stringify(snapshot) })}`);
    assert.equal(escaped.document.querySelector("#injected"), null);
    assert.match(escaped.document.body.textContent, /<img id="injected">/);
});
