import assert from "node:assert/strict";
import fs from "node:fs/promises";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { BrowserService } from "../src/browser-service.mjs";
import { LIMITS } from "../src/protocol.mjs";

const fixtureRoot = path.join(path.dirname(fileURLToPath(import.meta.url)), "fixtures");

async function startFixtureServer() {
  const server = http.createServer(async (request, response) => {
    if (request.url === "/login" && request.method === "POST") {
      const chunks = [];
      for await (const chunk of request) chunks.push(chunk);
      const credentials = JSON.parse(Buffer.concat(chunks).toString("utf8"));
      if (credentials.login !== "agent" || credentials.password !== "secret") {
        response.writeHead(401, { "content-type": "text/plain; charset=utf-8" });
        response.end("denied");
        return;
      }
      response.writeHead(200, {
        "content-type": "application/json; charset=utf-8",
        "set-cookie": "fixture_session=accepted; Path=/; HttpOnly; SameSite=Lax",
      });
      response.end(JSON.stringify({ token: "must-not-be-returned" }));
      return;
    }
    if (request.url === "/protected") {
      if (!request.headers.cookie?.includes("fixture_session=accepted")) {
        response.writeHead(401, { "content-type": "text/plain; charset=utf-8" });
        response.end("sign in first");
        return;
      }
      response.writeHead(200, { "content-type": "text/html; charset=utf-8" });
      response.end('<main><h1 id="authenticated">Authenticated</h1></main>');
      return;
    }
    if (request.url === "/many") {
      const label = "Long action label ".repeat(20);
      const rows = Array.from({ length: 600 }, (_, index) => `<button id="b${index}">${label}${index}</button>`);
      response.writeHead(200, { "content-type": "text/html; charset=utf-8" });
      response.end(`<main>${rows.join("")}</main>`);
      return;
    }
    const name = request.url === "/candidate" ? "candidate.html" : "reference.html";
    response.writeHead(200, { "content-type": "text/html; charset=utf-8" });
    response.end(await fs.readFile(path.join(fixtureRoot, name)));
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  return {
    server,
    origin: `http://127.0.0.1:${address.port}`,
    close: () => new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve())),
  };
}

