#!/usr/bin/env node
// End-to-end check of KetPlus Computer on a real Mac (ADR 0026, 0027). Needs a test build
// (build-app.sh --testing) that already holds Screen Recording and Accessibility, and the
// fixture and fixture-cover from build-fixture.sh. Uses no provider quota. The background
// part must not move the pointer; the foreground part moves and clicks the real pointer.
// Usage: node e2e.mjs "<KetPlus Computer.app>" "<KetPlus Computer Fixture.app>" [--manual-esc]
// --manual-esc asks you to press Esc and then click Stop: synthetic events from an untrusted
// shell never reach the kill switch, so only real input verifies them.
import { execFileSync, spawn } from "node:child_process";
import { createHmac, createHash, randomBytes } from "node:crypto";
import { chmodSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createInterface } from "node:readline";
import { dirname, join, resolve } from "node:path";

const manualEsc = process.argv.includes("--manual-esc");
const [app, fixtureApp] = process.argv.slice(2).filter((arg) => !arg.startsWith("--")).map((path) => resolve(path));
if (!app || !fixtureApp) {
  console.error('usage: e2e.mjs "<KetPlus Computer.app>" "<KetPlus Computer Fixture.app>"');
  process.exit(2);
}
const dir = "/tmp/kpc-e2e"; // short: Unix socket paths are limited to 104 bytes
const output = "/tmp/kpc-e2e-fixture.txt";
let failures = 0;
const check = (name, ok, detail = "") => {
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? ` — ${detail}` : ""}`);
  if (!ok) failures += 1;
};

// The daemon's role: host key and per-conversation tokens.
rmSync(dir, { recursive: true, force: true });
mkdirSync(dir, { recursive: true, mode: 0o700 });
const key = randomBytes(32);
writeFileSync(join(dir, "host.key"), key, { mode: 0o600 });
const token = (conversation) => createHmac("sha256", key).update(`computer.v0:${conversation}`).digest("hex");
const stem = (conversation) => createHash("sha256").update(conversation).digest("hex").slice(0, 32);

try { execFileSync("pkill", ["-f", "Contents/MacOS/KetPlusComputer$"], { stdio: "ignore" }); } catch {}
await new Promise((resolve) => setTimeout(resolve, 500));
execFileSync("/usr/bin/open", ["-g", "--env", `KETPLUS_COMPUTER_DIR=${dir}`,
  "--env", "KETPLUS_COMPUTER_TEST_APPROVAL=allow", "-a", app]);

function front(conversation, presentedToken = token(conversation)) {
  const server = spawn(join(app, "Contents/MacOS/ketplus-computer-mcp"), [], {
    stdio: ["pipe", "pipe", "inherit"],
    env: { ...process.env, KETPLUS_COMPUTER_DIR: dir, KETPLUS_COMPUTER_CONVERSATION: conversation,
           KETPLUS_COMPUTER_TOKEN: presentedToken },
  });
  const pending = new Map();
  createInterface({ input: server.stdout }).on("line", (line) => {
    const message = JSON.parse(line);
    pending.get(message.id)?.(message);
    pending.delete(message.id);
  });
  let nextId = 1;
  const request = (method, params = {}) => new Promise((resolve) => {
    const id = nextId++;
    pending.set(id, resolve);
    server.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id, method, params })}\n`);
  });
  const call = async (name, args = {}) => (await request("tools/call", { name, arguments: args })).result;
  return { request, call, close: () => server.stdin.end() };
}
const text = (result) => result.content.find((part) => part.type === "text")?.text ?? "";
const json = (result) => JSON.parse(text(result));

