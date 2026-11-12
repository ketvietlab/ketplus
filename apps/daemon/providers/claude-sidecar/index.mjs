#!/usr/bin/env node
import { sessionUsage } from "./session-usage.mjs";
import { deleteSessionCopies } from "./session-delete.mjs";
import { authenticationOptions, authenticationChangedWhileBusy, redact } from "./authentication.mjs";
import { ClaudeTranscript } from "./transcript.mjs";
import { TurnInput, userMessage } from "./input.mjs";
import { BackgroundTasks } from "./tasks.mjs";
import { ensureResumable } from "./resume.mjs";
import { reviewDecision, reviewOptions, reviewProfile } from "./review-guard.mjs";
import { open, readFile } from "node:fs/promises";
import path from "node:path";
import { createInterface } from "node:readline";
import { stdin as input, stdout, stderr } from "node:process";

function emit(value) {
  stdout.write(`${JSON.stringify(redact(value))}\n`);
}

function permissionMode(profile) {
  if (profile === "read-only") return "plan";
  if (profile === "danger-full-access") return "bypassPermissions";
  return "acceptEdits";
}

function textFromContent(content) {
  if (typeof content === "string") return content;
  if (!Array.isArray(content)) return "";
  return content
    .map((block) => {
      if (!block || typeof block !== "object") return "";
      if (typeof block.text === "string") return block.text;
      if (block.delta && typeof block.delta.text === "string") return block.delta.text;
      return "";
    })
    .join("");
}

function streamDelta(event) {
  if (!event || typeof event !== "object") return "";
  if (event.type === "content_block_delta" && event.delta?.type === "text_delta") {
    return typeof event.delta.text === "string" ? event.delta.text : "";
  }
  return "";
}

// Thinking budget per KetPlus effort level; low answers without extended thinking.
const thinkingTokens = { low: 0, medium: 8000, high: 32000, xhigh: 64000, max: 100000 };

function fallbackCatalog() {
  return {
    data: [
      {
        model: "opus",
        displayName: "Claude Opus",
        hidden: false,
        provider: "claude",
        defaultReasoningEffort: "high",
        supportedReasoningEfforts: [
          { reasoningEffort: "low" },
          { reasoningEffort: "medium" },
          { reasoningEffort: "high" },
        ],
      },
      {
        model: "sonnet",
        displayName: "Claude Sonnet",
        hidden: false,
        provider: "claude",
        defaultReasoningEffort: "medium",
        supportedReasoningEfforts: [
          { reasoningEffort: "low" },
          { reasoningEffort: "medium" },
          { reasoningEffort: "high" },
        ],
      },
      {
        model: "haiku",
        displayName: "Claude Haiku",
        hidden: false,
        provider: "claude",
        defaultReasoningEffort: "low",
        supportedReasoningEfforts: [
          { reasoningEffort: "low" },
          { reasoningEffort: "medium" },
          { reasoningEffort: "high" },
        ],
      },
    ],
  };
}

function effortEntries(info) {
  const levels = Array.isArray(info?.supportedEffortLevels) ? info.supportedEffortLevels : [];
  if (levels.length > 0) return levels.map((reasoningEffort) => ({ reasoningEffort }));
  if (info?.supportsEffort === false) return [];
  return [{ reasoningEffort: "low" }, { reasoningEffort: "medium" }, { reasoningEffort: "high" }];
}

function toCatalogEntry(info) {
  const id = info?.value || info?.model || "";
  const efforts = effortEntries(info);
  const defaultEffort =
    efforts.find((entry) => entry.reasoningEffort === "medium")?.reasoningEffort ||
    efforts[0]?.reasoningEffort ||
    "medium";
  return {
    model: id,
    displayName: info?.displayName || id,
    description: info?.description || "",
    hidden: false,
    provider: "claude",
    defaultReasoningEffort: defaultEffort,
    supportedReasoningEfforts: efforts,
  };
}

async function listSupportedModels() {
  if (!queryFn) return null;
  async function* emptyPrompt() {}
  const q = queryFn({
    prompt: emptyPrompt(),
    options: {
      permissionMode: "plan",
      persistSession: false,
      includePartialMessages: false,
    },
  });
  try {
    const timeout = new Promise((_, reject) => {
      setTimeout(() => reject(new Error("Claude model listing timed out.")), 8000);
    });
    if (typeof q.initializationResult === "function") {
      try {
        const init = await Promise.race([q.initializationResult(), timeout]);
        if (Array.isArray(init?.models) && init.models.length > 0) return init.models;
      } catch {
        // Fall through to supportedModels().
      }
    }
    if (typeof q.supportedModels !== "function") return null;
    const models = await Promise.race([q.supportedModels(), timeout]);
    return Array.isArray(models) ? models : null;
  } finally {
    try {
      q.close?.();
    } catch {
      try {
        await q.interrupt?.();
      } catch {
        // The listing query is discarded either way.
      }
    }
  }
}

