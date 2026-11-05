// Background work the Claude Agent SDK reports for one session: shells and
// subagents that keep running after the tool call that started them. The SDK
// sends edges (task_started/progress/updated/notification) and a level
// (background_tasks_changed, the full live set). The level decides what is
// running; the edges only describe it. Clients get one snapshot with REPLACE
// semantics, so a missed event can never wedge a stale running row.

const kinds = { local_bash: "shell", monitor: "shell", local_agent: "agent" };
// At most this many ended tasks stay listed until the next user turn.
const endedLimit = 8;

export class BackgroundTasks {
  constructor(emit, { progressIntervalMs = 2000, now = () => Date.now() } = {}) {
    this.emit = emit;
    this.now = now;
    this.progressIntervalMs = progressIntervalMs;
    this.tasks = new Map();
    // Tool inputs by tool_use id: the command or prompt a task row names.
    this.toolInputs = new Map();
    this.live = new Set();
    this.timer = null;
    this.lastEmit = 0;
  }

  // Returns true when the message changed what a client shows.
  accept(message) {
    if (message?.type === "assistant") {
      for (const block of Array.isArray(message.message?.content) ? message.message.content : []) {
        if (block?.type === "tool_use" && block.id) this.toolInputs.set(block.id, block.input || {});
      }
      return false;
    }
    if (message?.type !== "system") return false;
    switch (message.subtype) {
      case "background_tasks_changed":
        return this.level(Array.isArray(message.tasks) ? message.tasks : []);
      case "task_started":
        return this.started(message);
      case "task_progress":
        return this.progress(message);
      case "task_updated":
        return this.updated(message);
      case "task_notification":
        return this.ended(message);
      default:
        return false;
    }
  }

  level(entries) {
    const next = new Set();
    for (const entry of entries) {
      if (!entry?.task_id) continue;
      next.add(entry.task_id);
      const task = this.ensure(entry.task_id, entry.task_type, entry.description);
      task.ambient = entry.ambient === true;
      task.backgrounded = true;
      if (task.status !== "stopping") task.status = "running";
    }
    // A task that left the level without a notification ended unobserved.
    for (const id of this.live) {
      const task = this.tasks.get(id);
      if (!next.has(id) && task && (task.status === "running" || task.status === "stopping"))
        this.finish(task, "completed");
    }
    this.live = next;
    this.publish(true);
    return true;
  }

  started(message) {
    const task = this.ensure(message.task_id, message.task_type, message.description);
    task.toolUseId = message.tool_use_id || task.toolUseId;
    task.subagent = message.subagent_type || task.subagent;
    task.ambient = message.ambient === true || message.skip_transcript === true;
    if (message.is_backgrounded === true) task.backgrounded = true;
    const input = this.toolInputs.get(task.toolUseId);
    if (task.kind === "shell" && typeof input?.command === "string") task.command = input.command;
    if (task.kind === "agent" && !task.summary && typeof input?.description === "string")
      task.summary = input.description;
    this.publish(true);
    return true;
  }

  progress(message) {
    const task = this.tasks.get(message.task_id);
    if (!task) return false;
    if (message.description) task.title = message.description;
    if (message.usage) {
      task.tokens = message.usage.total_tokens ?? task.tokens;
      task.toolUses = message.usage.tool_uses ?? task.toolUses;
      task.durationMs = message.usage.duration_ms ?? task.durationMs;
    }
    if (message.last_tool_name) task.lastTool = message.last_tool_name;
    if (message.summary) task.summary = message.summary;
    this.publish(false);
    return true;
  }

  updated(message) {
    const task = this.tasks.get(message.task_id);
    const patch = message.patch || {};
    if (!task) return false;
    if (patch.description) task.title = patch.description;
    if (patch.is_backgrounded === true) task.backgrounded = true;
    if (patch.error) task.error = patch.error;
    this.publish(true);
    return true;
  }

  ended(message) {
    const task = this.ensure(message.task_id, "", message.summary);
    task.outputFile = message.output_file || task.outputFile;
    if (message.summary) task.result = message.summary;
    if (message.reason) task.reason = message.reason;
    if (message.usage) {
      task.tokens = message.usage.total_tokens ?? task.tokens;
      task.toolUses = message.usage.tool_uses ?? task.toolUses;
      task.durationMs = message.usage.duration_ms ?? task.durationMs;
    }
    this.live.delete(task.id);
    this.finish(task, ["completed", "failed", "stopped"].includes(message.status) ? message.status : "completed");
    this.publish(true);
    return true;
  }

