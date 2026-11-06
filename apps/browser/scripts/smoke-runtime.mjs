#!/usr/bin/env node
// Smoke-tests a staged runtime: starts <runtime>/bin/node <runtime>/src/mcp.mjs (never the
// host node) with a minimal environment, lists tools, reads capabilities and captures a
// local fixture page. Needs this checkout's dev dependencies for the MCP client.
//   node scripts/smoke-runtime.mjs <runtimeDir>
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import fs from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/client";
import { StdioClientTransport } from "@modelcontextprotocol/client/stdio";

const runtimeDir = path.resolve(process.argv[2] ?? "");
const manifest = JSON.parse(fs.readFileSync(path.join(runtimeDir, "runtime.json"), "utf8"));
const fixture = fs.readFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "test", "fixtures", "reference.html"));
const server = http.createServer((_request, response) => {
  response.writeHead(200, { "content-type": "text/html; charset=utf-8" });
  response.end(fixture);
});
await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
const artifacts = fs.mkdtempSync(path.join(os.tmpdir(), "ketplus-runtime-smoke-"));

const client = new Client({ name: "ketplus-runtime-smoke", version: "0.1.0" });
const env = { PATH: "/usr/bin:/bin", HOME: os.homedir(), KETPLUS_BROWSER_ARTIFACTS: artifacts, KETPLUS_BROWSER_ALLOWED_ORIGINS: "loopback" };
for (const name of ["KETPLUS_BROWSER_ENGINE", "KETPLUS_BROWSER_CACHE", "KETPLUS_BROWSER_DOWNLOAD"]) if (process.env[name]) env[name] = process.env[name];
await client.connect(new StdioClientTransport({
  command: path.join(runtimeDir, manifest.node),
  args: [path.join(runtimeDir, manifest.entrypoints.mcp)],
  cwd: runtimeDir,
  env,
  stderr: "inherit",
}));
const call = async (name, args = {}) => {
  const result = await client.callTool({ name, arguments: args });
  assert.ok(!result.isError, `${name}: ${JSON.stringify(result.structuredContent)}`);
  return result;
};
try {
  const { tools } = await client.listTools();
  console.log(`tools (${tools.length}): ${tools.map((tool) => tool.name).join(", ")}`);
  const session = (await call("browser_create_session", { viewport: { width: 800, height: 600 } })).structuredContent;
  const page = (await call("browser_open", { sessionId: session.sessionId, url: `${origin}/` })).structuredContent;
  // The host's KETPLUS_BROWSER_ALLOWED_ORIGINS reached the bundled server: a public
  // origin is refused before any page loads.
  const denied = await client.callTool({ name: "browser_open", arguments: { sessionId: session.sessionId, url: "https://example.com/" } });
  assert.ok(denied.isError, "a non-loopback origin is refused under the loopback policy");
  assert.equal(denied.structuredContent.code, "NAVIGATION_DENIED");
  const capture = await call("browser_capture", { sessionId: session.sessionId, pageId: page.pageId, label: "smoke" });
  const image = capture.content.find((block) => block.type === "image");
  const pngPath = capture.structuredContent.path;
  assert.ok(path.isAbsolute(pngPath), "capture returns the original PNG path");
  const bytes = fs.readFileSync(pngPath);
  assert.equal(createHash("sha256").update(bytes).digest("hex"), capture.structuredContent.sha256);
  const retained = path.join(artifacts, "retained.png");
  fs.copyFileSync(pngPath, retained);
  const engine = (await call("browser_capabilities")).structuredContent.engine;
  console.log(`engine: ${JSON.stringify(engine)}`);
  console.log(`environment: ${JSON.stringify(session.environment)}`);
  console.log(`capture: ${capture.structuredContent.width}x${capture.structuredContent.height} sha256=${capture.structuredContent.sha256} image=${image ? Buffer.from(image.data, "base64").length : 0} bytes`);
  await call("browser_close_session", { sessionId: session.sessionId });
  assert.ok(!fs.existsSync(pngPath), "session capture is removed on close");
  assert.deepEqual(fs.readFileSync(retained), bytes, "copied asset survives session cleanup");
  console.log(`runtime node: ${manifest.nodeVersion} OK`);
} finally {
  await client.close();
  server.close();
  fs.rmSync(artifacts, { recursive: true, force: true });
}