let catalogCache = null;
let catalogPromise = null;

async function catalog() {
  if (catalogCache) return catalogCache;
  if (!catalogPromise) {
    catalogPromise = (async () => {
      const fallback = fallbackCatalog();
      try {
        const live = await listSupportedModels();
        if (!Array.isArray(live) || live.length === 0) return fallback;
        const data = [];
        const seen = new Set();
        for (const info of live) {
          const entry = toCatalogEntry(info);
          if (!entry.model || seen.has(entry.model)) continue;
          seen.add(entry.model);
          data.push(entry);
        }
        for (const extra of fallback.data) {
          if (!seen.has(extra.model)) data.push(extra);
        }
        return data.length > 0 ? { data } : fallback;
      } catch {
        return fallback;
      }
    })().then((result) => {
      catalogCache = result;
      return result;
    });
  }
  return catalogPromise;
}

function imageMediaType(file) {
  const extension = path.extname(file).toLowerCase();
  if (extension === ".png") return "image/png";
  if (extension === ".gif") return "image/gif";
  if (extension === ".webp") return "image/webp";
  return "image/jpeg";
}

// One user message carrying the text and every attached image.
async function promptMessage(text, images) {
  if (!Array.isArray(images) || images.length === 0) return userMessage(text);
  const content = [];
  for (const file of images) {
    if (typeof file !== "string" || !file) continue;
    const data = await readFile(file);
    content.push({
      type: "image",
      source: { type: "base64", media_type: imageMediaType(file), data: data.toString("base64") },
    });
  }
  if (text) content.push({ type: "text", text });
  return userMessage(content);
}

// A result with a steer still waiting keeps the stream open for the request
// that answers it; if none starts, the turn ends rather than hang.
const steerGraceMs = 60000;

const pendingPermissions = new Map();
// One query per session: the running turn, or one kept open while background
// tasks it started are still running.
const activeTurns = new Map();
// Each session's last closed query's tasks, whose output files stay readable.
const endedOutputs = new Map();
const taskOutputTailBytes = 8192;
let queryFn = null;
let forkFn = null;
let deleteFn = null;
let sdkError = "";

try {
  const sdk = await import("@anthropic-ai/claude-agent-sdk");
  queryFn = sdk.query;
  forkFn = typeof sdk.forkSession === "function" ? sdk.forkSession : null;
  deleteFn = typeof sdk.deleteSession === "function" ? sdk.deleteSession : null;
} catch (error) {
  sdkError = error instanceof Error ? error.message : String(error);
}

function emitEvent(sessionId, type, payload) {
  emit({ method: "event", params: { sessionId, type, payload } });
}

function finiteNumber(...values) {
  for (const value of values) {
    const n = Number(value);
    if (Number.isFinite(n)) return n;
  }
  return Number.NaN;
}

function occupancyTokenUsage(source) {
  if (!source || typeof source !== "object") return null;
  const used = finiteNumber(source.totalTokens, source.total_tokens);
  const limit = finiteNumber(source.maxTokens, source.max_tokens, source.modelContextWindow);
  const threshold = finiteNumber(source.autoCompactThreshold, source.auto_compact_threshold);
  if (!Number.isFinite(used) || used < 0 || !Number.isFinite(limit) || limit <= 0) return null;
  return {
    last: { totalTokens: Math.round(used) },
    modelContextWindow: Math.round(limit),
    ...(Number.isFinite(threshold) && threshold > 0
      ? { autoCompactThreshold: Math.round(threshold) }
      : {}),
  };
}

// The SDK answers both probes over the query's control channel, so they are only
// reachable while the query is still iterating. Draining it first closes the
// transport and every request after that hangs until this timeout.
const usageProbeTimeoutMillis = 2000;

function probe(promise) {
  return Promise.race([
    promise,
    new Promise((_, reject) => {
      setTimeout(() => reject(new Error("timeout")), usageProbeTimeoutMillis);
    }),
  ]);
}

// The last usage each session reported: the daemon journals every event, so an
// unchanged reading is not sent again.
const lastUsage = new Map();

function emitUsageEvent(sessionId, type, payload) {
  const key = `${sessionId}\n${type}`;
  const text = JSON.stringify(payload);
  if (lastUsage.get(key) === text) return;
  lastUsage.set(key, text);
  emitEvent(sessionId, type, payload);
}

