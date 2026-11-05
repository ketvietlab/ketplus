import assert from "node:assert/strict";
import fs from "node:fs/promises";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { BrowserService } from "../src/browser-service.mjs";

// An island-style page: a facet filters in place with pushState, a scrolled grid renders rows
// far above its visible area, and the page posts to a server function on load.
const TAPE_CHART = `<!doctype html>
<html lang="en">
<head><link rel="stylesheet" href="/style.css"></head>
<body>
<main>
  <a id="free" href="?floor=9&view=free">Free tonight</a>
  <details id="menu"><summary>Menu</summary><a href="#settings">Settings</a></details>
  <input id="secret" type="password" value="hunter2-secret">
  <input id="note" value="typed note">
  <div class="hos-tape" style="height:200px; overflow:auto; margin-top:16px">
    ${Array.from({ length: 120 }, (_, index) => `<a class="row" href="#r${index}" style="display:block;height:30px">Room ${index}</a>`).join("")}
  </div>
</main>
<script>
  document.getElementById("free").addEventListener("click", (event) => {
    event.preventDefault();
    history.pushState({}, "", "?floor=9&view=free");
  });
  window.addEventListener("popstate", () => console.info("popstate " + location.search));
  fetch("/_ket/fn/hospitality_core.getTapeChart", { method: "POST", body: "{}" })
    .then((response) => response.text())
    .then(() => { document.documentElement.dataset.kvIslands = "hydrated"; });
</script>
</body>
</html>`;

const NOISY = `<!doctype html><main><h1>Noisy</h1></main><script>
  console.error("x");
  for (let index = 0; index < 3; index += 1) console.warn("repeated warning");
  setTimeout(() => { throw new Error("boom from timer"); }, 0);
  Promise.reject(new Error("rejected island"));
  fetch("/missing");
</script>`;

