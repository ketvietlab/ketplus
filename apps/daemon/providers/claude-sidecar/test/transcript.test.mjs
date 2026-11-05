import assert from "node:assert/strict";
import test from "node:test";
import { ClaudeTranscript } from "../transcript.mjs";

function record() {
  const events = [];
  const transcript = new ClaudeTranscript("turn-1", (type, payload) => events.push({ type, payload }));
  const messages = () => {
    const latest = new Map();
    for (const { payload } of events)
      if (payload.item?.type === "agentMessage") latest.set(payload.item.id, payload.item);
    return [...latest.values()];
  };
  return { transcript, messages };
}

const stream = (event) => ({ type: "stream_event", event });

function streamText(transcript, messageId, index, text) {
  transcript.accept(stream({ type: "content_block_start", index, content_block: { type: "text", text: "" } }));
  transcript.accept(stream({ type: "content_block_delta", index, delta: { type: "text_delta", text } }));
  transcript.accept(stream({ type: "content_block_stop", index }));
}

test("text after thinking is one answer, not the stream and the per-block snapshot", () => {
  const { transcript, messages } = record();
  transcript.accept(stream({ type: "message_start", message: { id: "msg_a" } }));
  transcript.accept(stream({ type: "content_block_start", index: 0, content_block: { type: "thinking" } }));
  transcript.accept(stream({ type: "content_block_stop", index: 0 }));
  transcript.accept({ type: "assistant", message: { id: "msg_a", content: [{ type: "thinking", thinking: "private" }] } });
  streamText(transcript, "msg_a", 1, "The answer.");
  // Claude Code snapshots each block alone, so the text is at index 0 here.
  transcript.accept({ type: "assistant", message: { id: "msg_a", content: [{ type: "text", text: "The answer." }] } });
  transcript.accept({ type: "result", result: "The answer." });
  transcript.finish();

  const answers = messages();
  assert.equal(answers.length, 1);
  assert.equal(answers[0].id, "msg_a-1");
  assert.equal(answers[0].text, "The answer.");
  assert.equal(answers[0].phase, "final_answer");
});

test("a snapshot repairs missed deltas on the streamed item", () => {
  const { transcript, messages } = record();
  transcript.accept(stream({ type: "message_start", message: { id: "msg_b" } }));
  transcript.accept(stream({ type: "content_block_start", index: 1, content_block: { type: "text", text: "" } }));
  transcript.accept(stream({ type: "content_block_delta", index: 1, delta: { type: "text_delta", text: "Partial" } }));
  transcript.accept({ type: "assistant", message: { id: "msg_b", content: [{ type: "text", text: "Partial and complete." }] } });

  const answers = messages();
  assert.equal(answers.length, 1);
  assert.equal(answers[0].text, "Partial and complete.");
});

test("distinct text blocks and messages stay distinct", () => {
  const { transcript, messages } = record();
  transcript.accept(stream({ type: "message_start", message: { id: "msg_c" } }));
  streamText(transcript, "msg_c", 0, "First step.");
  transcript.accept({ type: "assistant", message: { id: "msg_c", content: [{ type: "text", text: "First step." }] } });
  transcript.accept(stream({ type: "message_start", message: { id: "msg_d" } }));
  streamText(transcript, "msg_d", 1, "Second step.");
  transcript.accept({ type: "assistant", message: { id: "msg_d", content: [{ type: "text", text: "Second step." }] } });

  assert.deepEqual(messages().map((item) => [item.id, item.text]),
    [["msg_c-0", "First step."], ["msg_d-1", "Second step."]]);
});

test("a snapshot without a stream keeps its own item", () => {
  const { transcript, messages } = record();
  transcript.accept({ type: "assistant", message: { id: "msg_e", content: [{ type: "text", text: "Only snapshot." }] } });
  assert.deepEqual(messages().map((item) => item.id), ["msg_e-0"]);
});

test("a command says what it does and a file tool names its file inside the worktree", () => {
  const events = [];
  const transcript = new ClaudeTranscript("turn-1", (type, payload) => events.push(payload.item), "/work/repo");
  transcript.accept({ type: "assistant", message: { id: "msg_f", content: [
    { type: "tool_use", id: "bash-1", name: "Bash", input: { command: "cd /work/repo && git status", description: " Show working tree status " } },
    { type: "tool_use", id: "read-1", name: "Read", input: { file_path: "/work/repo/apps/ios/Sources/App.swift" } },
    { type: "tool_use", id: "read-2", name: "Read", input: { file_path: "/work/other/secret.txt" } },
    { type: "tool_use", id: "edit-1", name: "Edit", input: { file_path: "/work/repo-other/x.ts", old_string: "a", new_string: "b" } },
  ] } });
  const byId = new Map(events.map((item) => [item.id, item]));
  assert.equal(byId.get("bash-1").description, "Show working tree status");
  assert.equal(byId.get("read-1").path, "apps/ios/Sources/App.swift");
  assert.equal(byId.get("read-1").outsideWorktree, undefined);
  assert.equal(byId.get("read-2").path, "/work/other/secret.txt");
  assert.equal(byId.get("read-2").outsideWorktree, true);
  // A sibling folder that shares the worktree's name prefix is not inside it.
  assert.equal(byId.get("edit-1").path, "/work/repo-other/x.ts");
  assert.equal(byId.get("edit-1").outsideWorktree, true);
  assert.equal(byId.get("bash-1").outsideWorktree, undefined);
});
