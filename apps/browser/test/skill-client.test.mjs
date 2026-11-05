import assert from "node:assert/strict";
import fs from "node:fs/promises";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { connect } from "../.claude/skills/ketplus-browser/scripts/browser-rpc.mjs";

// Keeps the agent skill's bundled client and its documented flow working.
test("the agent skill client drives a login flow and a scoped comparison", async (t) => {
  const server = http.createServer((request, response) => {
    response.writeHead(200, { "content-type": "text/html; charset=utf-8" });
    if (request.url.startsWith("/welcome")) {
      response.end("<main><h1>Welcome</h1></main>");
      return;
    }
    const width = request.url === "/candidate" ? 240 : 200;
    response.end(`<body style="margin:0;display:flex">
      <aside style="width:${width}px;height:600px;background:#eee"><nav><a href="/">Home</a></nav></aside>
      <main style="flex:1;padding:16px">
        <form action="/welcome">
          <label for="email">Email</label><input id="email" name="email">
          <button>Sign in</button>
        </form>
      </main>
    </body>`);
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const origin = `http://127.0.0.1:${server.address().port}`;
  const artifactsDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-skill-"));
  const browser = await connect({ allowedOrigins: [origin], artifactsDir });
  t.after(async () => {
    await browser.close();
    server.close();
    await fs.rm(artifactsDir, { recursive: true, force: true });
  });

  await assert.rejects(
    browser.call("browser.observe", {}),
    (error) => error.code === "INVALID_PARAMS" && error.rpcCode === -32602,
  );

  const { sessionId } = await browser.call("browser.session.create", {
    viewport: { width: 800, height: 600 },
    allowedOrigins: [origin],
  });
  const { pageId } = await browser.call("browser.open", { sessionId, url: `${origin}/reference` });
  let snapshot = await browser.call("browser.observe", { sessionId, pageId, rootSelector: "main", maxNodes: 80 });
  const email = snapshot.nodes.find((node) => node.role === "textbox" && node.name === "Email");
  await browser.call("browser.act", {
    sessionId,
    pageId,
    expectedSnapshotId: snapshot.snapshotId,
    action: { type: "fill", target: { nodeRef: email.ref }, value: "agent@example.test" },
  });
  snapshot = await browser.call("browser.observe", { sessionId, pageId, rootSelector: "main", maxNodes: 80 });
  const submit = snapshot.nodes.find((node) => node.role === "button" && node.name === "Sign in");
  await browser.call("browser.act", {
    sessionId,
    pageId,
    expectedSnapshotId: snapshot.snapshotId,
    action: { type: "click", target: { nodeRef: submit.ref } },
  });
  await browser.call("browser.wait", { sessionId, pageId, condition: { type: "text", text: "Welcome" } });

  const [reference, candidate] = await Promise.all(
    ["reference", "candidate"].map(async (name) => {
      const page = await browser.call("browser.open", { sessionId, url: `${origin}/${name}` });
      return browser.call("browser.observe", {
        sessionId,
        pageId: page.pageId,
        rootSelector: "main",
        includeScreenshot: true,
      });
    }),
  );
  const comparison = await browser.call("visual.compare", {
    reference: { snapshotId: reference.snapshotId, captureId: reference.captureId },
    candidate: { snapshotId: candidate.snapshotId, captureId: candidate.captureId },
    policy: {
      rasterRegions: {
        reference: { x: 200, y: 0, width: 500, height: 600 },
        candidate: { x: 240, y: 0, width: 500, height: 600 },
      },
    },
  });
  assert.equal(comparison.findings.some((finding) => finding.kind === "missing-element"), false);
  assert.equal(comparison.raster.width, 500);
});
