import assert from "node:assert/strict";
import { mkdtemp, mkdir, readFile, rm, utimes, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { ensureResumable, projectFolder } from "../resume.mjs";

test("copies a transcript into the new cwd's project folder", async t => {
  const config = await mkdtemp(path.join(os.tmpdir(), "resume-"));
  t.after(() => rm(config, { recursive: true, force: true }));
  const old = path.join(config, "projects", projectFolder("/repo/feature"));
  await mkdir(old, { recursive: true });
  await writeFile(path.join(old, "abc-1.jsonl"), "{}\n");
  assert.equal(await ensureResumable("abc-1", "/repo", config), true);
  const copied = path.join(config, "projects", projectFolder("/repo"), "abc-1.jsonl");
  assert.equal(await readFile(copied, "utf8"), "{}\n");
  assert.equal(await readFile(path.join(old, "abc-1.jsonl"), "utf8"), "{}\n");
});

test("returning A → B → A resumes the newest history and preserves both copies", async t => {
  const config = await mkdtemp(path.join(os.tmpdir(), "resume-"));
  t.after(() => rm(config, { recursive: true, force: true }));
  const a = path.join(config, "projects", projectFolder("/repo/a"), "abc-1.jsonl");
  const b = path.join(config, "projects", projectFolder("/repo/b"), "abc-1.jsonl");
  await mkdir(path.dirname(a), { recursive: true });
  await writeFile(a, "first turn\n");
  assert.equal(await ensureResumable("abc-1", "/repo/b", config), true);
  await writeFile(b, "first turn\nsecond turn\n");
  await utimes(a, 100, 100);
  await utimes(b, 200, 200);
  assert.equal(await ensureResumable("abc-1", "/repo/a", config), true);
  assert.equal(await readFile(a, "utf8"), "first turn\nsecond turn\n");
  assert.equal(await readFile(b, "utf8"), "first turn\nsecond turn\n");
  await writeFile(a, "first turn\nsecond turn\nthird turn\n");
  assert.equal(await ensureResumable("abc-1", "/repo/b", config), true);
  assert.equal(await readFile(b, "utf8"), await readFile(a, "utf8"));
});

test("ignores unknown sessions and unsafe ids", async t => {
  const config = await mkdtemp(path.join(os.tmpdir(), "resume-"));
  t.after(() => rm(config, { recursive: true, force: true }));
  assert.equal(await ensureResumable("missing", "/repo", config), false);
  assert.equal(await ensureResumable("../x", "/repo", config), false);
});