async function startServer() {
  const counts = { style: 0, tapeChart: 0 };
  const server = http.createServer(async (request, response) => {
    const url = new URL(request.url, "http://fixture");
    if (url.pathname === "/style.css") {
      counts.style += 1;
      response.writeHead(200, { "content-type": "text/css", "cache-control": "max-age=3600" });
      response.end(`main { color: rgb(${counts.style}, 0, 0); }`);
      return;
    }
    if (url.pathname === "/_ket/fn/hospitality_core.getTapeChart" && request.method === "POST") {
      counts.tapeChart += 1;
      for await (const _ of request);
      response.writeHead(200, { "content-type": "application/json" });
      response.end(JSON.stringify({ rows: "x".repeat(800_000) }));
      return;
    }
    if (url.pathname === "/missing") {
      response.writeHead(404, { "content-type": "text/plain" });
      response.end("missing");
      return;
    }
    response.writeHead(200, { "content-type": "text/html; charset=utf-8" });
    response.end(url.pathname === "/noisy" ? NOISY : TAPE_CHART);
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  return {
    origin: `http://127.0.0.1:${server.address().port}`,
    counts,
    close: () => new Promise((resolve) => server.close(resolve)),
  };
}

async function setup(t, viewport = { width: 1024, height: 768 }) {
  const fixture = await startServer();
  const artifactsDir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-browser-diagnostics-"));
  const service = new BrowserService({ artifactsDir });
  t.after(async () => {
    await service.close();
    await fixture.close();
    await fs.rm(artifactsDir, { recursive: true, force: true });
  });
  const { sessionId } = await service.createSession({ viewport, allowedOrigins: [fixture.origin] });
  return { fixture, service, sessionId };
}

const nodeNamed = (snapshot, name) => snapshot.nodes.find((node) => node.name === name);

test("history actions traverse pushState entries without reloading and report the navigation kind", async (t) => {
  const { fixture, service, sessionId } = await setup(t);
  const { pageId } = await service.open({ sessionId, url: `${fixture.origin}/screen?screen=tape-chart&floor=9` });
  const snapshot = await service.observe({ sessionId, pageId });
  const facet = await service.act({
    sessionId,
    pageId,
    expectedSnapshotId: snapshot.snapshotId,
    action: { type: "click", target: { nodeRef: nodeNamed(snapshot, "Free tonight").ref } },
  });
  assert.equal(facet.navigation, "same-document");
  assert.match(facet.url, /view=free$/);

  const back = await service.act({ sessionId, pageId, action: { type: "history", direction: "back" } });
  assert.equal(back.navigation, "same-document");
  assert.match(back.url, /floor=9$/);
  const forward = await service.act({ sessionId, pageId, action: { type: "history", direction: "forward" } });
  assert.equal(forward.navigation, "same-document");
  assert.match(forward.url, /view=free$/);
  // Each traversal fired popstate in the page, and the document was never replaced.
  const popstates = await service.logs({ sessionId, pageId, kinds: ["console"] });
  assert.deepEqual(
    popstates.entries.map((entry) => entry.text).filter((text) => text.startsWith("popstate")),
    ["popstate ?screen=tape-chart&floor=9", "popstate ?floor=9&view=free"],
  );
  assert.equal(fixture.counts.tapeChart, 1);

  const forwardAtEnd = await service.act({ sessionId, pageId, action: { type: "history", direction: "forward" } });
  assert.equal(forwardAtEnd.navigation, "none");
  const reload = await service.act({ sessionId, pageId, action: { type: "reload" } });
  assert.equal(reload.navigation, "full-load");
  assert.match(reload.url, /view=free$/);
  // A press reaches the page only; browser shortcuts such as Alt+ArrowLeft never navigate.
  const observed = await service.observe({ sessionId, pageId });
  const press = await service.act({
    sessionId,
    pageId,
    expectedSnapshotId: observed.snapshotId,
    action: { type: "press", target: { nodeRef: nodeNamed(observed, "Free tonight").ref }, key: "Alt+ArrowLeft" },
  });
  assert.equal(press.navigation, "none");
});

test("reload ignoreCache fetches cached stylesheets again", async (t) => {
  const { fixture, service, sessionId } = await setup(t);
  const url = `${fixture.origin}/screen`;
  const { pageId } = await service.open({ sessionId, url });
  await service.act({ sessionId, pageId, action: { type: "navigate", url } });
  assert.equal(fixture.counts.style, 1, "navigating again serves the stylesheet from the HTTP cache");
  const reload = await service.act({ sessionId, pageId, action: { type: "reload", ignoreCache: true } });
  assert.equal(reload.navigation, "full-load");
  assert.equal(fixture.counts.style, 2);
  const inspected = await service.inspect({ sessionId, pageId, selector: "main", fields: ["computedStyle:color"] });
  assert.equal(inspected.elements[0].computedStyle.color, "rgb(2, 0, 0)");
});

test("logs report console messages, uncaught errors with stacks, rejections and HTTP errors", async (t) => {
  const { fixture, service, sessionId } = await setup(t);
  const { pageId } = await service.open({ sessionId, url: `${fixture.origin}/noisy` });
  await service.wait({ sessionId, pageId, condition: { type: "load", state: "networkidle" } });
  let logs;
  for (let attempt = 0; attempt < 50; attempt += 1) {
    logs = await service.logs({ sessionId, pageId });
    if (logs.entries.some((entry) => entry.kind === "pageerror")) break;
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
  const byKind = (kind) => logs.entries.filter((entry) => entry.kind === kind);
  const consoleError = byKind("console").find((entry) => entry.level === "error" && entry.text === "x");
  assert.ok(consoleError);
  assert.deepEqual(consoleError.source, { url: `${fixture.origin}/noisy`, line: 2, column: 11 });
  const warning = byKind("console").find((entry) => entry.text === "repeated warning");
  assert.equal(warning.count, 3, "repeats collapse into one entry");
  const thrown = byKind("pageerror")[0];
  assert.equal(thrown.text, "Error: boom from timer");
  assert.match(thrown.stack, /boom from timer\n\s+at .*\/noisy:4:/);
  assert.equal(byKind("unhandledrejection")[0].text, "Error: rejected island");
  assert.ok(byKind("http").some((entry) => entry.text === `GET ${fixture.origin}/missing 404`));

  // The cursor returns only what arrived later; a level filter narrows the read.
  const errors = await service.logs({ sessionId, pageId, levels: ["error"] });
  assert.ok(errors.entries.every((entry) => entry.level === "error"));
  const later = await service.logs({ sessionId, pageId, since: logs.cursor });
  assert.deepEqual(later.entries, []);
  assert.equal(later.cursor, logs.cursor);
  const firstTwo = await service.logs({ sessionId, pageId, limit: 2 });
  assert.equal(firstTwo.entries.length, 2);
  assert.equal(firstTwo.hasMore, true);
  const rest = await service.logs({ sessionId, pageId, since: firstTwo.cursor });
  assert.equal(rest.entries[0].seq > firstTwo.cursor, true);

  const observation = await service.observe({ sessionId, pageId });
  assert.ok(observation.logs.errors >= 4);
  const reload = await service.act({ sessionId, pageId, action: { type: "reload" } });
  assert.ok(reload.logs.errorsSinceObserve >= 1, "errors after the observation are counted on the action");
});

test("network lists the page's own requests with status, size and timing", async (t) => {
  const { fixture, service, sessionId } = await setup(t);
  const { pageId } = await service.open({ sessionId, url: `${fixture.origin}/screen?screen=tape-chart` });
  await service.wait({ sessionId, pageId, condition: { type: "load", state: "networkidle" } });
  const calls = await service.network({ sessionId, pageId, urlContains: "/_ket/fn/hospitality_core.getTapeChart" });
  assert.equal(calls.entries.length, 1);
  const [call] = calls.entries;
  assert.equal(call.method, "POST");
  assert.equal(call.status, 200);
  assert.equal(call.resourceType, "fetch");
  assert.ok(call.responseBodyBytes >= 800_000 && call.responseBodyBytes < 801_000);
  assert.equal(typeof call.durationMs, "number");
  const all = await service.network({ sessionId, pageId, methods: ["GET"] });
  assert.ok(all.entries.some((entry) => entry.resourceType === "stylesheet"));
  assert.deepEqual((await service.network({ sessionId, pageId, failedOnly: true })).entries, []);
});

test("inspect reads attributes, scroll state and values without invalidating the snapshot", async (t) => {
  const { fixture, service, sessionId } = await setup(t);
  const { pageId } = await service.open({ sessionId, url: `${fixture.origin}/screen` });
  await service.wait({ sessionId, pageId, condition: { type: "load", state: "networkidle" } });
  const islands = await service.inspect({ sessionId, pageId, selector: "html", fields: ["attributes"] });
  assert.equal(islands.matchCount, 1);
  assert.equal(islands.elements[0].attributes["data-kv-islands"], "hydrated");

  const snapshot = await service.observe({ sessionId, pageId });
  const row = snapshot.nodes.find((node) => node.name === "Room 1");
  await service.act({
    sessionId,
    pageId,
    expectedSnapshotId: snapshot.snapshotId,
    action: { type: "scroll", target: { nodeRef: row.ref }, deltaY: 900 },
  });
  const tape = await service.inspect({ sessionId, pageId, selector: ".hos-tape", fields: ["scroll"] });
  assert.equal(tape.elements[0].scroll.top, 900);
  assert.equal(tape.elements[0].scroll.clientHeight, 200);

  const fields = await service.inspect({ sessionId, pageId, selector: "input", fields: ["value", "attributes"] });
  assert.equal(fields.elements[0].value, "[redacted]");
  assert.equal(fields.elements[0].attributes.value, undefined);
  assert.equal(fields.elements[1].value, "typed note");

  const rows = await service.inspect({ sessionId, pageId, selector: ".row", fields: ["text"], maxElements: 2 });
  assert.equal(rows.matchCount, 120);
  assert.equal(rows.truncated, true);
  assert.deepEqual(rows.elements.map((element) => element.text), ["Room 0", "Room 1"]);

  const fresh = await service.observe({ sessionId, pageId });
  const menu = fresh.nodes.find((node) => node.tag === "summary");
  const byRef = await service.inspect({
    sessionId,
    pageId,
    target: { nodeRef: menu.ref },
    expectedSnapshotId: fresh.snapshotId,
    fields: ["bounds", "computedStyle:display"],
  });
  assert.equal(byRef.elements[0].tag, "summary");
  // The snapshot is still the latest one, so its refs remain usable.
  const opened = await service.act({
    sessionId,
    pageId,
    expectedSnapshotId: fresh.snapshotId,
    action: { type: "click", target: { nodeRef: menu.ref } },
  });
  assert.equal(opened.outcome, "applied");
  await assert.rejects(
    service.inspect({ sessionId, pageId, selector: "text=Menu", fields: ["text"] }),
    (error) => error.code === "INVALID_ARGUMENT",
  );
  await assert.rejects(
    service.inspect({ sessionId, pageId, fields: ["text"] }),
    (error) => error.code === "INVALID_ARGUMENT",
  );
});

test("details summaries are observed as disclosure buttons", async (t) => {
  const { fixture, service, sessionId } = await setup(t);
  const { pageId } = await service.open({ sessionId, url: `${fixture.origin}/screen` });
  const before = await service.observe({ sessionId, pageId });
  const summary = before.nodes.find((node) => node.tag === "summary");
  assert.equal(summary.role, "button");
  assert.equal(summary.name, "Menu");
  assert.ok(summary.states.includes("collapsed"));
  await service.act({
    sessionId,
    pageId,
    expectedSnapshotId: before.snapshotId,
    action: { type: "click", target: { nodeRef: summary.ref } },
  });
  const after = await service.observe({ sessionId, pageId });
  assert.ok(after.nodes.find((node) => node.tag === "summary").states.includes("expanded"));
  assert.ok(nodeNamed(after, "Settings"));
});

test("visibleOnly keeps only nodes inside the visible area of their scroll container", async (t) => {
  const { fixture, service, sessionId } = await setup(t);
  const { pageId } = await service.open({ sessionId, url: `${fixture.origin}/screen` });
  const first = await service.observe({ sessionId, pageId });
  await service.act({
    sessionId,
    pageId,
    expectedSnapshotId: first.snapshotId,
    action: { type: "scroll", target: { nodeRef: first.nodes.find((node) => node.name === "Room 0").ref }, deltaY: 1500 },
  });
  const tape = (await service.inspect({ sessionId, pageId, selector: ".hos-tape", fields: ["bounds"] })).elements[0].bounds;
  const everything = await service.observe({ sessionId, pageId, detail: "full", maxNodes: 1000 });
  const rooms = (snapshot) => snapshot.nodes.filter((node) => node.name.startsWith("Room "));
  assert.ok(rooms(everything).some((node) => node.bounds.y + node.bounds.height < tape.y), "rows above the scrolled area exist");

  const visible = await service.observe({ sessionId, pageId, visibleOnly: true, detail: "full" });
  assert.equal(visible.visibleOnly, true);
  const visibleRooms = rooms(visible);
  assert.ok(visibleRooms.length >= 6 && visibleRooms.length <= 9, `${visibleRooms.length} rows fit in 200px`);
  for (const node of visibleRooms) {
    assert.ok(node.bounds.y + node.bounds.height > tape.y && node.bounds.y < tape.y + tape.height, node.name);
  }
  assert.ok(visibleRooms.some((node) => node.name === "Room 50"));
  assert.ok(nodeNamed(visible, "Free tonight"), "content outside scroll containers stays");
  assert.equal(visible.truncated, false);
});