test("observes, acts, fences stale nodes, and compares structured render output", async (t) => {
  const fixture = await startFixtureServer();
  const artifactsDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-test-"));
  const nativeCaptureRoot = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-native-test-"));
  const service = new BrowserService({ artifactsDir, nativeCaptureRoot });
  t.after(async () => {
    await service.close();
    await fixture.close();
    await fs.rm(artifactsDir, { recursive: true, force: true });
    await fs.rm(nativeCaptureRoot, { recursive: true, force: true });
  });

  const session = await service.createSession({
    viewport: { width: 800, height: 600 },
    deviceScaleFactor: 1,
    theme: "light",
    allowedOrigins: [fixture.origin],
    evidenceContext: { fixtureId: "workspace-light" },
  });
  const referencePage = await service.open({ sessionId: session.sessionId, url: `${fixture.origin}/reference` });
  const candidatePage = await service.open({ sessionId: session.sessionId, url: `${fixture.origin}/candidate` });

  const reference = await service.observe({
    sessionId: session.sessionId,
    pageId: referencePage.pageId,
    includeScreenshot: true,
    label: "reference",
  });
  const candidate = await service.observe({
    sessionId: session.sessionId,
    pageId: candidatePage.pageId,
    includeScreenshot: true,
    label: "candidate",
  });

  assert.ok(reference.ariaSnapshot.includes('button "Start"'));
  // Replies are summaries by default; the stored snapshots keep the computed styles that the
  // fontWeight finding below depends on.
  assert.equal(reference.detail, "summary");
  assert.ok(reference.nodes.every((node) => !("styles" in node) && !("clipped" in node)));
  assert.equal(reference.nodes.find((node) => node.stableId === "sidebar").bounds.width, 216);
  const scopedReference = await service.observe({
    sessionId: session.sessionId,
    pageId: referencePage.pageId,
    rootSelector: "#content",
  });
  assert.equal(scopedReference.rootSelector, "#content");
  assert.equal(scopedReference.nodes.some((node) => node.stableId === "sidebar"), false);
  const start = candidate.nodes.find((node) => node.stableId === "start");
  assert.ok(start);

  const comparison = await service.compare({
    reference: { snapshotId: reference.snapshotId, captureId: reference.captureId },
    candidate: { snapshotId: candidate.snapshotId, captureId: candidate.captureId },
    policy: { geometryTolerance: 1, maxPixelMismatchRatio: 0.05 },
  });
  assert.equal(comparison.status, "failed");
  assert.ok(comparison.findings.some((finding) => finding.kind === "geometry"));
  assert.ok(comparison.findings.some((finding) => finding.kind === "style" && finding.property === "fontWeight"));
  assert.ok(comparison.raster.mismatchRatio > 0);

  const withoutSidebarPixels = await service.compare({
    reference: { snapshotId: reference.snapshotId, captureId: reference.captureId },
    candidate: { snapshotId: candidate.snapshotId, captureId: candidate.captureId },
    policy: { ignoredRegions: [{ x: 0, y: 0, width: 216, height: 600 }] },
  });
  assert.equal(withoutSidebarPixels.raster.ignoredPixels, 216 * 600);
  assert.equal(withoutSidebarPixels.raster.comparedPixels, (800 - 216) * 600);

  const identical = await service.compare({
    reference: { snapshotId: reference.snapshotId, captureId: reference.captureId },
    candidate: { snapshotId: reference.snapshotId, captureId: reference.captureId },
  });
  assert.equal(identical.status, "passed");
  assert.equal(identical.raster.mismatchRatio, 0);

  const nativePath = path.join(nativeCaptureRoot, "desktop.png");
  await fs.copyFile(service.captures.get(reference.captureId).path, nativePath);
  const native = await service.dispatch("visual.compare.native", {
    reference: { snapshotId: reference.snapshotId, captureId: reference.captureId },
    candidate: { path: nativePath, viewport: { width: 800, height: 600 },
      deviceScaleFactor: 1, theme: "light" },
    policy: { maxPixelMismatchRatio: 0.05 },
  });
  assert.equal(native.status, "passed");
  assert.equal(native.mode, "raster-only");
  assert.equal(native.raster.mismatchRatio, 0);
  await assert.rejects(service.dispatch("visual.compare.native", {
    reference: { snapshotId: reference.snapshotId, captureId: reference.captureId },
    candidate: { path: service.captures.get(reference.captureId).path,
      viewport: { width: 800, height: 600 }, deviceScaleFactor: 1, theme: "light" },
  }), { code: "INVALID_NATIVE_CAPTURE" });

  await service.act({
    sessionId: session.sessionId,
    pageId: candidatePage.pageId,
    expectedSnapshotId: candidate.snapshotId,
    action: { type: "click", target: { nodeRef: start.ref } },
  });
  await assert.rejects(
    service.act({
      sessionId: session.sessionId,
      pageId: candidatePage.pageId,
      expectedSnapshotId: candidate.snapshotId,
      action: { type: "click", target: { nodeRef: start.ref } },
    }),
    (error) => error.code === "STALE_SNAPSHOT",
  );
  const afterAction = await service.observe({
    sessionId: session.sessionId,
    pageId: candidatePage.pageId,
  });
  assert.equal(afterAction.nodes.find((node) => node.stableId === "status").text, "Working");
});

test("summary observations stay within the byte budget and full detail returns everything", async (t) => {
  const fixture = await startFixtureServer();
  const artifactsDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-test-"));
  const service = new BrowserService({ artifactsDir });
  t.after(async () => {
    await service.close();
    await fixture.close();
    await fs.rm(artifactsDir, { recursive: true, force: true });
  });
  const session = await service.createSession({ allowedOrigins: [fixture.origin] });
  const page = await service.open({ sessionId: session.sessionId, url: `${fixture.origin}/many` });
  const target = { sessionId: session.sessionId, pageId: page.pageId };

  const summary = await service.observe({ ...target, maxNodes: 1000 });
  assert.equal(summary.detail, "summary");
  assert.equal(summary.truncated, true);
  assert.ok(Buffer.byteLength(JSON.stringify(summary)) <= 32_000);
  assert.ok(summary.nodes.length > 0);
  assert.ok(summary.nodes.every((node) => !("styles" in node) && node.text.length <= 161));

  // A ref from a summary still addresses its node.
  const node = summary.nodes.find((candidate) => candidate.role === "button");
  const acted = await service.act({
    ...target,
    expectedSnapshotId: summary.snapshotId,
    action: { type: "hover", target: { nodeRef: node.ref } },
  });
  assert.ok(acted);

  const full = await service.observe({ ...target, maxNodes: 1000, detail: "full" });
  assert.equal(full.detail, undefined);
  assert.ok(full.nodes.length > summary.nodes.length);
  assert.ok(full.nodes.every((node) => node.styles && typeof node.clipped === "boolean"));
  assert.ok(Buffer.byteLength(JSON.stringify(full)) > 32_000);
});

