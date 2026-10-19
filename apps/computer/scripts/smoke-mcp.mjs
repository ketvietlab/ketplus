#!/usr/bin/env node
// Drives ketplus-computer-mcp over stdio like a provider would: initialize,
// tools/list, screenshot and (with --click x,y) left_click. Consumes no
// provider quota. Usage: node smoke-mcp.mjs "<KetPlus Computer.app>" [--click x,y] [--out shot.png]
import { spawn } from "node:child_process";
import { writeFileSync } from "node:fs";
import { createInterface } from "node:readline";
import { join } from "node:path";

const [app, ...rest] = process.argv.slice(2);
if (!app) {
  console.error('usage: smoke-mcp.mjs "<KetPlus Computer.app>" [--click x,y] [--out shot.png]');
  process.exit(2);
}
const option = (name) => {
  const index = rest.indexOf(name);
  return index >= 0 ? rest[index + 1] : undefined;
};

const server = spawn(join(app, "Contents/MacOS/ketplus-computer-mcp"), [], { stdio: ["pipe", "pipe", "inherit"] });
const lines = createInterface({ input: server.stdout });
const pending = new Map();
lines.on("line", (line) => {
  const message = JSON.parse(line);
  pending.get(message.id)?.(message);
  pending.delete(message.id);
});
let nextId = 1;
const request = (method, params = {}) =>
  new Promise((resolve) => {
    const id = nextId++;
    pending.set(id, resolve);
    server.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id, method, params })}\n`);
  });

const timed = async (label, promise) => {
  const started = performance.now();
  const value = await promise;
  console.log(`${label}: ${Math.round(performance.now() - started)} ms`);
  return value;
};

const init = await timed("initialize", request("initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "smoke", version: "0" } }));
console.log("server:", init.result.serverInfo);
server.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" })}\n`);
const list = await request("tools/list");
console.log("tools:", list.result.tools.map((tool) => tool.name).join(", "));

const shot = await timed("screenshot", request("tools/call", { name: "screenshot", arguments: {} }));
const content = shot.result.content;
if (shot.result.isError) {
  console.log("screenshot error:", content[0].text);
} else {
  const image = content.find((part) => part.type === "image");
  const bytes = Buffer.from(image.data, "base64");
  // Claude bills an image at about width * height / 750 tokens.
  const [, width, height] = /(\d+)x(\d+)/.exec(content.find((part) => part.type === "text").text).map(Number);
  console.log(`image: ${width}x${height}, ${bytes.length} bytes PNG, ~${Math.round((width * height) / 750)} image tokens`);
  if (option("--out")) writeFileSync(option("--out"), bytes);
}

if (option("--click")) {
  const coordinate = option("--click").split(",").map(Number);
  const click = await timed("left_click", request("tools/call", { name: "left_click", arguments: { coordinate } }));
  console.log("left_click:", click.result.content[0].text);
}

const bad = await request("tools/call", { name: "left_click", arguments: { coordinate: [999999, 0] } });
console.log("out-of-bounds click:", bad.result.content[0].text);
server.stdin.end();
