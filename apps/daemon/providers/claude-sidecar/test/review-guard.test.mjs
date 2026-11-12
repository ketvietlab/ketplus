import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { createInterface } from "node:readline";
import test from "node:test";

test("a review turn stays in plan mode and refuses every write without a prompt", { timeout: 10000 }, async (t) => {
  // Run the real sidecar against a fake SDK; never import or call an authenticated provider.
  const root = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-claude-review-"));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  for (const file of ["authentication.mjs", "index.mjs", "transcript.mjs", "input.mjs", "tasks.mjs", "resume.mjs", "session-delete.mjs", "session-usage.mjs", "review-guard.mjs"])
    await fs.copyFile(new URL(`../${file}`, import.meta.url), path.join(root, file));
  const sdk = path.join(root, "node_modules/@anthropic-ai/claude-agent-sdk");
  await fs.mkdir(sdk, { recursive: true });
  await fs.writeFile(path.join(sdk, "package.json"), JSON.stringify({ type: "module", exports: "./index.mjs" }));
  await fs.writeFile(path.join(sdk, "index.mjs"), `
    export function query({ prompt: input, options }) {
      return {
        async *[Symbol.asyncIterator]() {
          await input[Symbol.asyncIterator]().next();
          const decisions = {};
          for (const tool of ["ExitPlanMode", "Edit", "Write", "Bash", "mcp__github__merge_pull_request",
                              "mcp__ketplus_browser__browser_act", "mcp__ketplus_agents__agents_send"])
            decisions[tool] = (await options.canUseTool(tool, {})).behavior;
          yield { type: "result", session_id: "thread-1", result: JSON.stringify({
            permissionMode: options.permissionMode,
            skip: options.allowDangerouslySkipPermissions,
            allowedTools: options.allowedTools, decisions
          }) };
        },
        close() { console.log(JSON.stringify({ method: "test.closed" })); }
      };
    }
  `);
  const child = spawn(process.execPath, [path.join(root, "index.mjs")], { stdio: ["pipe", "pipe", "pipe"] });
  const exited = once(child, "exit");
  t.after(async () => { child.kill(); await exited; });
  let stderr = "";
  child.stderr.on("data", (data) => { stderr += data; });
  const iterator = createInterface({ input: child.stdout })[Symbol.asyncIterator]();
  const send = (message) => child.stdin.write(`${JSON.stringify(message)}\n`);
  // Even a conversation in full access reviews another's turn read-only.
  send({ id: 1, method: "turn/start", params: {
    sessionId: "reviewer", prompt: "Review", permission: "danger-full-access", reviewGuard: true,
    mcpServers: { ketplus_agents: { command: "agents" }, ketplus_browser: { command: "browser" } } } });
  const prompts = [];
  let answer;
  for (;;) {
    const { value, done } = await iterator.next();
    assert.equal(done, false, stderr);
    const message = JSON.parse(value);
    if (message.method === "permission.request" || message.method === "question.request")
      prompts.push(message.params.tool);
    if (message.params?.type === "item.completed") answer = JSON.parse(message.params.payload.item.text);
    if (message.method === "test.closed") break;
  }
  assert.deepEqual(prompts, []);
  assert.equal(answer.permissionMode, "plan");
  assert.equal(answer.skip, false);
  assert.deepEqual(answer.allowedTools, ["mcp__ketplus_agents__*"]);
  assert.deepEqual(answer.decisions, {
    ExitPlanMode: "deny", Edit: "deny", Write: "deny", Bash: "deny",
    mcp__github__merge_pull_request: "deny", mcp__ketplus_browser__browser_act: "deny",
    mcp__ketplus_agents__agents_send: "allow",
  });
  assert.equal(stderr, "");
});
