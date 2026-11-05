import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { createInterface } from "node:readline";
import test from "node:test";

test("full access never asks for tool permission but still asks the user's questions", { timeout: 10000 }, async (t) => {
  // Run the real sidecar against a fake SDK; never import or call an authenticated provider.
  const root = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-claude-permission-"));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  for (const file of ["authentication.mjs", "index.mjs", "transcript.mjs", "input.mjs", "tasks.mjs", "resume.mjs", "session-delete.mjs", "session-usage.mjs"])
    await fs.copyFile(new URL(`../${file}`, import.meta.url), path.join(root, file));
  const sdk = path.join(root, "node_modules/@anthropic-ai/claude-agent-sdk");
  await fs.mkdir(sdk, { recursive: true });
  await fs.writeFile(path.join(sdk, "package.json"), JSON.stringify({ type: "module", exports: "./index.mjs" }));
  // The SDK reaches canUseTool in bypassPermissions when a command trips its safety checks.
  await fs.writeFile(path.join(sdk, "index.mjs"), `
    export function query({ prompt: input, options }) {
      return {
        async *[Symbol.asyncIterator]() {
          await input[Symbol.asyncIterator]().next();
          const bash = await options.canUseTool("Bash", { command: "node <<'EOF'\\nEOF" });
          const question = await options.canUseTool("AskUserQuestion", { questions: [{ question: "Which?" }] });
          yield { type: "result", session_id: "thread-1", result: JSON.stringify({
            permissionMode: options.permissionMode, bash, question
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
  for (const [id, permission] of [[1, "danger-full-access"], [2, "workspace-write"]]) {
    send({ id, method: "turn/start", params: { sessionId: `session-${id}`, prompt: "Edit", permission } });
    const requests = [];
    let answer;
    for (;;) {
      const { value, done } = await iterator.next();
      assert.equal(done, false, stderr);
      const message = JSON.parse(value);
      if (message.method === "permission.request" || message.method === "question.request") {
        requests.push(`${message.method}:${message.params.tool}`);
        send({ id: message.id, result: { allow: false } });
      }
      if (message.params?.type === "item.completed") answer = JSON.parse(message.params.payload.item.text);
      if (message.method === "test.closed") break;
    }
    const full = permission === "danger-full-access";
    assert.equal(answer.permissionMode, full ? "bypassPermissions" : "acceptEdits");
    assert.equal(answer.bash.behavior, full ? "allow" : "deny");
    assert.deepEqual(requests, full
      ? ["question.request:AskUserQuestion"]
      : ["permission.request:Bash", "question.request:AskUserQuestion"]);
  }
  assert.equal(stderr, "");
});
