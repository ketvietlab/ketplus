import { randomUUID } from "node:crypto";

// A turn's prompt as an open stream of user messages, so steering reaches the
// running query. A steer never cuts in: Claude Code lets the running tool call
// finish and folds the steer into the turn before its next model request, or
// answers it with one more request after the current result.
export class TurnInput {
  constructor(first) {
    this.queue = [first];
    this.wake = null;
    this.closed = false;
    this.latestId = first.uuid;
    this.awaitingResult = true;
    this.modelStarted = false;
    // A steer sent since the last top-level model request started is not in
    // any request yet, so a result must not end the stream under it.
    this.unanswered = false;
  }
  // `content` is the steer's text, or its text and images as message blocks.
  steer(content) {
    if (this.closed) return false;
    this.unanswered = true;
    // "next" waits for the tool boundary; "now" would abort the running step.
    this.push({ ...userMessage(content), priority: "next" });
    return true;
  }
  // A new user turn on a query kept open for background tasks. Nothing is
  // running, so it needs no priority; like a steer, a result must wait for it.
  send(message) {
    if (this.closed) return false;
    this.unanswered = true;
    this.push(message);
    return true;
  }
  push(message) {
    this.latestId = message.uuid;
    this.awaitingResult = true;
    this.modelStarted = false;
    this.queue.push(message);
    this.wake?.();
  }
  observe(message) {
    const event = message.type === "stream_event" ? message.event : null;
    if (event?.type !== "message_start" || message.parent_tool_use_id ||
        event.message?.model === "<synthetic>" || this.belongsToOtherPrompt(message)) return;
    this.modelStarted = true;
    this.unanswered = false;
  }
  belongsToOtherPrompt(message) {
    const ids = [...(Array.isArray(message.user_message_uuids) ? message.user_message_uuids : []),
      message.user_message_uuid].filter(id => typeof id === "string" && id);
    if (ids.length) return !ids.includes(this.latestId);
    return Boolean(message.origin?.kind && message.origin.kind !== "human");
  }
  // Resume may deliver a task-notification result before consuming our prompt.
  // Older SDKs omit correlation fields, so retain their real-result behavior.
  resultPending(message) {
    if (message.queued_turn_count > 0) return true;
    if (!this.awaitingResult) return false;
    if (this.belongsToOtherPrompt(message)) return true;
    if (message.is_error) return false;
    const matched = message.user_message_uuid === this.latestId ||
      (Array.isArray(message.user_message_uuids) && message.user_message_uuids.includes(this.latestId));
    if (matched) return false;
    return this.unanswered || (!this.modelStarted &&
      !(typeof message.result === "string" && message.result.trim()));
  }
  answered() {
    this.awaitingResult = false;
    this.unanswered = false;
  }
  // After a result: end the stream unless a steer still waits on a request.
  settle() {
    if (!this.unanswered) this.close();
    return this.closed;
  }
  close() {
    this.closed = true;
    this.wake?.();
  }
  async *[Symbol.asyncIterator]() {
    while (true) {
      if (this.queue.length > 0) {
        yield this.queue.shift();
        continue;
      }
      if (this.closed) return;
      await new Promise((resolve) => { this.wake = resolve; });
      this.wake = null;
    }
  }
}

export function userMessage(content) {
  return { type: "user", uuid: randomUUID(), message: { role: "user", content }, parent_tool_use_id: null, session_id: "" };
}
