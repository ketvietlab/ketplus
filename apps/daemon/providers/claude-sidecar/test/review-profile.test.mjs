// ADR 0114 §2: a team's review round runs under the verify profile; ADR 0067's turn review keeps
// the read profile. Runs the real sidecar against a fake SDK; never calls an authenticated provider.
import assert from "node:assert/strict";
import { execFileSync, spawn } from "node:child_process";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { createInterface } from "node:readline";
import test from "node:test";

const sidecarFiles = ["authentication.mjs", "index.mjs", "transcript.mjs", "input.mjs", "tasks.mjs",
  "resume.mjs", "session-delete.mjs", "session-usage.mjs", "review-guard.mjs"];

// Asks the hook and canUseTool about every attempt in KETPLUS_FAKE_ATTEMPTS and, like Claude
// Code, runs what both allow, so a wrong "allow" really changes the files under test. A
// background task keeps the query open after its result, as a live session would be.
const fakeSdk = `
  import { spawnSync } from "node:child_process";
  import fs from "node:fs";
  export function query({ prompt, options }) {
    console.log(JSON.stringify({ method: "test.query", params: {
      permissionMode: options.permissionMode, skip: options.allowDangerouslySkipPermissions,
      allowedTools: options.allowedTools, sandbox: options.sandbox || null,
      hooks: Object.keys(options.hooks || {}), additionalDirectories: options.additionalDirectories || [],
      resume: options.resume || "" } }));
    const input = prompt[Symbol.asyncIterator]();
    let serial = 0;
    async function attempt() {
      const attempts = JSON.parse(fs.readFileSync(process.env.KETPLUS_FAKE_ATTEMPTS, "utf8"));
      const decisions = [];
      for (const { tool, input: toolInput } of attempts) {
        const hook = options.hooks?.PreToolUse?.[0];
        const hooked = hook ? (await hook.hooks[0]({ hook_event_name: "PreToolUse", tool_name: tool, tool_input: toolInput }))
          .hookSpecificOutput : null;
        const asked = await options.canUseTool(tool, toolInput);
        const allowed = (!hooked || hooked.permissionDecision === "allow") && asked.behavior === "allow";
        if (allowed && tool === "Bash")
          spawnSync("/bin/sh", ["-c", toolInput.command], { cwd: options.cwd, stdio: "ignore" });
        if (allowed && (tool === "Write" || tool === "Edit" || tool === "NotebookEdit"))
          fs.writeFileSync(toolInput.file_path || toolInput.notebook_path, "changed\\n");
        decisions.push({ tool, input: toolInput, hook: hooked?.permissionDecision || "", behavior: asked.behavior,
          message: asked.message || hooked?.permissionDecisionReason || "" });
      }
      return decisions;
    }
    const gen = (async function* () {
      for (;;) {
        const next = await input.next();
        if (next.done) return;
        yield { type: "system", subtype: "init", session_id: "thread-1" };
        yield { type: "stream_event", parent_tool_use_id: null, event: { type: "message_start", message: { id: "m" + ++serial } } };
        if (serial === 1) {
          yield { type: "system", subtype: "task_started", task_id: "t1", tool_use_id: "tu1", description: "Watch",
            task_type: "local_bash", is_backgrounded: true };
          yield { type: "system", subtype: "background_tasks_changed",
            tasks: [{ task_id: "t1", task_type: "local_bash", description: "Watch" }] };
        }
        yield { type: "result", session_id: "thread-1", is_error: false,
          result: JSON.stringify({ decisions: await attempt() }) };
      }
    })();
    gen.setPermissionMode = async (mode) => console.log(JSON.stringify({ method: "test.mode", params: { mode } }));
    gen.close = () => console.log(JSON.stringify({ method: "test.closed" }));
    return gen;
  }
`;

