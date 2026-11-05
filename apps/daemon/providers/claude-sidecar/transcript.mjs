import path from "node:path";

// SDK messages are snapshots as well as deltas. Keep their identities so a snapshot
// repairs missed deltas without duplicating text or merging distinct assistant steps.
// Tools whose `file_path` (or `notebook_path`) names the one file they read or change.
const fileTools = new Set(["Read", "Edit", "MultiEdit", "Write", "NotebookEdit"]);

// A file inside the worktree as a worktree-relative path, the form a client opens it by.
// Outside paths retain their absolute origin for daemon-verified resource resolution.
function worktreePath(cwd, file) {
  if (!cwd || typeof file !== "string" || !path.isAbsolute(file)) return "";
  const relative = path.relative(cwd, file);
  return relative && !relative.startsWith("..") && !path.isAbsolute(relative) ? relative.split(path.sep).join("/") : "";
}

export class ClaudeTranscript {
  constructor(turnId, emit, cwd = "") {
    this.turnId = turnId;
    this.emit = emit;
    this.cwd = cwd;
    this.items = new Map();
    this.messageId = "";
    this.blocks = new Map();
    this.lastText = "";
    this.serial = 0;
    // Streamed text item ids per message, in stream order, and the ones a
    // snapshot has already claimed.
    this.streamedText = new Map();
    this.snapshotted = new Set();
  }
  publish(item, completed = false) {
    this.items.set(item.id, item);
    this.emit(completed ? "item.completed" : "item.started", {
      item_id: item.id, item_type: item.type, item,
    });
  }
  text(id, text, completed = false, phase = "commentary") {
    this.publish({ id, type: "agentMessage", phase, text,
      status: completed ? "completed" : "inProgress" }, completed);
    this.lastText = id;
  }
  streamedTextItem(id) {
    const ids = this.streamedText.get(this.messageId) || [];
    if (!ids.includes(id)) ids.push(id);
    this.streamedText.set(this.messageId, ids);
  }
  // Claude Code sends each content block as its own assistant snapshot, so a
  // block's index there is not its stream index: text after thinking streams as
  // index 1 and arrives as index 0. The snapshot joins the streamed item with the
  // same text, else the first streamed one it has not claimed.
  snapshotTextId(mid, index, text) {
    const streamed = this.streamedText.get(mid) || [];
    const id = streamed.find((candidate) => !this.snapshotted.has(candidate)
        && this.items.get(candidate)?.text === text)
      || streamed.find((candidate) => !this.snapshotted.has(candidate))
      || `${mid}-${index}`;
    this.snapshotted.add(id);
    return id;
  }
  tool(block) {
    const previous = this.items.get(block.id);
    const args = block.input || previous?.arguments || {};
    const item = { id: block.id, type: block.name === "Bash" ? "commandExecution" : "dynamicToolCall",
      tool: block.name, arguments: args, status: "inProgress" };
    if (item.type === "commandExecution") {
      item.command = args.command || block.name;
      // Claude's own words for what the command does ("List the relay's tests").
      if (typeof args.description === "string" && args.description.trim())
        item.description = args.description.trim();
    }
    if (fileTools.has(block.name)) {
      // Preserve explicit absolute paths so clients can resolve another checkout.
      const requested = args.file_path ?? args.notebook_path;
      const file = worktreePath(this.cwd, requested);
      if (file) item.path = file;
      else if (typeof requested === "string" && requested) {
        item.outsideWorktree = true;
        if (path.isAbsolute(requested)) item.path = requested;
      }
    }
    this.publish(item);
  }
  accept(message) {
    if (message.type === "stream_event") {
      const event = message.event || {};
      if (event.type === "message_start") {
        this.messageId = event.message?.id || `${this.turnId}-message-${++this.serial}`;
        this.blocks.clear();
      }
      const index = event.index ?? 0;
      const id = `${this.messageId || this.turnId}-${index}`;
      if (event.type === "content_block_start") {
        const b = event.content_block || {};
        this.blocks.set(index, { ...b, id: b.id || id });
        if (b.type === "text") {
          this.streamedTextItem(id);
          this.text(id, b.text || "");
        }
        if (b.type === "tool_use") this.tool(b);
      }
      if (event.type === "content_block_delta" && event.delta?.type === "text_delta") {
        const prior = this.items.get(id);
        if (!prior) {
          this.streamedTextItem(id);
          this.text(id, "");
        }
        const item = this.items.get(id);
        item.text += event.delta.text || "";
        this.emit("assistant.delta", { item_id: id, delta: event.delta.text || "" });
      }
      if (event.type === "content_block_stop") {
        const item = this.items.get(id);
        if (item?.type === "agentMessage") this.text(id, item.text, true);
      }
      return;
    }
    if (message.type === "assistant") {
      const msg = message.message || {};
      const mid = msg.id || message.uuid || this.messageId || `${this.turnId}-${++this.serial}`;
      for (const [index, block] of (Array.isArray(msg.content) ? msg.content : []).entries()) {
        if (block.type === "text") this.text(this.snapshotTextId(mid, index, block.text || ""), block.text || "", true);
        if (block.type === "tool_use") this.tool(block);
        // Thinking is intentionally private, matching the other providers.
      }
    }
    if (message.type === "user") {
      for (const block of Array.isArray(message.message?.content) ? message.message.content : []) {
        if (block.type !== "tool_result") continue;
        const item = this.items.get(block.tool_use_id) || {
          id: block.tool_use_id, type: "dynamicToolCall", tool: "Tool",
        };
        const content = (typeof block.content === "string"
          ? [{ type: "text", text: block.content }] : block.content || []).map(b =>
            b.type === "image" && b.source?.type === "base64"
              ? { type: "image", data: b.source.data, mimeType: b.source.media_type } : b);
        item.result = { content, isError: block.is_error === true };
        item.status = block.is_error ? "failed" : "completed";
        if (item.type === "commandExecution")
          item.aggregatedOutput = content.filter(b => b.type === "text").map(b => b.text).join("\n");
        this.publish(item, true);
      }
    }
    if (message.type === "system" && message.subtype === "compact_boundary")
      this.publish({ id: `${this.turnId}-compact-${++this.serial}`, type: "contextCompaction" }, true);
    if (message.type === "system" && message.subtype === "api_error")
      this.emit("runtime.error", { message: message.error?.message || "Claude request failed." });
    if (message.type === "result") {
      const prior = this.items.get(this.lastText);
      const text = typeof message.result === "string" ? message.result : prior?.text || "";
      if (text) this.text(this.lastText || `${this.turnId}-answer`, text, true, "final_answer");
      if (message.is_error)
        this.emit("runtime.error", { message: (message.errors || []).join("\n") || text || "Claude turn failed." });
    }
  }
  finish(interrupted = false) {
    for (const item of this.items.values()) {
      if (item.status !== "inProgress") continue;
      this.publish({ ...item, status: interrupted ? "interrupted" : "completed" }, true);
    }
  }
}