test("denies origins outside the session policy before launching a page", async (t) => {
  const artifactsDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-policy-"));
  const service = new BrowserService({ artifactsDir, allowedOrigins: ["https://example.test"] });
  t.after(async () => {
    await service.close();
    await fs.rm(artifactsDir, { recursive: true, force: true });
  });
  const session = await service.createSession({ allowedOrigins: ["https://example.test"] });
  await assert.rejects(
    service.open({ sessionId: session.sessionId, url: "https://not-allowed.test/" }),
    (error) => error.code === "NAVIGATION_DENIED",
  );
});

test("session requests establish browser cookies without exposing response secrets", async (t) => {
  const fixture = await startFixtureServer();
  const artifactsDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-auth-"));
  const service = new BrowserService({ artifactsDir });
  t.after(async () => {
    await service.close();
    await fixture.close();
    await fs.rm(artifactsDir, { recursive: true, force: true });
  });

  const session = await service.createSession({ allowedOrigins: [fixture.origin] });
  const login = await service.request({
    sessionId: session.sessionId,
    url: `${fixture.origin}/login`,
    method: "POST",
    headers: { "content-type": "application/json" },
    json: { login: "agent", password: "secret" },
  });
  assert.deepEqual(
    Object.keys(login).sort(),
    ["contentType", "ok", "outcome", "redirected", "sessionId", "status", "url"],
  );
  assert.equal(login.status, 200);

  const page = await service.open({ sessionId: session.sessionId, url: `${fixture.origin}/protected` });
  const observation = await service.observe({ sessionId: session.sessionId, pageId: page.pageId });
  assert.equal(observation.nodes.find((node) => node.stableId === "authenticated").text, "Authenticated");
});

test("sessions and pages are capped and closing one frees its slot", async (t) => {
  const fixture = await startFixtureServer();
  const artifactsDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-limits-"));
  const service = new BrowserService({ artifactsDir });
  t.after(async () => {
    await service.close();
    await fixture.close();
    await fs.rm(artifactsDir, { recursive: true, force: true });
  });
  const limit = (code, max) => (error) => error.code === code && error.data?.limit === max;
  const options = { allowedOrigins: [fixture.origin] };

  // Sessions still starting count, so concurrent creates cannot overshoot the cap.
  const created = await Promise.allSettled(
    Array.from({ length: LIMITS.maxSessions + 1 }, () => service.createSession(options)),
  );
  const sessions = created.filter((result) => result.status === "fulfilled").map((result) => result.value);
  assert.equal(sessions.length, LIMITS.maxSessions);
  assert.ok(limit("LIMIT_REACHED", LIMITS.maxSessions)(created.find((r) => r.status === "rejected").reason));
  await service.closeSession({ sessionId: sessions.pop().sessionId });
  const session = await service.createSession(options);

  const opened = await Promise.allSettled(
    Array.from({ length: LIMITS.maxPagesPerSession + 1 }, () =>
      service.open({ sessionId: session.sessionId, url: `${fixture.origin}/` })),
  );
  const pages = opened.filter((result) => result.status === "fulfilled").map((result) => result.value);
  assert.equal(pages.length, LIMITS.maxPagesPerSession);
  assert.ok(limit("LIMIT_REACHED", LIMITS.maxPagesPerSession)(opened.find((r) => r.status === "rejected").reason));
  await assert.rejects(
    service.open({ sessionId: session.sessionId, url: `${fixture.origin}/` }),
    limit("LIMIT_REACHED", LIMITS.maxPagesPerSession),
  );
  // A page the site opens past the cap (a popup) is closed rather than adopted.
  const context = service.sessions.get(session.sessionId).context;
  const popup = await context.newPage().catch(() => null);
  if (popup && !popup.isClosed()) await popup.waitForEvent("close", { timeout: 5_000 });
  assert.equal(context.pages().length, LIMITS.maxPagesPerSession);
  assert.equal((await service.pages({ sessionId: session.sessionId })).pages.length, LIMITS.maxPagesPerSession);

  await service.closePage({ sessionId: session.sessionId, pageId: pages[0].pageId });
  await service.open({ sessionId: session.sessionId, url: `${fixture.origin}/` });
});