async function fixture(t, attempts = []) {
  const root = await fs.realpath(await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-review-profile-")));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  const sidecar = path.join(root, "sidecar");
  await fs.mkdir(sidecar);
  for (const file of sidecarFiles) await fs.copyFile(new URL(`../${file}`, import.meta.url), path.join(sidecar, file));
  const sdk = path.join(sidecar, "node_modules/@anthropic-ai/claude-agent-sdk");
  await fs.mkdir(sdk, { recursive: true });
  await fs.writeFile(path.join(sdk, "package.json"), JSON.stringify({ type: "module", exports: "./index.mjs" }));
  await fs.writeFile(path.join(sdk, "index.mjs"), fakeSdk);
  const attemptsFile = path.join(root, "attempts.json");
  const setAttempts = (list) => fs.writeFile(attemptsFile, JSON.stringify(list));
  await setAttempts(attempts);
  const child = spawn(process.execPath, [path.join(sidecar, "index.mjs")],
    { stdio: ["pipe", "pipe", "pipe"], env: { ...process.env, KETPLUS_FAKE_ATTEMPTS: attemptsFile } });
  const exited = new Promise((resolve) => child.once("exit", resolve));
  t.after(async () => { child.kill(); await exited; });
  const events = [];
  let stderr = "";
  child.stderr.on("data", (data) => { stderr += data; });
  createInterface({ input: child.stdout }).on("line", (line) => events.push(JSON.parse(line)));
  let serial = 0;
  // One turn: its query options, any mode change it made, and the tool decisions it saw.
  const turn = async (params) => {
    const from = events.length;
    child.stdin.write(`${JSON.stringify({ id: ++serial, method: "turn/start", params: {
      sessionId: "reviewer", prompt: "Review", ...params } })}\n`);
    for (let i = 0; i < 500; ++i) {
      const done = events.slice(from).find((event) => event.params?.type === "turn.completed");
      if (done) {
        const later = events.slice(from);
        const answer = later.filter((event) => event.params?.type === "item.completed").at(-1);
        return {
          query: later.find((event) => event.method === "test.query")?.params || null,
          mode: later.find((event) => event.method === "test.mode")?.params.mode || "",
          closed: later.some((event) => event.method === "test.closed"),
          prompts: later.filter((event) => event.method === "permission.request" || event.method === "question.request"),
          decisions: JSON.parse(answer.params.payload.item.text).decisions,
        };
      }
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    assert.fail(`The turn did not complete: ${stderr} ${JSON.stringify(events.slice(from))}`);
  };
  return { root, turn, setAttempts, stderr: () => stderr };
}

const servers = { ketplus_agents: { command: "agents" }, ketplus_browser: { command: "browser" } };
const git = (cwd, ...args) => execFileSync("git", args, { cwd, encoding: "utf8" });

// A checkout with a tracked file, a second branch and a linked worktree, like a task's.
async function checkout(root) {
  const repo = path.join(root, "repo");
  await fs.mkdir(repo);
  git(repo, "init", "-q", "-b", "main");
  git(repo, "config", "user.email", "test@example.invalid");
  git(repo, "config", "user.name", "Test");
  await fs.writeFile(path.join(repo, "tracked.txt"), "original\n");
  git(repo, "add", "tracked.txt");
  git(repo, "commit", "-q", "-m", "first");
  git(repo, "branch", "other");
  git(repo, "worktree", "add", "-q", path.join(root, "linked"), "other");
  return repo;
}

const state = (repo) => ({
  status: git(repo, "status", "--porcelain"),
  refs: git(repo, "for-each-ref", "--format=%(refname) %(objectname)"),
  worktrees: git(repo, "worktree", "list", "--porcelain"),
  tracked: git(repo, "show", "HEAD:tracked.txt"),
});

test("a team's review round verifies without a prompt: reads, runs commands, writes its scratch directory", { timeout: 20000 }, async (t) => {
  const f = await fixture(t);
  const repo = await checkout(f.root);
  const scratch = path.join(f.root, "scratch");
  await fs.mkdir(scratch);
  await f.setAttempts([
    { tool: "Read", input: { file_path: path.join(repo, "tracked.txt") } },
    { tool: "Grep", input: { pattern: "original" } },
    { tool: "Bash", input: { command: "git status --short && git log -1 --oneline && git diff --stat" } },
    { tool: "Bash", input: { command: `printf built > ${path.join(scratch, "build.log")}` } },
    { tool: "Write", input: { file_path: path.join(scratch, "notes.md"), content: "notes" } },
    { tool: "Edit", input: { file_path: path.join(scratch, "notes.md"), old_string: "notes", new_string: "more" } },
    { tool: "mcp__ketplus_agents__agents_send", input: { to: "1234", message: "Verdict: no findings" } },
    { tool: "mcp__ketplus_browser__browser_capture", input: {} },
  ]);
  const result = await f.turn({ cwd: repo, permission: "danger-full-access", reviewGuard: true,
    reviewProfile: "verify", scratchDirectory: scratch, mcpServers: servers });
  assert.deepEqual(result.prompts, []);
  assert.equal(result.query.permissionMode, "default");
  assert.equal(result.query.skip, false);
  assert.deepEqual(result.query.allowedTools, ["mcp__ketplus_browser__*", "mcp__ketplus_agents__*"]);
  assert.deepEqual(result.query.hooks, ["PreToolUse"]);
  assert.ok(result.query.additionalDirectories.includes(scratch));
  // The provider's sandbox bounds what commands write, and fails closed.
  assert.equal(result.query.sandbox.enabled, true);
  assert.equal(result.query.sandbox.failIfUnavailable, true);
  assert.equal(result.query.sandbox.allowUnsandboxedCommands, false);
  assert.equal(result.query.sandbox.autoAllowBashIfSandboxed, false);
  assert.ok(result.query.sandbox.filesystem.allowWrite.includes(scratch));
  // The reviewed checkout stays unwritable even where it lies inside the temporary directory.
  assert.ok(!result.query.sandbox.filesystem.allowWrite.includes(repo));
  assert.ok(result.query.sandbox.filesystem.denyWrite.includes(repo));
  for (const decision of result.decisions) {
    assert.equal(decision.behavior, "allow", `${decision.tool}: ${decision.message}`);
    assert.equal(decision.hook, "allow", decision.tool);
  }
  assert.equal(await fs.readFile(path.join(scratch, "build.log"), "utf8"), "built");
  assert.equal(await fs.readFile(path.join(scratch, "notes.md"), "utf8"), "changed\n");
  assert.equal(f.stderr(), "");
});

test("a team's review round cannot change, commit or publish the work", { timeout: 20000 }, async (t) => {
  const f = await fixture(t);
  const repo = await checkout(f.root);
  const scratch = path.join(f.root, "scratch");
  await fs.mkdir(scratch);
  await fs.symlink(repo, path.join(scratch, "escape"));
  const before = state(repo);
  const bash = (command) => ({ tool: "Bash", input: { command } });
  const refused = [
    { tool: "Write", input: { file_path: path.join(repo, "tracked.txt"), content: "x" } },
    { tool: "Edit", input: { file_path: "tracked.txt", old_string: "original", new_string: "x" } },
    { tool: "NotebookEdit", input: { notebook_path: path.join(repo, "book.ipynb"), new_source: "x" } },
    { tool: "Write", input: { file_path: path.join(scratch, "escape", "tracked.txt"), content: "x" } },
    { tool: "Write", input: { file_path: path.join(scratch, "..", "repo", "new.txt"), content: "x" } },
    bash("git commit -qam changed --allow-empty"),
    bash("git -C . push origin HEAD"),
    bash("git merge other"),
    bash("git rebase other"),
    bash("git reset --hard HEAD"),
    bash("git switch other"),
    bash("git checkout -q other"),
    bash(`git worktree remove ${path.join(f.root, "linked")}`),
    bash("git branch -D other"),
    bash("cd . && GIT_TRACE=0 git commit --allow-empty -qm sneaky"),
    bash("echo ok; (git tag v1)"),
    bash("gh pr merge 7 --merge"),
    bash("gh pr review 7 --approve"),
    bash("gh pr close 7"),
    bash("gh api -X PUT repos/owner/repo/pulls/7/merge"),
    { tool: "Bash", input: { command: "echo outside", dangerouslyDisableSandbox: true } },
    { tool: "ExitPlanMode", input: { plan: "Do it" } },
    { tool: "EnterPlanMode", input: {} },
    { tool: "AskUserQuestion", input: { questions: [] } },
    { tool: "mcp__github__merge_pull_request", input: {} },
    { tool: "Agent", input: { prompt: "commit it" } },
  ];
  await f.setAttempts(refused);
  const result = await f.turn({ cwd: repo, permission: "danger-full-access", reviewGuard: true,
    reviewProfile: "verify", scratchDirectory: scratch, mcpServers: servers });
  assert.deepEqual(result.prompts, []);
  assert.equal(result.decisions.length, refused.length);
  for (const decision of result.decisions) {
    assert.equal(decision.behavior, "deny", `${decision.tool} ${JSON.stringify(decision.input)}`);
    assert.equal(decision.hook, "deny", decision.tool);
    assert.match(decision.message, /^A review cannot /, decision.tool);
  }
  assert.deepEqual(state(repo), before);
  assert.equal(await fs.readFile(path.join(repo, "tracked.txt"), "utf8"), "original\n");
  assert.equal(f.stderr(), "");
});

test("a turn review outside a team keeps the read profile", { timeout: 20000 }, async (t) => {
  const f = await fixture(t, [
    { tool: "Bash", input: { command: "echo hi" } },
    { tool: "Write", input: { file_path: "/nonexistent/x", content: "x" } },
    { tool: "mcp__ketplus_browser__browser_capture", input: {} },
    { tool: "mcp__ketplus_agents__agents_send", input: {} },
  ]);
  // No profile, or a verify profile without a scratch directory, is read-only.
  for (const params of [{}, { reviewProfile: "read" }, { reviewProfile: "verify" }]) {
    const result = await f.turn({ cwd: f.root, permission: "danger-full-access", reviewGuard: true, freshContext: true,
      mcpServers: servers, ...params });
    assert.equal(result.query.permissionMode, "plan");
    assert.equal(result.query.sandbox, null);
    assert.deepEqual(result.query.hooks, []);
    assert.deepEqual(result.query.allowedTools, ["mcp__ketplus_agents__*"]);
    assert.deepEqual(result.decisions.map((decision) => decision.behavior), ["deny", "deny", "deny", "allow"]);
    assert.equal(result.decisions[0].message, "A review is read-only: reply with your findings instead.");
  }
});

// Criterion 6: each turn's profile comes from its own launch, whatever the session ran before.
for (const [name, order] of [
  ["an unguarded turn first and review turns after", ["none", "verify", "verify", "read", "none"]],
  ["review turns first and an unguarded turn after", ["verify", "read", "none", "none", "verify"]],
]) {
  test(`a live session keeps no earlier profile: ${name}`, { timeout: 20000 }, async (t) => {
    const f = await fixture(t);
    await f.setAttempts([{ tool: "Write", input: { file_path: path.join(f.root, "free.txt"), content: "x" } }]);
    const scratch = path.join(f.root, "scratch");
    await fs.mkdir(scratch);
    const expected = { none: "bypassPermissions", verify: "default", read: "plan" };
    let previous = "";
    for (const [index, profile] of order.entries()) {
      // After the first turn the session is loaded and resumes its provider thread.
      const params = { cwd: f.root, permission: "danger-full-access", mcpServers: servers,
        ...(index > 0 ? { resume: "thread-1", freshContext: false } : {}),
        ...(profile === "none" ? {} : { reviewGuard: true, reviewProfile: profile, scratchDirectory: scratch }) };
      const result = await f.turn(params);
      if (profile === previous) {
        // The same launch reuses the live query and sets its mode again.
        assert.equal(result.query, null, `${index}: ${profile}`);
        assert.equal(result.mode, expected[profile]);
      } else {
        assert.ok(result.query, `${index}: ${profile} needs a fresh query`);
        assert.equal(result.query.permissionMode, expected[profile], `${index}: ${profile}`);
        assert.equal(result.query.sandbox?.enabled === true, profile === "verify");
        if (index > 0) assert.equal(result.query.resume, "thread-1");
      }
      assert.equal(result.decisions[0].behavior, profile === "none" ? "allow" : "deny", `${index}: ${profile}`);
      assert.deepEqual(result.prompts, []);
      previous = profile;
    }
    assert.equal(f.stderr(), "");
  });
}
