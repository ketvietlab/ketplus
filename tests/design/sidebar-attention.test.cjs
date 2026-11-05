const test = require("node:test");
const assert = require("node:assert/strict");
const { screen } = require("./ketatlas-dom.cjs");

test("async questions and confirmations pause the sidebar until resolved", () => {
    const s = screen("screens/workspace.html?state=running");
    const record = s.K.current;
    assert.match(s.K.conversationActivity(record), /status-working/);
    record.needsInput = true;
    const waiting = s.K.conversationActivity(record);
    assert.match(waiting, /status-waiting/);
    assert.match(waiting, /aria-label="Needs input"/);
    assert.doesNotMatch(waiting, /status-working/);
    assert.equal(record.turn, "running");
    const background = screen("screens/" + s.K.url("workspace", { conversation: "startup" }));
    assert.ok(background.document.querySelector(".conversation-indicator.status-waiting"));
    assert.notEqual(background.K.current.id, record.id);
    background.close();
    record.needsInput = false;
    assert.match(s.K.conversationActivity(record), /status-working/);
    record.turn = "ready";
    record.needsInput = true;
    assert.match(s.K.conversationActivity(record), /status-completed/);
    s.close();
});
