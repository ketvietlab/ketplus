// SPDX-License-Identifier: MPL-2.0
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { smokeRuntime } from "../scripts/runtime-smoke.mjs";

function fixture(t, source) {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), "ketplus-runtime-smoke-"));
  t.after(() => fs.rmSync(work, { recursive: true, force: true }));
  const stage = path.join(work, "stage");
  const library = path.join(stage, "node_modules/playwright-core/lib");
  fs.mkdirSync(library, { recursive: true });
  fs.mkdirSync(path.join(stage, "src"));
  fs.writeFileSync(path.join(stage, "src/browser-service.mjs"), source);
  fs.writeFileSync(path.join(library, "coreBundle.js"), "module.exports = {};\n");
  return { work, stage };
}

test("runtime import smoke has a private home and cannot inherit host configuration", (t) => {
  const { work, stage } = fixture(t, `
    import assert from 'node:assert/strict';
    import fs from 'node:fs';
    import os from 'node:os';
    import path from 'node:path';
    assert.equal(path.basename(os.homedir()), 'smoke-home');
    assert.ok(fs.statSync(os.homedir()).isDirectory());
    assert.equal(process.env.PATH, '/usr/bin:/bin');
    assert.equal(process.env.NODE_OPTIONS, undefined);
    assert.equal(process.env.PACKAGING_PARENT_ONLY, undefined);
    fs.writeFileSync(path.join(os.homedir(), 'probe'), 'isolated');
  `);
  const original = process.env.NODE_OPTIONS;
  const parentOnly = process.env.PACKAGING_PARENT_ONLY;
  process.env.NODE_OPTIONS = "--require=/missing-packaging-fixture-preload";
  process.env.PACKAGING_PARENT_ONLY = "must-not-reach-child";
  try {
    assert.equal(smokeRuntime(process.execPath, stage, work), process.version);
  } finally {
    if (original === undefined) delete process.env.NODE_OPTIONS;
    else process.env.NODE_OPTIONS = original;
    if (parentOnly === undefined) delete process.env.PACKAGING_PARENT_ONLY;
    else process.env.PACKAGING_PARENT_ONLY = parentOnly;
  }
  assert.equal(fs.readFileSync(path.join(work, "smoke-home/probe"), "utf8"), "isolated");
  if (process.platform !== "win32") assert.equal(fs.statSync(path.join(work, "smoke-home")).mode & 0o777, 0o700);
});

test("runtime import failures still fail the packaging probe", (t) => {
  const { work, stage } = fixture(t, "throw new Error('fixture import failure');");
  assert.throws(() => smokeRuntime(process.execPath, stage, work), /fixture import failure/);
});