async function emitContextUsage(sessionId, query) {
  if (!query || typeof query.getContextUsage !== "function") return;
  const tokenUsage = occupancyTokenUsage(await probe(query.getContextUsage()));
  if (tokenUsage) emitUsageEvent(sessionId, "usage.context", { tokenUsage });
}

function epochSeconds(value) {
  const millis = Date.parse(String(value));
  return Number.isFinite(millis) ? Math.round(millis / 1000) : 0;
}

function limitRow(id, label, window) {
  // A window the endpoint knows nothing about carries a null utilization, which
  // Number() would happily read as a confident 0%.
  if (!window || typeof window !== "object" || window.utilization === null) return null;
  const used = finiteNumber(window.utilization);
  if (!Number.isFinite(used) || used < 0) return null;
  return {
    id,
    label,
    usedPercent: Math.max(0, Math.min(100, used)),
    resetsAt: epochSeconds(window.resets_at),
    available: true,
  };
}

function planLimitRows(usage) {
  if (!usage || usage.rate_limits_available !== true) return [];
  const limits = usage.rate_limits;
  if (!limits || typeof limits !== "object") return [];
  const rows = [
    limitRow("five_hour", "Session \u00b7 5 hours", limits.five_hour),
    limitRow("seven_day", "Weekly \u00b7 all models", limits.seven_day),
    limitRow("seven_day_opus", "Weekly \u00b7 Opus", limits.seven_day_opus),
    limitRow("seven_day_sonnet", "Weekly \u00b7 Sonnet", limits.seven_day_sonnet),
  ];
  for (const [index, scoped] of (limits.model_scoped || []).entries()) {
    const label = scoped && scoped.display_name ? `Weekly \u00b7 ${scoped.display_name}` : null;
    if (label) rows.push(limitRow(`model_scoped_${index}`, label, scoped));
  }
  return rows.filter(Boolean);
}

async function emitPlanLimits(sessionId, query) {
  // EXPERIMENTAL in the SDK: the method is documented to be renamed or removed
  // without notice, so a build without it simply reports no plan limits.
  const read = query?.usage_EXPERIMENTAL_MAY_CHANGE_DO_NOT_RELY_ON_THIS_API_YET;
  if (typeof read !== "function") return;
  const usage = await probe(read.call(query, { skipBehaviors: true }));
  const run = activeTurns.get(sessionId);
  if (!run || run.query !== query) return;
  if (Number.isFinite(usage?.session?.total_cost_usd) && usage.session.total_cost_usd >= 0)
    emitUsageEvent(sessionId, "usage.session", sessionUsage(run.authentication, usage.session));
  const rows = run.authentication?.enabled ? [] : planLimitRows(usage);
  if (rows.length > 0) emitUsageEvent(sessionId, "usage.limits", { limits: rows });
}

async function emitUsage(sessionId, query) {
  // One probe failing must not hide the other; both are optional decoration.
  for (const emitOne of [emitContextUsage, emitPlanLimits]) {
    try {
      await emitOne(sessionId, query);
    } catch {
      // The window or the plan rows stay unreported for this turn.
    }
  }
}

// While a turn runs: the window after each top-level model step or compaction,
// the plan rows when the SDK reports a rate-limit change or a minute has passed.
// Probes never hold up the message loop, and one runs at a time.
const contextProbeEveryMs = 5000;
const limitsProbeEveryMs = 60000;

function liveUsage(sessionId, query) {
  const last = { context: 0, limits: Date.now() };
  let busy = false;
  return (message) => {
    const now = Date.now();
    const stepEnded = message.type === "assistant" && !message.parent_tool_use_id;
    const compacted = message.type === "system" && message.subtype === "compact_boundary";
    const wantContext = (stepEnded || compacted) && now - last.context >= contextProbeEveryMs;
    const wantLimits = message.type === "rate_limit_event" || (stepEnded && now - last.limits >= limitsProbeEveryMs);
    if (busy || (!wantContext && !wantLimits)) return;
    busy = true;
    if (wantContext) last.context = now;
    if (wantLimits) last.limits = now;
    (async () => {
      if (wantContext) await emitContextUsage(sessionId, query).catch(() => {});
      if (wantLimits) await emitPlanLimits(sessionId, query).catch(() => {});
    })().finally(() => { busy = false; });
  };
}

