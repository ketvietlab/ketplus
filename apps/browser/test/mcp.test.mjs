import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import fs from "node:fs/promises";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const fixturePath = path.join(appRoot, "test/fixtures/reference.html");

async function startFixtureServer() {
  const server = http.createServer(async (_request, response) => {
    response.writeHead(200, { "content-type": "text/html; charset=utf-8" });
    response.end(await fs.readFile(fixturePath));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  return {
    origin: `http://127.0.0.1:${address.port}`,
    close: () => new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve())),
  };
}

test("an MCP host can discover and call the canonical browser tools", async (t) => {
  const fixture = await startFixtureServer();
  const artifactsDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-mcp-"));
  const client = new Client({ name: "browser-prototype-test", version: "0.1.0" });
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [path.join(appRoot, "src/mcp.mjs")],
    cwd: appRoot,
    env: { ...process.env, KETPLUS_BROWSER_ARTIFACTS: artifactsDir },
    stderr: "pipe",
  });
  t.after(async () => {
    await client.close();
    await fixture.close();
    await fs.rm(artifactsDir, { recursive: true, force: true });
  });
  await client.connect(transport);
  // Hosts get the skill essentials at initialize, so agents need no pointer to SKILL.md.
  const instructions = client.getInstructions();
  assert.ok(instructions.length < 3072, "instructions stay compact");
  for (const phrase of ["browser_observe", "snapshotId", "STALE_SNAPSHOT", "NAVIGATION_DENIED",
    "rootSelector", "visual_compare", "browser_close_session", "provider-native browsers", "instead of falling back",
    "Safari/WebKit", "native desktop apps that are not browsers",
    "You MUST call browser_close_session", "before your final response",
    "blocked or abandoned work", "report the unclosed", "Do not close user-owned",
    "browser_capture returns path", "capturePath", "Use/copy those files", "Do not rebuild PNGs"]) {
    assert.ok(instructions.includes(phrase), `instructions mention ${phrase}`);
  }
  const { tools } = await client.listTools();
  assert.deepEqual(
    tools.map((tool) => tool.name).sort(),
    [
      "browser_act",
      "browser_capabilities",
      "browser_capture",
      "browser_close_all_sessions",
      "browser_close_page",
      "browser_close_session",
      "browser_create_session",
      "browser_inspect",
      "browser_logs",
      "browser_network",
      "browser_observe",
      "browser_open",
      "browser_pages",
      "browser_session_request",
      "browser_wait",
      "visual_compare",
      "visual_compare_native",
    ],
  );
  const result = await client.callTool({ name: "browser_capabilities", arguments: {} });
  assert.equal(result.isError, undefined);
  assert.equal(result.structuredContent.protocolVersion, "browser.v0");

  const sessionResult = await client.callTool({
    name: "browser_create_session",
    arguments: {
      viewport: { width: 800, height: 600 },
      allowedOrigins: [fixture.origin],
    },
  });
  const sessionId = sessionResult.structuredContent.sessionId;
  const pageResult = await client.callTool({
    name: "browser_open",
    arguments: { sessionId, url: fixture.origin },
  });
  const observation = await client.callTool({
    name: "browser_observe",
    arguments: {
      sessionId,
      pageId: pageResult.structuredContent.pageId,
      includeScreenshot: true,
    },
  });
  assert.ok(observation.structuredContent.nodes.some((node) => node.stableId === "start"));
  assert.ok(observation.content.some((item) => item.type === "image" && item.mimeType === "image/png"));
  const observedPath = observation.structuredContent.capturePath;
  assert.ok(path.isAbsolute(observedPath));
  assert.equal(createHash("sha256").update(await fs.readFile(observedPath)).digest("hex"),
    createHash("sha256").update(Buffer.from(observation.content.find(item => item.type === "image").data, "base64")).digest("hex"));
  const captured = await client.callTool({ name: "browser_capture", arguments: {
    sessionId, pageId: pageResult.structuredContent.pageId, label: "Reusable asset",
  } });
  assert.equal(captured.isError, undefined);
  const original = captured.structuredContent.path;
  assert.ok(path.isAbsolute(original));
  const assetDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-reused-"));
  t.after(() => fs.rm(assetDir, { recursive: true, force: true }));
  const asset = path.join(assetDir, "screenshot.png");
  await fs.copyFile(original, asset);
  const originalBytes = await fs.readFile(original);
  assert.equal(createHash("sha256").update(originalBytes).digest("hex"), captured.structuredContent.sha256);
  assert.deepEqual(originalBytes.subarray(0, 8), Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]));
  await client.callTool({ name: "browser_close_session", arguments: { sessionId } });
  await assert.rejects(fs.access(original), { code: "ENOENT" });
  await assert.rejects(fs.access(observedPath), { code: "ENOENT" });
  assert.deepEqual(await fs.readFile(asset), originalBytes);

});

async function connect(t, env = {}) {
  const artifactsDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-mcp-"));
  const client = new Client({ name: "browser-prototype-test", version: "0.1.0" });
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [path.join(appRoot, "src/mcp.mjs")],
    cwd: appRoot,
    env: { ...process.env, KETPLUS_BROWSER_ARTIFACTS: artifactsDir, ...env },
    stderr: "pipe",
  });
  t.after(async () => {
    await client.close();
    await fs.rm(artifactsDir, { recursive: true, force: true });
  });
  await client.connect(transport);
  return client;
}

async function createSession(client) {
  const result = await client.callTool({
    name: "browser_create_session",
    arguments: { viewport: { width: 400, height: 300 } },
  });
  assert.equal(result.isError, undefined);
  return result.structuredContent.sessionId;
}

async function isOpen(client, sessionId) {
  const result = await client.callTool({ name: "browser_pages", arguments: { sessionId } });
  if (!result.isError) return true;
  assert.equal(result.structuredContent.code, "SESSION_NOT_FOUND");
  return false;
}

test("the host can close every session the agent left open", async (t) => {
  const client = await connect(t);
  const first = await createSession(client);
  const second = await createSession(client);
  const result = await client.callTool({ name: "browser_close_all_sessions", arguments: {} });
  assert.deepEqual(result.structuredContent.sessionIds.sort(), [first, second].sort());
  assert.equal(await isOpen(client, first), false);
  assert.equal(await isOpen(client, second), false);
});

test("a turn-end signal closes the sessions opened before it and keeps later ones", async (t) => {
  const signalDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-turn-"));
  t.after(() => fs.rm(signalDir, { recursive: true, force: true }));
  const signal = path.join(signalDir, "turn-end");
  const client = await connect(t, { KETPLUS_BROWSER_TURN_END_FILE: signal });
  const finished = await createSession(client);
  await fs.writeFile(signal, `${Date.now()}\n`);
  const next = await createSession(client);
  const deadline = Date.now() + 5_000;
  while (await isOpen(client, finished)) {
    assert.ok(Date.now() < deadline, "the finished turn's session closes");
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  assert.equal(await isOpen(client, next), true);
});
