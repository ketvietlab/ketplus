import assert from "node:assert/strict";
import test from "node:test";
import { TurnInput, userMessage } from "../input.mjs";

const start = (parent = null) => ({ type: "stream_event", parent_tool_use_id: parent, event: { type: "message_start" } });
const result = { type: "result" };

async function take(iterator) {
  const next = await Promise.race([iterator.next(), new Promise((r) => setTimeout(() => r("pending"), 20))]);
  return next === "pending" ? next : next.done ? "done" : next.value;
}

test("the prompt streams first and a result without steering ends the stream", async () => {
  const input = new TurnInput(userMessage("first"));
  const iterator = input[Symbol.asyncIterator]();
  assert.equal((await take(iterator)).message.content, "first");
  assert.equal(await take(iterator), "pending");
  input.observe(start());
  input.observe(result);
  assert.equal(input.settle(), true);
  assert.equal(await take(iterator), "done");
  assert.equal(input.steer("late"), false);
});

test("a steer reaches the running query", async () => {
  const input = new TurnInput(userMessage("first"));
  const iterator = input[Symbol.asyncIterator]();
  await take(iterator);
  const waiting = iterator.next();
  assert.equal(input.steer("use the other branch"), true);
  const { value } = await waiting;
  assert.equal(value.message.content, "use the other branch");
  assert.equal(value.parent_tool_use_id, null);
  // Waits for the running tool call instead of interrupting it.
  assert.equal(value.priority, "next");
});

test("a result keeps the stream open for a steer no request has carried yet", () => {
  const input = new TurnInput(userMessage("first"));
  input.observe(start());
  input.steer("also update the docs");
  // A subagent's request does not carry the main thread's steer.
  input.observe(start("toolu_agent"));
  input.observe(result);
  assert.equal(input.settle(), false);
  input.observe(start());
  input.observe(result);
  assert.equal(input.settle(), true);
});

test("a steer folded into a later request does not hold the stream open", () => {
  const input = new TurnInput(userMessage("first"));
  input.observe(start());
  input.steer("stop after the tests");
  input.observe(start());
  input.observe(result);
  assert.equal(input.settle(), true);
});


test("result correlation covers prompt batches and background origins", () => {
  const first = userMessage("continue");
  const input = new TurnInput(first);
  assert.ok(first.uuid);
  assert.notEqual(first.uuid, userMessage("continue").uuid);
  assert.equal(input.resultPending({result:"old", user_message_uuid:"previous"}), true);
  assert.equal(input.resultPending({result:"old", queued_turn_count:1}), true);
  assert.equal(input.resultPending({result:"", origin:{kind:"task-notification"}}), true);
  assert.equal(input.resultPending({result:"", user_message_uuids:["merged", first.uuid],
    origin:{kind:"task-notification"}}), false);
  input.answered();
  assert.equal(input.resultPending({result:"background", origin:{kind:"task-notification"}}), false);
});

test("stale and synthetic starts cannot acknowledge a pending steer", () => {
  const input = new TurnInput(userMessage("start"));
  input.steer("continue");
  input.observe({...start(), user_message_uuid:"old"});
  assert.equal(input.unanswered, true);
  input.observe({...start(), event:{type:"message_start", message:{model:"<synthetic>"}}});
  assert.equal(input.unanswered, true);
  input.observe({...start(), user_message_uuid:input.latestId});
  assert.equal(input.unanswered, false);
});
