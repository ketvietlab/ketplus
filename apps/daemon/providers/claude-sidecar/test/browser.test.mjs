import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { createInterface } from "node:readline";
import test from "node:test";

test("sidecar supplies host MCP servers on resume and scopes auto-approval to injected tools", { timeout: 10000 }, async (t) => {
  // Run the real sidecar against a fake SDK; never import or call an authenticated provider.
  const root = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-claude-browser-"));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  for (const file of ["authentication.mjs", "index.mjs", "transcript.mjs", "input.mjs", "tasks.mjs", "resume.mjs", "session-delete.mjs", "session-usage.mjs", "review-guard.mjs", "background-policy.mjs", "process-barrier.mjs"])
    await fs.copyFile(new URL(`../${file}`, import.meta.url), path.join(root, file));
  for (const [directory, name] of [
    ["", "project-build"],
    ["apps/browser", "ketplus-browser"],
    ["apps/browser", "browser-project"],
    ["apps/browser", "enterprise-grade"],
    ["", "ket-design-system"],
    ["node_modules/fixture", "ignored-dependency"],
    ["apps/build", "ignored-build"],
    ["apps/build-local", "ignored-build-variant"],
    ["apps/vendor", "ignored-vendor"],
    [Array(9).fill("nested").join("/"), "ignored-depth"],
    [".git", "ignored-git"],
  ]) {
    const projectSkill = path.join(root, directory, ".claude", "skills", name);
    await fs.mkdir(projectSkill, { recursive: true });
    await fs.writeFile(path.join(projectSkill, "SKILL.md"), `---\nname: ${name}\n---\n`);
  }
  // Preserve T-4 discovery for a workspace that links its Claude settings.
  await fs.rename(path.join(root, ".claude"), path.join(root, ".linked-claude"));
  await fs.symlink(path.join(root, ".linked-claude"), path.join(root, ".claude"), "junction");
  // Fill the directory budget with shallow entries before a nested candidate.
  const budgetRoot = path.join(root, "budget");
  await fs.mkdir(budgetRoot);
  for (let offset = 0; offset < 4095; offset += 128) {
    await Promise.all(Array.from({ length: Math.min(128, 4095 - offset) }, (_, index) =>
      fs.mkdir(path.join(budgetRoot, String(offset + index).padStart(4, "0")))));
  }
  const beyondBudget = path.join(budgetRoot, "zz-over-budget", ".claude", "skills", "ignored-budget");
  await fs.mkdir(beyondBudget, { recursive: true });
  await fs.writeFile(path.join(beyondBudget, "SKILL.md"), "---\nname: ignored-budget\n---\n");
  const sdk = path.join(root, "node_modules/@anthropic-ai/claude-agent-sdk");
  await fs.mkdir(sdk, { recursive: true });
  await fs.writeFile(path.join(sdk, "package.json"), JSON.stringify({ type: "module", exports: "./index.mjs" }));
  await fs.writeFile(path.join(sdk, "index.mjs"), `
    export function query({ prompt: input, options }) {
      return {
        async *[Symbol.asyncIterator]() {
          const prompt = (await input[Symbol.asyncIterator]().next()).value.message.content;
          const browser = await options.canUseTool("mcp__ketplus_browser__browser_open", { url: "http://localhost" });
          const other = await options.canUseTool("mcp__other__browser_open", {});
          const similar = await options.canUseTool("mcp__ketplus_browser_extra__browser_open", {});
          const computer = await options.canUseTool("mcp__ketplus_computer__screenshot", {});
          yield { type: "result", session_id: "thread-1", result: JSON.stringify({
            mcpServers: options.mcpServers, allowedTools: options.allowedTools,
            skills: options.skills, settingSources: options.settingSources,
            plugins: options.plugins, persistSession: options.persistSession,
            prompt, systemPrompt: options.systemPrompt,
            resume: options.resume, permissionMode: options.permissionMode, browser, other, similar, computer
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
  const lines = createInterface({ input: child.stdout });
  const iterator = lines[Symbol.asyncIterator]();
  const send = (message) => child.stdin.write(`${JSON.stringify(message)}\n`);
  const server = { type: "stdio", command: "/test/node", args: ["/test/mcp.mjs"], env: { KETPLUS_BROWSER_ALLOWED_ORIGINS: "loopback" } };
  const computerServer = { type: "stdio", command: "/test/ketplus-computer-mcp", args: [],
    env: { KETPLUS_COMPUTER_CONVERSATION: "session-1", KETPLUS_COMPUTER_TOKEN: "t" } };
  const teamSkills = ["ketplusdev", "ketvietdev", "ketplus-browser"];
  const cases = [
    { id: 1, browser: true, computer: true },
    { id: 2, browser: false, computer: false, teamRole: "leader" },
    { id: 3, browser: false, computer: true, teamRole: "reviewer" },
    { id: 4, browser: false, computer: false, teamRole: "developer" },
    { id: 5, browser: false, computer: false, teamRole: "leader", cwd: budgetRoot },
  ];
  for (const { id, browser: enabled, computer, teamRole, cwd = root } of cases) {
    const mcpServers = {
      ...(enabled ? { ketplus_browser: server } : {}),
      ...(computer ? { ketplus_computer: computerServer } : {}),
    };
    send({ id, method: "turn/start", params: {
      sessionId: "session-1", prompt: "Inspect", resume: "thread-1", permission: "workspace-write",
      mcpServers, teamRole, cwd,
      browserInstructions: enabled ? "Use ketplus_browser." : "KetPlus browser is unavailable; do not fall back.",
    } });
    const approvals = [];
    let answer;
    for (;;) {
      const { value, done } = await iterator.next();
      assert.equal(done, false, stderr);
      const message = JSON.parse(value);
      if (message.method === "permission.request") {
        approvals.push(message.params.tool);
        send({ id: message.id, result: { allow: false } });
      }
      if (message.params?.type === "item.completed") answer = JSON.parse(message.params.payload.item.text);
      if (message.method === "test.closed") break;
    }
    assert.deepEqual(answer.mcpServers, mcpServers);
    assert.deepEqual(answer.allowedTools, [
      ...(enabled ? ["mcp__ketplus_browser__*"] : []),
      ...(computer ? ["mcp__ketplus_computer__*"] : []),
    ]);
    assert.equal(answer.prompt, "Inspect");
    assert.deepEqual(answer.systemPrompt, { type: "preset", preset: "claude_code",
      append: enabled ? "Use ketplus_browser." : "KetPlus browser is unavailable; do not fall back." });
    assert.deepEqual(answer.skills, teamRole ? [
      ...teamSkills,
      ...(teamRole === "leader" ? ["enterprise-grade", "ket-design-system"] : []),
      ...(cwd === root ? ["project-build", "browser-project"] : []),
    ] : undefined);
    assert.equal(answer.settingSources, undefined);
    assert.equal(answer.plugins, undefined);
    assert.equal(answer.persistSession, true);
    assert.equal(answer.resume, "thread-1");
    assert.equal(answer.permissionMode, "acceptEdits");
    assert.equal(answer.browser.behavior, enabled ? "allow" : "deny");
    assert.equal(answer.other.behavior, "deny");
    assert.equal(answer.similar.behavior, "deny");
    assert.equal(answer.computer.behavior, computer ? "allow" : "deny");
    assert.equal(approvals.length, 2 + (enabled ? 0 : 1) + (computer ? 0 : 1));
    if (id === 2) {
      const uncached = path.join(root, ".linked-claude", "skills", "added-after-scan");
      await fs.mkdir(uncached);
      await fs.writeFile(path.join(uncached, "SKILL.md"), "---\nname: added-after-scan\n---\n");
    }
  }
  assert.equal(stderr, "");
});
