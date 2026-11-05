#!/usr/bin/env node
// Manual check of the real approval panel (ADR 0030) on a release build, for example
// KetPlus.app/Contents/Helpers/KetPlus Computer.app from the desktop build. Uses no
// provider quota and posts no input. You answer the panel three times: Deny, Esc, Allow.
// Allow is disabled for the first second after the panel appears.
// The agent is started with KETPLUS_COMPUTER_TEST_APPROVAL=allow on purpose: a release
// build must ignore it and still show the panel.
// Usage: node approval-check.mjs "<KetPlus Computer.app>" "<KetPlus Computer Fixture.app>"
import { execFileSync, spawn } from "node:child_process";
import { createHmac, randomBytes } from "node:crypto";
import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { stdin, stdout } from "node:process";

const [app, fixtureApp] = process.argv.slice(2).map((path) => resolve(path));
if (!app || !fixtureApp) {
  console.error('usage: approval-check.mjs "<KetPlus Computer.app>" "<KetPlus Computer Fixture.app>"');
  process.exit(2);
}
const dir = "/tmp/kpc-approval"; // short: Unix socket paths are limited to 104 bytes
let failures = 0;
const check = (name, ok, detail = "") => {
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? ` — ${detail}` : ""}`);
  if (!ok) failures += 1;
};

rmSync(dir, { recursive: true, force: true });
mkdirSync(dir, { recursive: true, mode: 0o700 });
const key = randomBytes(32);
writeFileSync(join(dir, "host.key"), key, { mode: 0o600 });
try { execFileSync("pkill", ["-f", "Contents/MacOS/KetPlusComputer$"], { stdio: "ignore" }); } catch {}
await new Promise((done) => setTimeout(done, 500));
execFileSync("/usr/bin/open", ["-g", "--env", `KETPLUS_COMPUTER_DIR=${dir}`,
  "--env", "KETPLUS_COMPUTER_TEST_APPROVAL=allow", "-a", app]);
const fixture = spawn(join(fixtureApp, "Contents/MacOS/KetPlusComputerFixture"), ["/tmp/kpc-approval-fixture.txt"],
  { stdio: ["ignore", "pipe", "inherit"] });
await new Promise((done) => createInterface({ input: fixture.stdout }).once("line", done));

const conversation = "approval-check";
const server = spawn(join(app, "Contents/MacOS/ketplus-computer-mcp"), [], {
  stdio: ["pipe", "pipe", "inherit"],
  env: { ...process.env, KETPLUS_COMPUTER_DIR: dir, KETPLUS_COMPUTER_CONVERSATION: conversation,
         KETPLUS_COMPUTER_TOKEN: createHmac("sha256", key).update(`computer.v0:${conversation}`).digest("hex") },
});
const pending = new Map();
createInterface({ input: server.stdout }).on("line", (line) => {
  const message = JSON.parse(line);
  pending.get(message.id)?.(message);
});
let nextId = 1;
const call = (name, args = {}) => new Promise((done) => {
  const id = nextId++;
  pending.set(id, (message) => done(message.result.content.find((part) => part.type === "text")?.text ?? ""));
  server.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id, method: "tools/call", params: { name, arguments: args } })}\n`);
});
const terminal = createInterface({ input: stdin, output: stdout });
const ask = (question) => new Promise((done) => terminal.question(question, done));

async function request(instruction) {
  console.log(`\n>>> A "KetPlus Computer" panel opens. ${instruction}`);
  const started = Date.now();
  const answer = JSON.parse(await call("request_access", {
    apps: ["dev.ketplus.computer.fixture"], reason: "Approval check: type into the fixture's field.",
  }));
  return { answer, seconds: (Date.now() - started) / 1000 };
}

const deny = await request("Read it, then click Deny.");
check("the test override is ignored: the call waited for you", deny.seconds > 1, `${deny.seconds.toFixed(1)} s`);
check("the agent recorded Deny", deny.answer.decision === "deny", deny.answer.decision);
check("Deny grants nothing", deny.answer.granted.length === 0 && deny.answer.denied.length === 1, JSON.stringify(deny.answer));
check("nothing is captured after Deny", (await call("screenshot")).startsWith("APP_NOT_GRANTED"));
const looks = await ask("Did the panel name the fixture, show the reason and the warning-style icon, with Deny and Allow? [y/n] ");
check("the panel content looked right", looks.trim().toLowerCase().startsWith("y"));

const escape = await request("Press Esc on the keyboard.");
check("Esc denies", escape.answer.decision === "deny" && escape.answer.granted.length === 0, JSON.stringify(escape.answer));

const allow = await request('Wait a second, then click "Allow for this conversation".');
check("Allow grants the fixture", allow.answer.decision === "allow"
  && allow.answer.granted.includes("dev.ketplus.computer.fixture"), JSON.stringify(allow.answer));
check("a screenshot works after Allow", (await call("screenshot")).startsWith("Screenshot"));

terminal.close();
server.stdin.end();
fixture.kill();
try { execFileSync("pkill", ["-f", "Contents/MacOS/KetPlusComputer$"], { stdio: "ignore" }); } catch {}
console.log(failures === 0 ? "\nall checks passed" : `\n${failures} check(s) failed`);
process.exit(failures === 0 ? 0 : 1);
