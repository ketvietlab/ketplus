import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { sourceDigest } from "../scripts/source-digest.mjs";

const appDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const cmakeModule = path.resolve(appDir, "../../cmake/KetPlusBrowserRuntime.cmake");

function cmakeDigest(dir) {
  try {
    execFileSync("cmake", ["--version"], { stdio: "ignore" });
  } catch {
    return null;
  }
  // message() writes to stderr.
  const result = spawnSync("cmake", [`-DKETPLUS_BROWSER_APP_DIR=${dir}`, "-P", cmakeModule], {
    encoding: "utf8",
  });
  assert.equal(result.status, 0, result.stderr);
  return result.stderr.trim();
}

function fixture(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ketplus-source-digest-"));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  for (const [file, text] of [
    ["package.json", "{}"],
    ["package-lock.json", "{}"],
    ["src/mcp.mjs", "export {};"],
    ["src/nested/policy.mjs", "export const a = 1;"],
    [".claude/skills/ketplus-browser/SKILL.md", "# skill"],
  ]) {
    fs.mkdirSync(path.dirname(path.join(dir, file)), { recursive: true });
    fs.writeFileSync(path.join(dir, file), text);
  }
  return dir;
}

test("the staged source digest matches CMake's and changes with any source", (t) => {
  const dir = fixture(t);
  const before = sourceDigest(dir);
  assert.match(before, /^[0-9a-f]{64}$/);
  const fromCmake = cmakeDigest(dir);
  if (fromCmake === null) t.diagnostic("cmake not found; CMake parity not checked");
  else assert.equal(fromCmake, before);
  fs.writeFileSync(path.join(dir, "src/nested/policy.mjs"), "export const a = 2;");
  assert.notEqual(sourceDigest(dir), before);
  fs.writeFileSync(path.join(dir, "src/nested/policy.mjs"), "export const a = 1;");
  assert.equal(sourceDigest(dir), before);
  fs.writeFileSync(path.join(dir, ".claude/skills/ketplus-browser/extra.md"), "x");
  assert.notEqual(sourceDigest(dir), before);
});

test("the repository's own sources digest the same way in Node and CMake", (t) => {
  const fromCmake = cmakeDigest(appDir);
  if (fromCmake === null) return t.skip("cmake not found");
  assert.equal(fromCmake, sourceDigest(appDir));
});
