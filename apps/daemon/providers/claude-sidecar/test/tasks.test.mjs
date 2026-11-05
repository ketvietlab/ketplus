import assert from "node:assert/strict";
import test from "node:test";
import { BackgroundTasks } from "../tasks.mjs";

function tracker() {
  let clock = 1000;
  const sent = [];
  const tasks = new BackgroundTasks((payload) => sent.push(payload.tasks), { now: () => clock });
  return { tasks, sent, advance: (ms) => (clock += ms) };
}

const level = (...ids) => ({
  type: "system",
  subtype: "background_tasks_changed",
  tasks: ids.map((task_id) => ({ task_id, task_type: "local_agent", description: `Agent ${task_id}` })),
});

test("the level decides what runs; a task leaving it unannounced has ended", () => {
  const { tasks, sent } = tracker();
  tasks.accept(level("a", "b"));
  assert.deepEqual(sent.at(-1).map((t) => [t.task_id, t.status, t.kind]), [["a", "running", "agent"], ["b", "running", "agent"]]);
  assert.equal(tasks.hasLive(), true);
  tasks.accept(level("b"));
  assert.deepEqual(sent.at(-1).map((t) => [t.task_id, t.status]), [["b", "running"], ["a", "completed"]]);
  tasks.accept(level());
  assert.equal(tasks.hasLive(), false);
});

test("foreground and ambient tasks are not background activity", () => {
  const { tasks, sent } = tracker();
  tasks.accept({ type: "system", subtype: "task_started", task_id: "fg", task_type: "local_agent", description: "Explore", is_backgrounded: false });
  tasks.accept({ type: "system", subtype: "task_started", task_id: "watch", task_type: "local_bash", description: "Watch", is_backgrounded: true, ambient: true });
  assert.deepEqual(sent.at(-1), []);
  assert.equal(tasks.hasLive(), false);
  // A foreground subagent moved to the background becomes a row.
  tasks.accept({ type: "system", subtype: "task_updated", task_id: "fg", patch: { is_backgrounded: true } });
  assert.deepEqual(sent.at(-1).map((t) => t.task_id), ["fg"]);
  assert.equal(tasks.hasLive(), true);
});

test("progress is throttled and an ended task keeps its output file until the next turn", async () => {
  const { tasks, sent, advance } = tracker();
  tasks.progressIntervalMs = 20;
  tasks.accept(level("a"));
  const count = sent.length;
  tasks.accept({ type: "system", subtype: "task_progress", task_id: "a", description: "Agent a", usage: { total_tokens: 10, tool_uses: 1, duration_ms: 5 }, last_tool_name: "Read" });
  tasks.accept({ type: "system", subtype: "task_progress", task_id: "a", description: "Agent a", usage: { total_tokens: 20, tool_uses: 2, duration_ms: 9 }, last_tool_name: "Grep" });
  assert.equal(sent.length, count);
  await new Promise((resolve) => setTimeout(resolve, 40));
  advance(40);
  assert.equal(sent.length, count + 1);
  assert.equal(sent.at(-1)[0].tool_uses, 2);
  assert.equal(sent.at(-1)[0].last_tool, "Grep");

  tasks.accept({ type: "system", subtype: "task_notification", task_id: "a", status: "failed", output_file: "/tmp/a.out", summary: "Agent a failed" });
  assert.equal(sent.at(-1)[0].status, "failed");
  assert.equal(tasks.outputFile("a"), "/tmp/a.out");
  tasks.forgetEnded();
  assert.deepEqual(sent.at(-1), []);
  tasks.dispose();
});

test("a stop is shown until the SDK answers, and a lost worker orphans what ran", () => {
  const { tasks, sent } = tracker();
  tasks.accept(level("a"));
  assert.equal(tasks.markStopping("a"), true);
  assert.equal(sent.at(-1)[0].status, "stopping");
  tasks.restore("a");
  assert.equal(sent.at(-1)[0].status, "running");
  assert.equal(tasks.orphan(), true);
  assert.deepEqual(sent.at(-1).map((t) => [t.status, t.reason]), [["stopped", "worker_restart"]]);
  assert.equal(tasks.hasLive(), false);
});
