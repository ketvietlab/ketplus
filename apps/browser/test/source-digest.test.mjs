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

test("the build-time check refuses a runtime the sources moved away from", (t) => {
  try {
    execFileSync("cmake", ["--version"], { stdio: "ignore" });
  } catch {
    return t.skip("cmake not found");
  }
  const dir = fixture(t);
  const runtime = fs.mkdtempSync(path.join(os.tmpdir(), "ketplus-runtime-check-"));
  t.after(() => fs.rmSync(runtime, { recursive: true, force: true }));
  const stage = () =>
    fs.writeFileSync(path.join(runtime, "runtime.json"), JSON.stringify({ sourceSha256: sourceDigest(dir) }));
  const check = (allowStale = "OFF") =>
    spawnSync("cmake", [`-DKETPLUS_BROWSER_APP_DIR=${dir}`, `-DKETPLUS_BROWSER_RUNTIME_DIR=${runtime}`,
      `-DKETPLUS_BROWSER_RUNTIME_ALLOW_STALE=${allowStale}`, "-P", cmakeModule], { encoding: "utf8" });

  stage();
  assert.equal(check().status, 0, check().stderr);
  // A source edited after configure.
  fs.writeFileSync(path.join(dir, "src/nested/policy.mjs"), "export const a = 2;");
  assert.notEqual(check().status, 0);
  // CMake wraps diagnostics differently depending on the temporary path length.
  assert.match(check().stderr, /was\s+staged\s+from\s+sources/);
  assert.equal(check("ON").status, 0);
  stage();
  // A source added after configure.
  fs.writeFileSync(path.join(dir, "src/added.mjs"), "export {};");
  assert.notEqual(check().status, 0);
  stage();
  // A source removed after configure.
  fs.rmSync(path.join(dir, "src/added.mjs"));
  assert.notEqual(check().status, 0);
  stage();
  assert.equal(check().status, 0);
  // A runtime.json replaced after configure.
  fs.writeFileSync(path.join(runtime, "runtime.json"), JSON.stringify({ sourceSha256: "0".repeat(64) }));
  assert.notEqual(check().status, 0);
});