// A launch a kept-open query can serve. Anything else needs a fresh query.
function launchKey(params) {
  return JSON.stringify([
    params.cwd || "",
    params.mcpServers || {},
    params.browserInstructions || "",
    params.readableDirectories || [],
    params.compactWindow || 0,
    params.authentication?.revision || "",
    reviewProfile(params),
    params.scratchDirectory || "",
  ]);
}

function sdkUnavailable(messageId) {
  emit({
    id: messageId,
    error: {
      message: sdkError
        ? `Claude Agent SDK is unavailable: ${sdkError}`
        : "Claude Agent SDK is not installed. Stage claude-sidecar with package-sidecar.mjs or run npm install.",
    },
  });
}

function denyPendingPermissions(sessionId, message, interrupt = false) {
  for (const [id, pending] of pendingPermissions) {
    if (pending.sessionId !== sessionId) continue;
    pendingPermissions.delete(id);
    pending.resolve({ behavior: "deny", message, ...(interrupt ? { interrupt: true } : {}) });
  }
}

// Starts a daemon turn on `run`. A turn the agent starts itself (it answers a
// finished background task) has no request to reply to.
function beginTurn(run, messageId) {
  clearTimeout(run.idleTimer);
  const turnId = `claude-turn-${Date.now()}`;
  run.current = {
    turnId,
    interrupted: false,
    result: null,
    transcript: new ClaudeTranscript(turnId, (type, payload) => emitEvent(run.sessionId, type, payload), run.cwd),
  };
  // Only an id the SDK issued is a thread; an empty one is filled in once it reports it.
  if (messageId !== undefined)
    emit({ id: messageId, result: { turn: { id: turnId }, thread: { id: run.sdkSessionId } } });
  // Every turn is started and completed exactly once, however early it ends.
  emitEvent(run.sessionId, "turn.started", { turn_id: turnId, thread_id: run.sdkSessionId });
  return run.current;
}

function completeTurn(run, failed = false) {
  const current = run.current;
  if (!current) return;
  run.current = null;
  if (!current.interrupted && !failed && !current.result) {
    failed = true;
    emitEvent(run.sessionId, "runtime.error", {
      message: "Claude ended before answering the pending user message. No completed result was received.",
    });
  }
  current.transcript.finish(current.interrupted);
  denyPendingPermissions(run.sessionId, "Turn ended.");
  emitEvent(run.sessionId, "turn.completed", {
    status: current.interrupted ? "interrupted" : failed || current.result?.is_error ? "failed" : "completed",
    thread_id: run.sdkSessionId,
  });
}

// A top-level model request: the agent is answering something, so a daemon turn runs.
function startsModelRun(message) {
  if (message.parent_tool_use_id) return false;
  if (message.type === "stream_event") return message.event?.type === "message_start" &&
    message.event.message?.model !== "<synthetic>";
  return message.type === "assistant" && message.message?.model !== "<synthetic>";
}

// With every background task ended and nothing to answer, a kept-open query
// waits this long for the agent's reply to the last one before it closes.
const lingerSettleMs = 5000;

function settleIdle(run) {
  clearTimeout(run.idleTimer);
  if (run.current || run.input.unanswered || run.tasks.hasLive()) return;
  run.idleTimer = setTimeout(() => run.release(), run.lingering ? lingerSettleMs : 0);
}

async function runTurn(messageId, params) {
  const sessionId = params.sessionId;
  const prompt = params.prompt || "";
  const images = Array.isArray(params.images) ? params.images : [];
  if (!queryFn) {
    sdkUnavailable(messageId);
    return;
  }
  if (!sessionId || (!prompt && images.length === 0)) {
    emit({ id: messageId, error: { message: "Claude turn requires sessionId and prompt." } });
    return;
  }
  if (params.compactWindow != null &&
      (!Number.isInteger(params.compactWindow) || params.compactWindow < 100000 || params.compactWindow > 1000000)) {
    emit({ id: messageId, error: { message: "compactWindow must be null or an integer between 100000 and 1000000 tokens." } });
    return;
  }
  const existing = activeTurns.get(sessionId);
  if (existing?.current) {
    emit({ id: messageId, error: { message: "A Claude turn is already running in this conversation." } });
    return;
  }
  if (authenticationChangedWhileBusy(existing, params.authentication)) {
    emit({ id: messageId, error: { message: "Claude credential settings changed. Wait for this conversation's background tasks to finish before sending another turn." } });
    return;
  }
  if (!params.freshContext && existing?.tasks.hasLive() && existing.compactWindow !== (params.compactWindow || 0)) {
    emit({ id: messageId, error: { message: "Wait for background tasks to finish before changing compactWindow." } });
    return;
  }
  if (!params.freshContext && existing?.tasks.hasLive() && existing.cwd !== (params.cwd || process.cwd())) {
    emit({ id: messageId, error: { message: "Wait for background tasks to finish before switching worktrees." } });
    return;
  }
  if (!params.freshContext && existing?.input && existing.key === launchKey(params) && !existing.input.closed) {
    await continueRun(existing, messageId, params, prompt, images);
    return;
  }
  if (existing) {
    // A different launch cannot reuse the query; its background tasks end with it.
    existing.release();
    await existing.done;
  }
  await startRun(messageId, params, prompt, images);
}

