// ADR 0015 step 3: the Codex app-server surface KetPlus uses must exist in every CLI version the
// compatibility manifest lists as tested, and every method the daemon calls must be declared.
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { extractSurface } from "./extract-surface.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "../../..");
const readJson = (file) => JSON.parse(fs.readFileSync(file, "utf8"));
const usage = readJson(path.join(here, "ketplus-usage.json"));
const manifest = readJson(path.join(root, "apps/daemon/src/providers/compat/provider-compatibility.json"));
const tested = manifest.providers.codex.tested;
const snapshot = (version) => readJson(path.join(here, "schemas", version, "surface.json"));
const kinds = ["requests", "notifications", "server_requests"];

// Files that talk to the Codex app-server directly.
const sources = [
  "apps/daemon/src/providers/CodexAdapter.cpp",
  ...fs
    .readdirSync(path.join(root, "apps/daemon/src/runtime"))
    .filter((name) => /^SessionHost.*\.cpp$/.test(name))
    .map((name) => `apps/daemon/src/runtime/${name}`),
  "modules/browser/src/CodexServerRequests.h",
];

test("every tested Codex version has a surface snapshot", () => {
  assert.ok(tested.length > 0);
  for (const version of tested) {
    const surface = snapshot(version);
    assert.equal(surface.provider, "codex");
    assert.equal(surface.version, version);
  }
});

test("declared usage exists in every tested snapshot", () => {
  for (const version of tested) {
    const surface = snapshot(version);
    const missing = [];
    for (const [method, entry] of Object.entries(usage.requests)) {
      const found = surface.requests[method];
      if (!found) {
        missing.push(`request ${method}`);
        continue;
      }
      for (const field of entry.params ?? [])
        if (!found.params.includes(field)) missing.push(`${method} params.${field}`);
      for (const field of entry.response ?? [])
        if (!(found.response ?? []).includes(field)) missing.push(`${method} response.${field}`);
    }
    for (const kind of ["notifications", "server_requests"]) {
      for (const [method, fields] of Object.entries(usage[kind])) {
        const found = surface[kind][method];
        if (!found) {
          missing.push(`${kind} ${method}`);
          continue;
        }
        for (const field of fields)
          if (!found.params.includes(field)) missing.push(`${method} ${field}`);
      }
    }
    assert.deepEqual(missing, [], `Codex ${version} lacks surface KetPlus uses`);
  }
});

test("every Codex method the daemon names is declared", () => {
  const surface = snapshot(tested[tested.length - 1]);
  const namespaces = new Set(
    kinds.flatMap((kind) => Object.keys(surface[kind])).map((method) => method.split("/")[0]),
  );
  const declared = new Set(kinds.flatMap((kind) => Object.keys(usage[kind])));
  const undeclared = new Set();
  for (const source of sources) {
    const text = fs.readFileSync(path.join(root, source), "utf8");
    for (const [, literal] of text.matchAll(/"([a-z][A-Za-z]*(?:\/[a-z][A-Za-z]*)+)"/g)) {
      if (namespaces.has(literal.split("/")[0]) && !declared.has(literal))
        undeclared.add(`${literal} (${source})`);
    }
  }
  assert.deepEqual([...undeclared], [], "add these to tests/providers/codex/ketplus-usage.json");
});

test("surface extraction follows references, unions and arrays", () => {
  const bundle = {
    definitions: {
      ClientRequest: {
        oneOf: [
          {
            properties: {
              method: { enum: ["turn/steer"] },
              params: { $ref: "#/definitions/TurnSteerParams" },
            },
          },
        ],
      },
      ServerNotification: { oneOf: [] },
      ServerRequest: { oneOf: [] },
      TurnSteerParams: {
        properties: {
          threadId: { type: "string" },
          input: { type: "array", items: { $ref: "#/definitions/Input" } },
        },
      },
      Input: {
        oneOf: [{ properties: { text: { type: "string" } } }, { properties: { url: { type: "string" } } }],
      },
      TurnSteerResponse: { properties: { turnId: { type: "string" } } },
    },
  };
  const surface = extractSurface(bundle, "1.0.0");
  assert.deepEqual(surface.requests["turn/steer"], {
    params: ["input", "input[].text", "input[].url", "threadId"],
    response: ["turnId"],
  });
});
