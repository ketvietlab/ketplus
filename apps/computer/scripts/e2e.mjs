#!/usr/bin/env node
// End-to-end check of KetPlus Computer on a real Mac (ADR 0026). Needs a test build
// (build-app.sh --testing) that already holds Screen Recording and Accessibility, and the
// fixture from build-fixture.sh. Uses no provider quota. Moves and clicks the real pointer.
// Usage: node e2e.mjs "<KetPlus Computer.app>" "<KetPlus Computer Fixture.app>" [--manual-esc]
// --manual-esc asks you to press Esc: synthetic key events from an untrusted shell never reach
// the kill switch, so only a real key press verifies it.
import { execFileSync, spawn } from "node:child_process";
import { createHmac, createHash, randomBytes } from "node:crypto";
import { chmodSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createInterface } from "node:readline";
import { join, resolve } from "node:path";

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
  "scroll", "left_click_drag", "list_apps", "request_access", "open_application", "wait"].every((name) => tools.includes(name)));

const forged = front("e2e-a", "0".repeat(64));
check("a forged token is refused", text(await forged.call("screenshot")).startsWith("UNAUTHORIZED"));
forged.close();

check("nothing is captured before a grant", text(await a.call("screenshot")).startsWith("APP_NOT_GRANTED"));

const fixture = spawn(join(fixtureApp, "Contents/MacOS/KetPlusComputerFixture"), [output], { stdio: ["ignore", "pipe", "inherit"] });
const frames = await new Promise((resolve) => createInterface({ input: fixture.stdout }).once("line", resolve));
const numbers = frames.match(/-?\d+/g).map(Number);
const [fx, fy, fw, fh, bx, by, bw, bh, sw, sh] = numbers;
await new Promise((resolve) => setTimeout(resolve, 800));

const apps = json(await a.call("list_apps")).apps;
const listed = apps.find((entry) => entry.bundleId === "dev.ketplus.computer.fixture");
check("list_apps shows the fixture as full and not granted", listed?.tier === "full" && listed?.granted === false);
const ketplus = apps.find((entry) => entry.bundleId === "dev.ketplus.editor");
if (ketplus) check("KetPlus itself is blocked", ketplus.tier === "blocked");

const blocked = json(await a.call("request_access", { apps: ["com.apple.keychainaccess"], reason: "e2e" }));
check("blocked apps are never offered for approval", blocked.blocked.length === 1 && blocked.granted.length === 0);
const granted = json(await a.call("request_access", { apps: ["dev.ketplus.computer.fixture"], reason: "e2e" }));
check("request_access grants the fixture", granted.granted.includes("dev.ketplus.computer.fixture"));

const shot = await a.call("screenshot");
const image = shot.content.find((part) => part.type === "image");
const [, width, height] = /(\d+)x(\d+)/.exec(text(shot)).map(Number);
check("screenshot shows only granted apps", text(shot).includes("granted apps visible: dev.ketplus.computer.fixture") && !!image?.data);
const toImage = (x, y) => [Math.round(x * width / sw), Math.round(y * height / sh)];

const zoom = await a.call("zoom", { region: [...toImage(fx, fy), ...toImage(fx + fw, fy + fh)] });
check("zoom returns an image", zoom.content.some((part) => part.type === "image"));

const typed = "Xin chào KetPlus ✓";
const fieldClick = text(await a.call("left_click", { coordinate: toImage(fx + fw / 2, fy + fh / 2) }));
check("click the field", fieldClick === "OK", fieldClick.slice(0, 120));
check("type into it", text(await a.call("type", { text: typed })) === "OK");
check("cursor_position reports image pixels", Array.isArray(json(await a.call("cursor_position")).coordinate));
const submitClick = text(await a.call("left_click", { coordinate: toImage(bx + bw / 2, by + bh / 2) }));
check("click Submit", submitClick === "OK", submitClick.slice(0, 120));
await new Promise((resolve) => setTimeout(resolve, 600));
check("the fixture received the typed text", existsSync(output) && readFileSync(output, "utf8").trim() === typed,
  existsSync(output) ? JSON.stringify(readFileSync(output, "utf8").trim()) : "no output");

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
  entries().some((entry) => entry.tool === "type" && entry.characters === typed.length) && !auditText().includes("KetPlus ✓"));

if (manualEsc) {
  await a.call("list_apps"); // keep the lease active
  await a.call("cursor_position");
  check("lease is active before Esc", JSON.parse(execFileSync(join(app, "Contents/MacOS/ketplus-computer-mcp"),
    ["--status"], { env: { ...process.env, KETPLUS_COMPUTER_DIR: dir } })).controlling === true);
  // A held lease consumes the user's Esc; the agent's own Esc carries its PID and passes.
  console.log("\n>>> Press Esc now (within 20 s)…");
  let stopped = "";
  for (let i = 0; i < 40 && !stopped.startsWith("CANCELLED"); i++) {
    await new Promise((resolve) => setTimeout(resolve, 500));
    stopped = text(await a.call("left_click", { coordinate: [4, height - 4] }));
  }
  check("a user Esc stops the conversation", stopped.startsWith("CANCELLED"), stopped.slice(0, 50));
  check("Esc is audited", entries().some((entry) => entry.outcome === "cancelled_by_user"));
} else {
  console.log("skip Esc kill switch (run with --manual-esc and press Esc)");
}

a.close();
fixture.kill();
chmodSync(dir, 0o700);
console.log(failures === 0 ? "\nall checks passed" : `\n${failures} check(s) failed`);
process.exit(failures === 0 ? 0 : 1);