// A query kept open for background tasks takes the next user turn, so the
// tasks keep running and the agent hears when they finish.
async function continueRun(run, messageId, params, prompt, images) {
  let message;
  try {
    message = await promptMessage(prompt, images);
  } catch {
    emit({ id: messageId, error: { message: "An attached image could not be read." } });
    return;
  }
  const q = run.query;
  const effort = typeof params.effort === "string" ? params.effort : "";
  run.permissionMode = reviewProfile(params) === "read" ? "plan"
    : reviewProfile(params) === "verify" ? "default" : permissionMode(params.permission);
  for (const change of [
    () => q.applyFlagSettings
      ? q.applyFlagSettings({ model: params.model || "sonnet", effortLevel: effort || null })
      : q.setModel?.(params.model || "sonnet"),
    () => q.setPermissionMode?.(run.permissionMode),
    () => (effort in thinkingTokens ? q.setMaxThinkingTokens?.(thinkingTokens[effort]) : undefined),
  ]) {
    try {
      await change();
    } catch {
      // The turn runs with the setting the query already has.
    }
  }
  if (run.current || run.input.closed) {
    emit({ id: messageId, error: { message: "The Claude turn has already finished." } });
    return;
  }
  run.tasks.forgetEnded();
  beginTurn(run, messageId);
  run.input.send(message);
}

