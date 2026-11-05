#!/usr/bin/env node
// Minimal JSON-RPC 2.0 client for apps/browser/src/cli.mjs.
//
//   import { connect } from ".../browser-rpc.mjs";
//   const browser = await connect({ allowedOrigins: ["http://127.0.0.1:3000"] });
//   const { sessionId } = await browser.call("browser.session.create", {});
//   await browser.close();
//
// One-off call from a shell:
//   node browser-rpc.mjs browser.capabilities
//   node browser-rpc.mjs browser.protocol.schema
import { spawn } from "node:child_process";
import path from "node:path";
import readline from "node:readline";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../../..");

export class BrowserRpcError extends Error {
  constructor(error) {
    super(error.message);
    this.name = "BrowserRpcError";
    this.code = error.data?.code ?? String(error.code);
    this.rpcCode = error.code;
    this.details = error.data?.details;
  }
}

export async function connect({ allowedOrigins, artifactsDir, env = {} } = {}) {
  const child = spawn(process.execPath, [path.join(appRoot, "src/cli.mjs")], {
    cwd: appRoot,
    stdio: ["pipe", "pipe", "inherit"],
    env: {
      ...process.env,
      ...(allowedOrigins?.length ? { KETPLUS_BROWSER_ALLOWED_ORIGINS: allowedOrigins.join(",") } : {}),
      ...(artifactsDir ? { KETPLUS_BROWSER_ARTIFACTS: artifactsDir } : {}),
      ...env,
    },
  });
  child.stdin.on("error", () => {});

  let nextId = 1;
  const pending = new Map();
  readline.createInterface({ input: child.stdout }).on("line", (line) => {
    let message;
    try {
      message = JSON.parse(line);
    } catch {
      return;
    }
    const waiter = pending.get(message.id);
    if (!waiter) return;
    pending.delete(message.id);
    if (message.error) waiter.reject(new BrowserRpcError(message.error));
    else waiter.resolve(message.result);
  });
  const exited = new Promise((resolve) => {
    child.on("exit", (code) => {
      for (const waiter of pending.values()) waiter.reject(new Error(`Browser server exited with code ${code}`));
      pending.clear();
      resolve(code);
    });
  });

  const call = (method, params = {}) => {
    if (child.exitCode !== null) return Promise.reject(new Error("Browser server is not running"));
    return new Promise((resolve, reject) => {
      const id = nextId++;
      pending.set(id, { resolve, reject });
      child.stdin.write(`${JSON.stringify({ jsonrpc: "2.0", id, method, params })}\n`);
    });
  };

  return {
    call,
    // Closes every session, stops Chromium and waits for the process to exit.
    async close() {
      if (child.exitCode === null) {
        await call("server.shutdown").catch(() => {});
        child.stdin.end();
      }
      return exited;
    },
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [method = "browser.capabilities", params = "{}"] = process.argv.slice(2);
  const browser = await connect();
  try {
    process.stdout.write(`${JSON.stringify(await browser.call(method, JSON.parse(params)), null, 2)}\n`);
  } catch (error) {
    process.stderr.write(`${JSON.stringify({ code: error.code, message: error.message, details: error.details })}\n`);
    process.exitCode = 1;
  } finally {
    await browser.close();
  }
}