const a = front("e2e-a");
await a.request("initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "e2e", version: "0" } });
const tools = (await a.request("tools/list")).result.tools.map((tool) => tool.name);
check("tools/list has the toolset and grant tools", ["screenshot", "zoom", "left_click", "type", "key",
  "scroll", "left_click_drag", "list_apps", "request_access", "open_application", "observe", "act", "wait"].every((name) => tools.includes(name)));

const forged = front("e2e-a", "0".repeat(64));
check("a forged token is refused", text(await forged.call("screenshot")).startsWith("UNAUTHORIZED"));
forged.close();

check("nothing is captured before a grant", text(await a.call("screenshot")).startsWith("APP_NOT_GRANTED"));

// Each fixture run prints its field and button frames, then exits after Submit.
async function launchFixture() {
  const process_ = spawn(join(fixtureApp, "Contents/MacOS/KetPlusComputerFixture"), [output],
    { stdio: ["ignore", "pipe", "inherit"] });
  const frames = await new Promise((resolve) => createInterface({ input: process_.stdout }).once("line", resolve));
  const [fx, fy, fw, fh, bx, by, bw, bh, sw, sh] = frames.match(/-?\d+/g).map(Number);
  await new Promise((resolve) => setTimeout(resolve, 800));
  return { process: process_, field: [fx, fy, fw, fh], button: [bx, by, bw, bh], screen: [sw, sh] };
}
const centre = ([x, y, w, h]) => [x + w / 2, y + h / 2];
const received = () => (existsSync(output) ? readFileSync(output, "utf8").trim() : null);
const settle = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

let fixture = await launchFixture();
const [sw, sh] = fixture.screen;
const apps = json(await a.call("list_apps")).apps;
const listed = apps.find((entry) => entry.bundleId === "dev.ketplus.computer.fixture");
check("list_apps shows the fixture as full and not granted", listed?.tier === "full" && listed?.granted === false);
const ketplus = apps.find((entry) => entry.bundleId === "dev.ketplus.editor");
if (ketplus) check("KetPlus itself is blocked", ketplus.tier === "blocked");

const blocked = json(await a.call("request_access", { apps: ["com.apple.keychainaccess"], reason: "e2e" }));
check("blocked apps are never offered for approval", blocked.blocked.length === 1 && blocked.granted.length === 0);
const granted = json(await a.call("request_access", { apps: ["dev.ketplus.computer.fixture"], reason: "e2e" }));
check("request_access grants the fixture", granted.granted.includes("dev.ketplus.computer.fixture"));

// ---- Background (ADR 0027): the fixture is covered by a bundle-less window and Finder is frontmost.
const [fx, fy, , ] = fixture.field;
const [, by, , bh] = fixture.button;
const cover = spawn(join(dirname(fixtureApp), "fixture-cover"), [fx - 30, fy - 30, 420, by + bh - fy + 60].map(String),
  { stdio: ["ignore", "pipe", "inherit"] });
await new Promise((resolve) => createInterface({ input: cover.stdout }).once("line", resolve));
execFileSync("/usr/bin/open", ["-a", "Finder"]);
await settle(800);
console.log("     (do not type or move the mouse during the background checks)");
const frontBefore = json(await a.call("list_apps")).apps.find((entry) => entry.bundleId === "dev.ketplus.computer.fixture");
check("the fixture is in the background", frontBefore?.frontmost === false);

const shot = await a.call("screenshot");
const image = shot.content.find((part) => part.type === "image");
const [, width, height] = /(\d+)x(\d+)/.exec(text(shot)).map(Number);
check("screenshot shows only granted apps", text(shot).includes("granted apps visible: dev.ketplus.computer.fixture") && !!image?.data);
const toImage = (x, y) => [Math.round(x * width / sw), Math.round(y * height / sh)];
const zoom = await a.call("zoom", { region: [...toImage(fx, fy), ...toImage(fx + 360, fy + 28)] });
check("zoom returns an image", zoom.content.some((part) => part.type === "image"));

const pointerBefore = json(await a.call("cursor_position")).coordinate;
const typed = "Xin chào KetPlus ✓";
const focus = text(await a.call("left_click", { coordinate: toImage(...centre(fixture.field)) }));
check("a background click focuses the covered field", /"action":"focus"/.test(focus) && /"mode":"background"/.test(focus),
  focus.slice(0, 120));
check("background typing reaches it", /"mode":"background"/.test(text(await a.call("type", { text: "draft" }))));
const selectAll = text(await a.call("key", { text: "super+a" }));
check("background select-all selects the field's text", /"delivery":"text_selection"/.test(selectAll), selectAll.slice(0, 100));
await a.call("type", { text: typed });
const first = text(await a.call("observe", { app: "dev.ketplus.computer.fixture" }));
const second = text(await a.call("observe", { app: "dev.ketplus.computer.fixture" }));
const snapshotOf = (tree) => /^Snapshot (s\d+)/.exec(tree)?.[1];
const fieldLine = second.split("\n").find((line) => / textfield /.test(line)) ?? "";
check("observe reads the covered field's value", fieldLine.includes(`value="${typed}"`), fieldLine.trim().slice(0, 100));
const submitRef = /\[(n\d+)\] button "Submit"/.exec(second)?.[1];
check("observe lists Submit with press", !!submitRef && /button "Submit".*actions=[a-z_,]*press/.test(second));
const stale = text(await a.call("act", { snapshot_id: snapshotOf(first), ref: submitRef, action: "press" }));
check("an older snapshot is stale", stale.startsWith("STALE_SNAPSHOT"), stale.slice(0, 60));
const coveredForeground = text(await a.call("left_click", { coordinate: toImage(...centre(fixture.field)), foreground: true }));
check("a foreground click on the covered field hits the cover and is refused",
  /^(APP_BLOCKED|APP_NOT_GRANTED)/.test(coveredForeground), coveredForeground.slice(0, 60));
const drag = text(await a.call("left_click_drag", { start_coordinate: [10, 10], coordinate: [20, 20] }));
check("drag needs the real pointer", drag.startsWith("NEEDS_FOREGROUND"), drag.slice(0, 40));
const third = text(await a.call("observe", { app: "dev.ketplus.computer.fixture" }));
const pressRef = /\[(n\d+)\] button "Submit"/.exec(third)?.[1];
const pressed = text(await a.call("act", { snapshot_id: snapshotOf(third), ref: pressRef, action: "press" }));
check("act presses Submit in the background", /"mode":"background"/.test(pressed), pressed.slice(0, 80));
await settle(600);
check("the covered fixture received the text", received() === typed, JSON.stringify(received()));
const pointerAfter = json(await a.call("cursor_position")).coordinate;
const frontAfter = json(await a.call("list_apps")).apps.find((entry) => entry.bundleId === "dev.ketplus.computer.fixture");
check("background input did not bring the fixture forward", !frontAfter || frontAfter.frontmost === false);
// The user may move the mouse meanwhile; the agent would have left it on the field or Submit.
const near = (point, target) => Math.hypot(point[0] - target[0], point[1] - target[1]) < 6;
check("background input never moved the pointer to its targets",
  ![fixture.field, fixture.button].some((frame) => near(pointerAfter, toImage(...centre(frame))))
    || JSON.stringify(pointerBefore) === JSON.stringify(pointerAfter), `${pointerBefore} -> ${pointerAfter}`);
cover.kill();
await settle(1200);

// ---- Foreground (ADR 0026): the real pointer and keyboard, fixture in front.
fixture = await launchFixture();
await a.call("screenshot");
const fieldClick = text(await a.call("left_click", { coordinate: toImage(...centre(fixture.field)), foreground: true }));
check("foreground click on the field", fieldClick === "OK", fieldClick.slice(0, 120));
check("foreground typing", text(await a.call("type", { text: typed, foreground: true })) === "OK");
check("cursor_position reports image pixels", Array.isArray(json(await a.call("cursor_position")).coordinate));
const submitClick = text(await a.call("left_click", { coordinate: toImage(...centre(fixture.button)), foreground: true }));
check("foreground click on Submit", submitClick === "OK", submitClick.slice(0, 120));
await settle(600);
check("the fixture received the typed text", received() === typed, JSON.stringify(received()));

const outside = text(await a.call("left_click", { coordinate: [4, height - 4] }));
check("input outside granted apps is refused", /^(APP_NOT_GRANTED|APP_BLOCKED)/.test(outside), outside.slice(0, 60));

const b = front("e2e-b");
check("a second conversation is locked out", text(await b.call("screenshot")).startsWith("CONTROL_LOCKED"));
b.close();

mkdirSync(join(dir, "remote-turns"), { recursive: true });
writeFileSync(join(dir, "remote-turns", stem("e2e-a")), "");
check("a remote turn is refused", text(await a.call("screenshot")).startsWith("REMOTE_TURN"));
rmSync(join(dir, "remote-turns", stem("e2e-a")));

const audit = join(dir, "audit", `${stem("e2e-a")}.jsonl`);
const auditText = () => (existsSync(audit) ? readFileSync(audit, "utf8") : "");
const entries = () => auditText().trim().split("\n").filter(Boolean).map((line) => JSON.parse(line));
check("the audit log records actions without typed text",
  entries().some((entry) => entry.tool === "type" && entry.characters === typed.length && entry.mode === "background")
    && !auditText().includes("KetPlus ✓"));

if (manualEsc) {
  // Esc stops the agent while a granted app is frontmost; the fixture activates itself.
  fixture = await launchFixture();
  await a.call("list_apps");
  check("lease is active before Esc", JSON.parse(execFileSync(join(app, "Contents/MacOS/ketplus-computer-mcp"),
    ["--status"], { env: { ...process.env, KETPLUS_COMPUTER_DIR: dir } })).controlling === true);
  const waitFor = async (prompt) => {
    console.log(`\n>>> ${prompt} (within 20 s)…`);
    let stopped = "";
    for (let i = 0; i < 40 && !stopped.startsWith("CANCELLED"); i++) {
      await settle(500);
      stopped = text(await a.call("screenshot"));
    }
    return stopped;
  };
  const afterEsc = await waitFor("Press Esc now");
  check("a user Esc stops the conversation", afterEsc.startsWith("CANCELLED"), afterEsc.slice(0, 50));
  check("Esc is audited", entries().some((entry) => entry.tool === "esc" && entry.outcome === "cancelled_by_user"));
  json(await a.call("request_access", { apps: ["dev.ketplus.computer.fixture"], reason: "e2e" }));
  await a.call("screenshot");
  const afterStop = await waitFor("Click Stop on the KetPlus banner at the top of the screen");
  check("the Stop button stops the conversation", afterStop.startsWith("CANCELLED"), afterStop.slice(0, 50));
  check("Stop is audited", entries().some((entry) => entry.tool === "stop" && entry.outcome === "cancelled_by_user"));
  fixture.process.kill();
} else {
  console.log("skip Esc and Stop (run with --manual-esc)");
}

a.close();
fixture.process.kill();
chmodSync(dir, 0o700);
console.log(failures === 0 ? "\nall checks passed" : `\n${failures} check(s) failed`);
process.exit(failures === 0 ? 0 : 1);