async function startRun(messageId, params, prompt, images) {
  const sessionId = params.sessionId;
  // ADR 0067 and ADR 0114 §2: a review turn's profile, decided by this launch alone.
  const profile = reviewProfile(params);
  const mode = profile === "read" ? "plan" : profile === "verify" ? "default" : permissionMode(params.permission);
  const abortController = new AbortController();
  const stop = {};
  let release;
  const released = new Promise((resolve) => { release = () => resolve(stop); });
  let finished;
  const run = {
    sessionId,
    key: launchKey(params),
    compactWindow: params.compactWindow || 0,
    cwd: params.cwd || process.cwd(),
    permissionMode: mode,
    query: null,
    input: null,
    abortController,
    sdkSessionId: params.resume || "",
    current: null,
    lingering: false,
    idleTimer: null,
    release: () => release(),
    done: new Promise((resolve) => { finished = resolve; }),
    tasks: new BackgroundTasks((payload) => emitEvent(sessionId, "tasks.changed", payload)),
  };
  activeTurns.set(sessionId, run);
  const current = beginTurn(run, messageId);
  let grace = null;

  // Only the daemon supplies this map, rebuilt from local settings for every turn.
  const mcpServers = params.mcpServers || {};
  const browserInjected = Object.hasOwn(mcpServers, "ketplus_browser");
  // ADR 0027: the daemon injects this conversation's agent messaging server.
  const agentsInjected = Object.hasOwn(mcpServers, "ketplus_agents");
  // KetPlus Computer enforces per-app grants itself, so its tools need no prompt.
  const computerInjected = Object.hasOwn(mcpServers, "ketplus_computer");
  const reviewContext = { scratch: params.scratchDirectory || "", cwd: run.cwd, agentsInjected, browserInjected };
  const review = profile ? reviewOptions(profile, reviewContext) : null;
  const hostTools = review ? review.allowedTools : [
    ...(browserInjected ? ["mcp__ketplus_browser__*"] : []),
    ...(agentsInjected ? ["mcp__ketplus_agents__*"] : []),
    ...(computerInjected ? ["mcp__ketplus_computer__*"] : []),
  ];
  const readable = [...(Array.isArray(params.readableDirectories) ? params.readableDirectories : []),
    ...(review?.additionalDirectories || [])];
  const effort = typeof params.effort === "string" ? params.effort : "";
  try {
    run.authenticationRevision = params.authentication?.revision || "";
    run.authentication = { enabled: params.authentication?.enabled === true,
      budget_usd: params.authentication?.budget_usd || 0 };
    emitUsageEvent(sessionId, "usage.session", sessionUsage(run.authentication));
    run.input = new TurnInput(await promptMessage(prompt, images));
    if (params.resume) await ensureResumable(params.resume, run.cwd).catch(() => false);
    const authentication = authenticationOptions(params.authentication);
    const appended = [typeof params.browserInstructions === "string" ? params.browserInstructions : "",
      review?.instructions || ""].filter(Boolean).join("\n\n");
    const q = queryFn({
      prompt: run.input,
      options: {
        ...authentication,
        // A verify review's caches live in its scratch directory (ADR 0067).
        ...(review?.env ? { env: { ...(authentication.env || process.env), ...review.env } } : {}),
        cwd: run.cwd,
        mcpServers,
        // The daemon derives this from the same launch as mcpServers on every query,
        // including resume. Preserve Claude Code's system prompt and the user's input.
        ...(appended ? { systemPrompt: { type: "preset", preset: "claude_code", append: appended } } : {}),
        allowedTools: hostTools,
        model: params.model || "sonnet",
        // Per-launch CLI flag: do not replace the SDK's environment or model capacity.
        ...(params.compactWindow ? { extraArgs: { autocompact: String(params.compactWindow) } } : {}),
        ...(effort ? { effort } : {}),
        ...(effort in thinkingTokens ? { maxThinkingTokens: thinkingTokens[effort] } : {}),
        // A handoff transcript from another provider, readable without a permission prompt.
        ...(readable.length > 0 ? { additionalDirectories: readable } : {}),
        ...(review?.sandbox ? { sandbox: review.sandbox } : {}),
        ...(review?.hooks ? { hooks: review.hooks } : {}),
        permissionMode: mode,
        allowDangerouslySkipPermissions: !profile && mode === "bypassPermissions",
        includePartialMessages: true,
        persistSession: true,
        resume: params.resume || undefined,
        abortController,
        canUseTool: async (tool, toolInput) => {
          const requestId = `perm-${sessionId}-${Date.now()}-${Math.random().toString(16).slice(2)}`;
          const input = toolInput && typeof toolInput === "object" ? toolInput : {};
          // Decided without a prompt, so nobody can grant a reviewer more than its profile.
          if (profile) return reviewDecision(profile, tool, input, reviewContext);
          if ((browserInjected && tool.startsWith("mcp__ketplus_browser__")) ||
              (computerInjected && tool.startsWith("mcp__ketplus_computer__")))
            return { behavior: "allow", updatedInput: input };
          if (agentsInjected && tool.startsWith("mcp__ketplus_agents__"))
            return { behavior: "allow", updatedInput: input };
          // Claude Code still routes some commands here in bypassPermissions (its
          // safety checks). Full access means no prompts; questions still need the user.
          if (run.permissionMode === "bypassPermissions" && tool !== "AskUserQuestion")
            return { behavior: "allow", updatedInput: input };
          return await new Promise((resolve) => {
            pendingPermissions.set(String(requestId), { resolve, input, sessionId, tool });
            emit({
              id: requestId,
              method: tool === "AskUserQuestion" ? "question.request" : "permission.request",
              params: {
                sessionId,
                tool,
                input,
                itemId: requestId,
                questions: tool === "AskUserQuestion" ? (input.questions || []).map((q, i) => ({
                  ...q, id: `question-${i}`, isOther: true,
                  selection: q.multiSelect ? "multiple" : "single",
                })) : undefined,
              },
            });
          });
        },
      },
    });
    run.query = q;
    if (current.interrupted) await q.interrupt?.();

    const messages = q[Symbol.asyncIterator]();
    const noteUsage = liveUsage(sessionId, q);
    while (true) {
      const next = await Promise.race([messages.next(), released]);
      if (next === stop || next.done) break;
      const message = next.value;
      if (!message || typeof message !== "object") continue;
      run.input.observe(message);
      if (startsModelRun(message) && !run.input.belongsToOtherPrompt(message)) {
        clearTimeout(grace);
        grace = null;
      }
      if (message.type === "system" && message.subtype === "init") {
        emitEvent(sessionId, "provider.authentication", { source: message.apiKeySource });
      }
      if (message.type === "result" && !message.is_error && Number.isFinite(message.total_cost_usd) && message.total_cost_usd >= 0)
        emitUsageEvent(sessionId, "usage.session", sessionUsage(run.authentication, message, true));
      if (message.type === "result" && Number.isFinite(message.total_cost_usd)) {
        emitEvent(sessionId, "provider.authentication", { estimated_cost_usd: message.total_cost_usd });
      }
      if (typeof message.session_id === "string" && message.session_id && !run.sdkSessionId) {
        run.sdkSessionId = message.session_id;
        emitEvent(sessionId, "thread.started", { thread_id: run.sdkSessionId });
      }
      if (run.tasks.accept(message) && run.lingering) settleIdle(run);
      if (message.type === "assistant" && message.message?.model === "<synthetic>" &&
          !message.isApiErrorMessage) continue;
      // Probe while the query is alive, then check the pending prompt again:
      // steering can arrive while this asynchronous control request runs.
      if (message.type === "result" && run.current) {
        if (!run.input.resultPending(message)) {
          clearTimeout(grace);
          grace = null;
        }
        await emitUsage(sessionId, q);
      }
      if (message.type === "result" && run.input.resultPending(message)) {
        // Keep the initial prompt (or steer) alive across unrelated resume
        // results. Never publish their text as the answer to this user turn.
        if (!grace) grace = setTimeout(() => {
          if (run.current) run.current.result = null;
          completeTurn(run);
          run.release();
        }, steerGraceMs);
        continue;
      }
      // The agent answering a finished background task is a turn of its own.
      if (!run.current && startsModelRun(message)) beginTurn(run);
      run.current?.transcript.accept(message);
      if (run.current) noteUsage(message);
      if (message.type !== "result") continue;
      clearTimeout(grace);
      grace = null;
      run.input.answered();
      if (run.current) run.current.result = message;
      // A settled result is terminal for the daemon turn. Closing prompt
      // input alone does not end SDK output, so without background tasks the
      // query ends here; with them it stays open for their notifications.
      completeTurn(run);
      if (!run.tasks.hasLive()) break;
      run.lingering = true;
    }
    completeTurn(run);
  } catch (error) {
    if (!run.current?.interrupted) {
      emitEvent(sessionId, "runtime.error", {
        message: error instanceof Error ? error.message : String(error),
      });
    }
    completeTurn(run, true);
  } finally {
    clearTimeout(grace);
    clearTimeout(run.idleTimer);
    run.input?.close();
    denyPendingPermissions(sessionId, "Turn ended.");
    // Closing the query ends its process, and every task it still runs.
    run.tasks.orphan("session_closed");
    run.tasks.flush();
    run.tasks.dispose();
    endedOutputs.set(sessionId, run.tasks);
    // Each query owns its MCP workers; never leave an old browser policy running.
    try { run.query?.close?.(); } finally {
      if (activeTurns.get(sessionId) === run) activeTurns.delete(sessionId);
      finished();
    }
  }
}

