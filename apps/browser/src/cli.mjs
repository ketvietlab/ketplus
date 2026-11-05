#!/usr/bin/env node
import os from "node:os";
import path from "node:path";
import readline from "node:readline";
import { BrowserService } from "./browser-service.mjs";
import { toApiError } from "./errors.mjs";
import { parseOriginList } from "./origin-policy.mjs";

// JSON-RPC 2.0 error codes. The canonical string code travels in error.data.code.
export function jsonRpcCode(apiCode) {
  switch (apiCode) {
    case "METHOD_NOT_FOUND": return -32601;
    case "INVALID_PARAMS":
    case "INVALID_ARGUMENT": return -32602;
    case "INTERNAL_ERROR": return -32603;
    default: return -32000;
  }
}

const service = new BrowserService({
  artifactsDir:
    process.env.KETPLUS_BROWSER_ARTIFACTS || path.join(os.tmpdir(), `ketplus-browser-rpc-${process.pid}`),
  allowedOrigins: parseOriginList(process.env.KETPLUS_BROWSER_ALLOWED_ORIGINS),
});
const input = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
const pending = new Set();

function write(value) {
  process.stdout.write(`${JSON.stringify(value)}\n`);
}

const isValidId = (id) => id === null || typeof id === "string" || (typeof id === "number" && Number.isFinite(id));
const isRequest = (value) =>
  value !== null &&
  typeof value === "object" &&
  !Array.isArray(value) &&
  value.jsonrpc === "2.0" &&
  typeof value.method === "string" &&
  (!("id" in value) || isValidId(value.id));

async function handle(request) {
  const isNotification = !("id" in request);
  try {
    const result = await service.dispatch(request.method, request.params ?? {});
    if (!isNotification) write({ jsonrpc: "2.0", id: request.id, result });
  } catch (error) {
    if (isNotification) return;
    const apiError = toApiError(error, "INTERNAL_ERROR");
    write({
      jsonrpc: "2.0",
      id: request.id,
      error: {
        code: jsonRpcCode(apiError.code),
        message: apiError.message,
        data: { code: apiError.code, ...(apiError.data !== undefined ? { details: apiError.data } : {}) },
      },
    });
  }
}

let shutdown = false;
for await (const line of input) {
  if (!line.trim()) continue;
  let request;
  try {
    request = JSON.parse(line);
  } catch {
    write({ jsonrpc: "2.0", id: null, error: { code: -32700, message: "Parse error" } });
    continue;
  }
  if (!isRequest(request)) {
    write({ jsonrpc: "2.0", id: null, error: { code: -32600, message: "Invalid JSON-RPC 2.0 request" } });
    continue;
  }
  if (request.method === "server.shutdown") {
    await Promise.allSettled(pending);
    await service.close();
    if ("id" in request) write({ jsonrpc: "2.0", id: request.id, result: { outcome: "closed" } });
    shutdown = true;
    break;
  }
  // Requests run concurrently; BrowserService serializes work per page.
  const task = handle(request).finally(() => pending.delete(task));
  pending.add(task);
}

if (!shutdown) {
  await Promise.allSettled(pending);
  await service.close();
}

// A host may keep the pipe open after shutdown; stdin must not keep the
// process alive.
input.close();
process.stdin.destroy();