  ensure(id, type, description) {
    let task = this.tasks.get(id);
    if (!task) {
      task = {
        id,
        kind: kinds[type] || "other",
        title: description || "Background task",
        status: "running",
        startedAt: this.now(),
        backgrounded: false,
        ambient: false,
      };
      this.tasks.set(id, task);
    } else if (type && task.kind === "other") {
      task.kind = kinds[type] || "other";
    }
    return task;
  }

  finish(task, status) {
    task.status = status;
    task.endedAt = task.endedAt || this.now();
  }

  // A stop request the SDK has not answered yet.
  markStopping(id) {
    const task = this.tasks.get(id);
    if (!task || task.status !== "running") return false;
    task.status = "stopping";
    this.publish(true);
    return true;
  }

  // The stop request failed; the level still says what is running.
  restore(id) {
    const task = this.tasks.get(id);
    if (task?.status !== "stopping") return;
    task.status = "running";
    this.publish(true);
  }

  // True while background work the user would call activity is running.
  hasLive() {
    for (const task of this.tasks.values()) {
      if (task.backgrounded && !task.ambient && (task.status === "running" || task.status === "stopping"))
        return true;
    }
    return false;
  }

  // The worker process is gone; nothing it ran is still running.
  orphan(reason = "worker_restart") {
    let changed = false;
    for (const task of this.tasks.values()) {
      if (task.status !== "running" && task.status !== "stopping") continue;
      this.finish(task, "stopped");
      task.reason = reason;
      changed = true;
    }
    this.live.clear();
    if (changed) this.publish(true);
    return changed;
  }

  // A new user turn starts with only what is still running.
  forgetEnded() {
    let changed = false;
    for (const [id, task] of this.tasks) {
      if (task.status === "running" || task.status === "stopping") continue;
      this.tasks.delete(id);
      changed = true;
    }
    if (changed) this.publish(true);
  }

  outputFile(id) {
    return this.tasks.get(id)?.outputFile || "";
  }

  has(id) {
    return this.tasks.has(id);
  }

  snapshot() {
    const visible = [...this.tasks.values()].filter((task) => task.backgrounded && !task.ambient);
    const active = visible.filter((task) => task.status === "running" || task.status === "stopping");
    const ended = visible.filter((task) => !active.includes(task)).slice(-endedLimit);
    return [...active, ...ended].map((task) => ({
      task_id: task.id,
      kind: task.kind,
      title: task.title,
      status: task.status,
      started_at: task.startedAt,
      ...(task.endedAt ? { ended_at: task.endedAt } : {}),
      ...(task.command ? { command: task.command } : {}),
      ...(task.subagent ? { subagent_type: task.subagent } : {}),
      ...(task.summary ? { summary: task.summary } : {}),
      ...(task.lastTool ? { last_tool: task.lastTool } : {}),
      ...(task.toolUses !== undefined ? { tool_uses: task.toolUses } : {}),
      ...(task.tokens !== undefined ? { total_tokens: task.tokens } : {}),
      ...(task.durationMs !== undefined ? { duration_ms: task.durationMs } : {}),
      ...(task.result ? { result: task.result } : {}),
      ...(task.error ? { error: task.error } : {}),
      ...(task.reason ? { reason: task.reason } : {}),
      has_output: task.kind === "shell",
    }));
  }

  // Membership and status changes go out at once; progress at most every interval.
  publish(immediate) {
    const wait = this.lastEmit + this.progressIntervalMs - this.now();
    if (!immediate && wait > 0) {
      this.timer ||= setTimeout(() => this.flush(), wait);
      return;
    }
    this.flush();
  }

  flush() {
    clearTimeout(this.timer);
    this.timer = null;
    const tasks = this.snapshot();
    const key = JSON.stringify(tasks);
    if (key === this.lastKey) return;
    this.lastKey = key;
    this.lastEmit = this.now();
    this.emit({ tasks });
  }

  dispose() {
    clearTimeout(this.timer);
    this.timer = null;
  }
}