// The last 8 KiB of an ended shell task's output file, the tail the SDK reads.
async function outputTail(file) {
  const handle = await open(file, "r");
  try {
    const { size } = await handle.stat();
    const length = Math.min(size, taskOutputTailBytes);
    const buffer = Buffer.alloc(length);
    await handle.read(buffer, 0, length, size - length);
    // A cut multi-byte character decodes as a replacement mark; drop it.
    const output = buffer.toString("utf8").replace(/^�+/, "");
    return { output, total_bytes: size, truncated: size > length };
  } finally {
    await handle.close();
  }
}

async function handle(message) {
  const method = message.method;
  const messageId = message.id;
  const params = message.params || {};
  if (messageId !== undefined && pendingPermissions.has(String(messageId))) {
    const { resolve, input, tool } = pendingPermissions.get(String(messageId));
    pendingPermissions.delete(String(messageId));
    if (tool === "AskUserQuestion" && message.result?.answers && Object.keys(message.result.answers).length) {
      const answers = Object.fromEntries(Object.entries(message.result.answers)
        .map(([id, answer]) => [input.questions[Number(id.replace("question-", ""))]?.question || id, (answer.answers || []).join(", ")]));
      resolve({ behavior: "allow", updatedInput: { ...input, answers } });
      return;
    }
    const allow = message.result?.allow === true;
    // The SDK runs the tool with updatedInput, so an approval passes the input it asked about.
    resolve(
      allow
        ? { behavior: "allow", updatedInput: input }
        : { behavior: "deny", message: message.error?.message || "The user declined this tool." },
    );
    return;
  }
  if (method === "initialize") {
    emit({ id: messageId, result: { sdk: Boolean(queryFn), error: sdkError } });
    return;
  }
  if (method === "model/list") {
    emit({ id: messageId, result: await catalog() });
    return;
  }
  if (method === "turn/start") {
    runTurn(messageId, params);
    return;
  }
  if (method === "thread/fork") {
    // Copies the SDK transcript without calling the model or interrupting an active source turn.
    const threadId = typeof params.threadId === "string" ? params.threadId : "";
    if (!forkFn) emit({ id: messageId, error: { message: "This Claude Agent SDK cannot fork sessions." } });
    else if (!threadId) emit({ id: messageId, error: { message: "Forking requires a threadId." } });
    else {
      const forked = await forkFn(threadId, params.cwd ? { dir: params.cwd } : {});
      emit({ id: messageId, result: { thread: { id: forked.sessionId } } });
    }
    return;
  }
  if (method === "thread/delete") {
    const threadId = typeof params.threadId === "string" ? params.threadId : "";
    if (!deleteFn || !threadId) throw new Error("This Claude SDK cannot delete the requested session.");
    await deleteSessionCopies(deleteFn, threadId);
    emit({ id: messageId, result: { deleted: true } });
    return;
  }
  if (method === "turn/steer") {
    const active = activeTurns.get(params.sessionId);
    const text = typeof params.prompt === "string" ? params.prompt.trim() : "";
    if (!text) {
      emit({ id: messageId, error: { message: "Steering requires a prompt." } });
      return;
    }
    // A steer carries its images the way a turn's first message does.
    let message;
    try {
      message = await promptMessage(text, params.images);
    } catch {
      emit({ id: messageId, error: { message: "An attached image could not be read." } });
      return;
    }
    if (!active?.current || active.current.interrupted || !active.input?.steer(message.message.content))
      emit({ id: messageId, error: { message: "The Claude turn has already finished." } });
    else emit({ id: messageId, result: {} });
    return;
  }
  if (method === "turn/interrupt") {
    const active = activeTurns.get(params.sessionId);
    // A query kept open between turns has no turn to stop; its tasks keep running.
    if (active?.current) {
      active.current.interrupted = true;
      denyPendingPermissions(params.sessionId, "Stopped by user.", true);
      // Stopping the turn leaves background tasks running, as the Claude Code
      // terminal does; aborting would end the query and every task with it.
      if (active.tasks.hasLive() && typeof active.query?.interrupt === "function")
        active.query.interrupt().catch(() => active.abortController.abort());
      else active.abortController.abort();
    }
    // The turn ends through its own loop, which reports it as interrupted.
    emit({ id: messageId, result: {} });
    return;
  }
  if (method === "task/stop") {
    const active = activeTurns.get(params.sessionId);
    const taskId = typeof params.taskId === "string" ? params.taskId : "";
    if (!active?.query || !active.tasks.has(taskId) || typeof active.query.stopTask !== "function") {
      emit({ id: messageId, error: { message: "This background task is no longer running." } });
      return;
    }
    active.tasks.markStopping(taskId);
    try {
      // The SDK confirms with task_notification(status: stopped).
      await active.query.stopTask(taskId);
      emit({ id: messageId, result: {} });
    } catch (error) {
      active.tasks.restore(taskId);
      throw error;
    }
    return;
  }
  if (method === "task/output") {
    const active = activeTurns.get(params.sessionId);
    const taskId = typeof params.taskId === "string" ? params.taskId : "";
    // Undocumented in the SDK typings; an older build falls back to the file.
    if (active?.query && active.tasks.has(taskId) && typeof active.query.getTaskOutput === "function") {
      try {
        const tail = await active.query.getTaskOutput(taskId);
        emit({
          id: messageId,
          result: {
            output: typeof tail?.output === "string" ? tail.output : "",
            total_bytes: Number(tail?.total_bytes) || 0,
            truncated: tail?.truncated === true,
          },
        });
        return;
      } catch {
        // An ended task may still have its output file.
      }
    }
    const file = active?.tasks.outputFile(taskId) || endedOutputs.get(params.sessionId)?.outputFile(taskId);
    if (!file) {
      emit({ id: messageId, error: { message: "This task's output is not available." } });
      return;
    }
    emit({ id: messageId, result: await outputTail(file) });
    return;
  }
  if (messageId !== undefined) {
    emit({ id: messageId, error: { message: `Unknown Claude sidecar method: ${method}` } });
  }
}

const rl = createInterface({ input, crlfDelay: Infinity });
rl.on("line", (line) => {
  const trimmed = line.trim();
  if (!trimmed) return;
  let message;
  try {
    message = JSON.parse(trimmed);
  } catch (error) {
    stderr.write(`ketplus-claude-sidecar: invalid JSON: ${error}\n`);
    return;
  }
  handle(message).catch((error) => {
    const messageId = message?.id;
    if (messageId !== undefined) {
      emit({
        id: messageId,
        error: { message: error instanceof Error ? error.message : String(error) },
      });
    }
  });
});
