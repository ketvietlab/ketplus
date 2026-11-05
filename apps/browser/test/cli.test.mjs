import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

test("serves newline-delimited JSON-RPC without starting a browser for capabilities", async () => {
  const child = spawn(process.execPath, ["src/cli.mjs"], {
    cwd: appRoot,
    stdio: ["pipe", "pipe", "pipe"],
  });
  let stdout = "";
  child.stdout.setEncoding("utf8");
  child.stdout.on("data", (chunk) => { stdout += chunk; });
  child.stdin.write('{"jsonrpc":"2.0","id":1,"method":"browser.capabilities"}\n');
  child.stdin.write('{"jsonrpc":"2.0","method":"browser.capabilities"}\n');
  child.stdin.write('{"jsonrpc":"2.0","id":3,"method":"browser.observe","params":{}}\n');
  child.stdin.write('{"jsonrpc":"2.0","id":4,"method":"browser.unknown"}\n');
  child.stdin.write('{"id":5,"method":"browser.capabilities"}\n');
  child.stdin.write("not json\n");
  child.stdin.write('{"jsonrpc":"2.0","id":2,"method":"server.shutdown"}\n');
  child.stdin.end();
  const exitCode = await new Promise((resolve, reject) => {
    child.on("error", reject);
    child.on("close", resolve);
  });
  assert.equal(exitCode, 0);
  const responses = stdout.trim().split("\n").map((line) => JSON.parse(line));
  const byId = new Map(responses.filter((response) => response.id !== null).map((response) => [response.id, response]));
  // The notification (no id) produces no response.
  assert.equal(responses.length, 6);
  assert.equal(byId.get(1).result.protocolVersion, "browser.v0");
  assert.equal(byId.get(1).result.engine.adapter, "playwright");
  assert.equal(byId.get(3).error.code, -32602);
  assert.equal(byId.get(3).error.data.code, "INVALID_PARAMS");
  assert.equal(byId.get(4).error.code, -32601);
  assert.deepEqual(
    responses.filter((response) => response.id === null).map((response) => response.error.code).sort((left, right) => left - right),
    [-32700, -32600],
  );
  assert.equal(byId.get(2).result.outcome, "closed");
});

test("exits after server.shutdown while the host keeps stdin open", async () => {
  const child = spawn(process.execPath, ["src/cli.mjs"], {
    cwd: appRoot,
    stdio: ["pipe", "pipe", "pipe"],
  });
  let stdout = "";
  child.stdout.setEncoding("utf8");
  child.stdout.on("data", (chunk) => { stdout += chunk; });
  child.stdin.write('{"jsonrpc":"2.0","id":1,"method":"server.shutdown"}\n');
  const exitCode = await Promise.race([
    new Promise((resolve) => child.on("close", resolve)),
    new Promise((resolve) => setTimeout(() => resolve("timeout"), 5000)),
  ]);
  child.kill();
  assert.equal(exitCode, 0);
  assert.equal(JSON.parse(stdout.trim()).result.outcome, "closed");
});
