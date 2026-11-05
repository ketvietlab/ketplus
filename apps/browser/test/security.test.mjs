import assert from "node:assert/strict";
import fs from "node:fs/promises";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { BrowserService } from "../src/browser-service.mjs";

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function listen(handler) {
  const server = http.createServer(handler);
  const sockets = new Set();
  server.on("connection", (socket) => {
    sockets.add(socket);
    socket.on("close", () => sockets.delete(socket));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  return {
    server,
    origin: `http://127.0.0.1:${server.address().port}`,
    close: () => {
      for (const socket of sockets) socket.destroy();
      return new Promise((resolve) => server.close(() => resolve()));
    },
  };
}

function html(response, body) {
  response.writeHead(200, { "content-type": "text/html; charset=utf-8" });
  response.end(body);
}

async function setup(t) {
  const outsideHits = [];
  const outside = await listen((request, response) => {
    outsideHits.push(request.url);
    html(response, "<h1>Outside</h1>");
  });
  outside.server.on("upgrade", (request, socket) => {
    outsideHits.push(`upgrade ${request.url}`);
    socket.destroy();
  });
  const app = await listen((request, response) => {
    switch (request.url) {
      case "/redirect":
        response.writeHead(302, { location: `${outside.origin}/landed?token=abc` });
        response.end();
        return;
      case "/leaky":
        html(response, `<h1>Leaky</h1>
          <img src="${outside.origin}/pixel">
          <iframe src="${outside.origin}/frame"></iframe>
          <script>
            fetch("${outside.origin}/fetch").catch(() => {});
            try { new WebSocket("${outside.origin.replace("http:", "ws:")}/socket"); } catch {}
          </script>`);
        return;
      case "/popup":
        html(response, `<a id="popup" href="${outside.origin}/popup" target="_blank">Open popup</a>`);
        return;
      case "/password":
        html(response, `<main><label for="pw">Password</label><input id="pw" type="password" value="hunter2-secret"></main>`);
        return;
      case "/mutating":
        html(response, `<button id="target">Alpha</button>
          <script>setTimeout(() => { document.getElementById("target").textContent = "Beta"; }, 1000);</script>`);
        return;
      case "/states":
        html(response, `<main>
          <input type="checkbox" id="agree" aria-label="Agree">
          <input id="locked" aria-label="Locked" readonly value="fixed">
          <div id="host"></div>
          <script>
            document.getElementById("host").attachShadow({ mode: "open" }).innerHTML =
              '<button id="shadow">Shadow</button>';
          </script>
        </main>`);
        return;
      case "/large":
        html(response, `<ul>${Array.from({ length: 20_000 }, (_, index) => `<li>Item ${index}</li>`).join("")}</ul>`);
        return;
      default:
        html(response, `<main><button id="first">First</button><button id="second">Second</button></main>`);
    }
  });
  const artifactsDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-security-"));
  const service = new BrowserService({ artifactsDir });
  t.after(async () => {
    await service.close();
    await app.close();
    await outside.close();
    await fs.rm(artifactsDir, { recursive: true, force: true });
  });
  const { sessionId } = await service.createSession({
    viewport: { width: 800, height: 600 },
    allowedOrigins: [app.origin],
  });
  return { service, sessionId, app, outsideHits };
}

test("origin policy covers redirects, subresources, frames, sockets and popups", async (t) => {
  const { service, sessionId, app, outsideHits } = await setup(t);

  await assert.rejects(
    service.open({ sessionId, url: `${app.origin}/redirect` }),
    (error) => error.code === "NAVIGATION_DENIED" && !JSON.stringify(error.data).includes("abc"),
  );

  await service.open({ sessionId, url: `${app.origin}/leaky` });
  await delay(500);
  assert.ok((await service.pages({ sessionId })).blockedRequestCount >= 3);

  const opener = await service.open({ sessionId, url: `${app.origin}/popup` });
  const observation = await service.observe({ sessionId, pageId: opener.pageId });
  const link = observation.nodes.find((node) => node.stableId === "popup");
  await service.act({
    sessionId,
    pageId: opener.pageId,
    expectedSnapshotId: observation.snapshotId,
    action: { type: "click", target: { nodeRef: link.ref } },
  });
  let popup;
  for (let attempt = 0; attempt < 60 && !popup; attempt += 1) {
    popup = (await service.pages({ sessionId })).pages.find(
      (page) => page.openerPageId === opener.pageId && !page.allowed,
    );
    if (!popup) await delay(50);
  }
  assert.ok(popup, "popup is tracked by the session");
  await assert.rejects(
    service.observe({ sessionId, pageId: popup.pageId }),
    (error) => error.code === "NAVIGATION_DENIED",
  );

  assert.deepEqual(outsideHits, []);
});

test("sessions cannot widen the host origin policy", async (t) => {
  const artifactsDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-ceiling-"));
  const service = new BrowserService({ artifactsDir });
  t.after(async () => {
    await service.close();
    await fs.rm(artifactsDir, { recursive: true, force: true });
  });
  for (const allowedOrigins of [["*"], ["https://example.com"]]) {
    await assert.rejects(
      service.dispatch("browser.session.create", { allowedOrigins }),
      (error) => error.code === "ORIGIN_NOT_PERMITTED",
    );
  }
});

test("observations never expose password values", async (t) => {
  const { service, sessionId, app } = await setup(t);
  const page = await service.open({ sessionId, url: `${app.origin}/password` });
  const observation = await service.observe({ sessionId, pageId: page.pageId });
  assert.ok(observation.nodes.some((node) => node.stableId === "pw" && node.name === "Password"));
  assert.equal(JSON.stringify(observation).includes("hunter2-secret"), false);
});

test("concurrent and failed actions never reuse a snapshot", async (t) => {
  const { service, sessionId, app } = await setup(t);
  const page = await service.open({ sessionId, url: `${app.origin}/` });
  const observation = await service.observe({ sessionId, pageId: page.pageId });
  const click = (snapshot, stableId, extra = {}) =>
    service.act({
      sessionId,
      pageId: page.pageId,
      expectedSnapshotId: snapshot.snapshotId,
      action: {
        type: "click",
        target: { nodeRef: snapshot.nodes.find((node) => node.stableId === stableId).ref },
        ...extra,
      },
    });

  const results = await Promise.allSettled([click(observation, "first"), click(observation, "second")]);
  assert.equal(results.filter((result) => result.status === "fulfilled").length, 1);
  assert.equal(results.find((result) => result.status === "rejected").reason.code, "STALE_SNAPSHOT");

  const next = await service.observe({ sessionId, pageId: page.pageId });
  const first = next.nodes.find((node) => node.stableId === "first");
  await assert.rejects(
    service.act({
      sessionId,
      pageId: page.pageId,
      expectedSnapshotId: next.snapshotId,
      action: { type: "fill", target: { nodeRef: first.ref }, value: "x", timeoutMs: 1000 },
    }),
    (error) => error.code !== "STALE_SNAPSHOT",
  );
  await assert.rejects(click(next, "first"), (error) => error.code === "STALE_SNAPSHOT");
});

test("a recycled node fails instead of receiving the action", async (t) => {
  const { service, sessionId, app } = await setup(t);
  const page = await service.open({ sessionId, url: `${app.origin}/mutating` });
  const observation = await service.observe({ sessionId, pageId: page.pageId });
  const target = observation.nodes.find((node) => node.stableId === "target");
  assert.equal(target.name, "Alpha");
  await service.wait({ sessionId, pageId: page.pageId, condition: { type: "text", text: "Beta" }, timeoutMs: 5000 });
  await assert.rejects(
    service.act({
      sessionId,
      pageId: page.pageId,
      expectedSnapshotId: observation.snapshotId,
      action: { type: "click", target: { nodeRef: target.ref } },
    }),
    (error) => error.code === "NODE_CHANGED" && error.data.actual.name === "Beta",
  );
});

test("states are role-appropriate and open shadow roots are observed", async (t) => {
  const { service, sessionId, app } = await setup(t);
  const page = await service.open({ sessionId, url: `${app.origin}/states` });
  const observation = await service.observe({ sessionId, pageId: page.pageId });
  const byId = (stableId) => observation.nodes.find((node) => node.stableId === stableId);
  assert.equal(byId("agree").states.includes("readonly"), false);
  assert.ok(byId("locked").states.includes("readonly"));
  assert.equal(byId("shadow").name, "Shadow");
  await service.act({
    sessionId,
    pageId: page.pageId,
    expectedSnapshotId: observation.snapshotId,
    action: { type: "check", target: { nodeRef: byId("agree").ref } },
  });
});

test("large documents are limited inside the page", async (t) => {
  const { service, sessionId, app } = await setup(t);
  const page = await service.open({ sessionId, url: `${app.origin}/large` });
  const started = performance.now();
  const observation = await service.observe({ sessionId, pageId: page.pageId, maxNodes: 50 });
  const elapsed = performance.now() - started;
  assert.equal(observation.nodes.length, 50);
  assert.equal(observation.visibleNodeCount, 20_001);
  assert.equal(observation.truncated, true);
  assert.equal(observation.nodes.at(-1).ref, `${observation.snapshotId}:n50`);
  assert.ok(elapsed < 5000, `observation took ${Math.round(elapsed)} ms`);
});

test("comparison refuses evidence from different page states", async (t) => {
  const { service, sessionId, app } = await setup(t);
  const first = await service.open({ sessionId, url: `${app.origin}/` });
  const second = await service.open({ sessionId, url: `${app.origin}/password` });
  const firstObservation = await service.observe({ sessionId, pageId: first.pageId, includeScreenshot: true });
  const secondObservation = await service.observe({ sessionId, pageId: second.pageId, includeScreenshot: true });
  const secondEvidence = { snapshotId: secondObservation.snapshotId, captureId: secondObservation.captureId };

  await assert.rejects(
    service.compare({
      reference: { snapshotId: firstObservation.snapshotId, captureId: secondObservation.captureId },
      candidate: secondEvidence,
    }),
    (error) => error.code === "EVIDENCE_MISMATCH",
  );

  await service.act({ sessionId, pageId: first.pageId, action: { type: "scroll", deltaY: 10 } });
  const later = await service.capture({ sessionId, pageId: first.pageId });
  await assert.rejects(
    service.compare({
      reference: { snapshotId: firstObservation.snapshotId, captureId: later.captureId },
      candidate: secondEvidence,
    }),
    (error) => error.code === "EVIDENCE_MISMATCH",
  );
});
