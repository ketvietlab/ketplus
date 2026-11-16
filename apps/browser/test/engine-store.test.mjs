import assert from "node:assert/strict";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { enrolledEngineIdentity } from "../src/engine-integrity.mjs";
import { ManagedEngineStore } from "../src/engine-store.mjs";

const ARCHIVE = Buffer.from("fixture-archive\0v1\n");
const ARCHIVE_HASH = "07f0877bcc28df52a334578989727262868a8e16749e9ba6fad1cf873bbf9903";
const TREE_HASH = process.platform === "win32"
  ? "87fd10f8d9a43c85330cdccff1d39d9ccbffe9bfb59b7a0db5d40a24d9a4ba52"
  : "2250e1c0e46a173dd506f3caac37a70f75050708fe8eb9790be7eb25341d164c";
const pin = enrolledEngineIdentity({
  schemaVersion: 1, playwrightVersion: "fixture", revision: "fixture", browserVersion: "fixture",
  platforms: {
    "fixture-fixture": {
      archiveSha256: ARCHIVE_HASH, treeSha256: TREE_HASH, executable: "bin/chrome",
      archiveUrl: "https://example.invalid/fixture.zip",
      provenance: { url: "https://example.invalid/fixture-identity", revision: "fixture-v1" },
    },
  },
}, { playwrightVersion: "fixture", revision: "fixture", browserVersion: "fixture", platform: "fixture", arch: "fixture" });

async function fixtureTree(tree) {
  await fs.mkdir(path.join(tree, "bin"));
  await fs.mkdir(path.join(tree, "resources"));
  await fs.writeFile(path.join(tree, "bin/chrome"), "fixture-browser\n", { mode: 0o755 });
  await fs.chmod(path.join(tree, "bin/chrome"), 0o755);
  await fs.writeFile(path.join(tree, "resources/data"), "fixture-data\n", { mode: 0o644 });
  await fs.chmod(path.join(tree, "resources/data"), 0o644);
}

async function fixture(t, overrides = {}) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-engine-store-"));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  const calls = { downloads: 0, extractions: 0 };
  const options = {
    cacheDir: path.join(root, "cache"), identity: pin,
    downloadArchive: async (url, archive) => {
      calls.downloads++;
      assert.equal(url, pin.archiveUrl);
      await fs.writeFile(archive, ARCHIVE);
    },
    extractArchive: async (bytes, tree) => {
      calls.extractions++;
      assert.deepEqual(bytes, ARCHIVE);
      await fixtureTree(tree);
    },
    ...overrides,
  };
  return { root, options, calls, store: new ManagedEngineStore(options) };
}

test("a valid identity publishes a verified tree and rechecks it on cache reuse", async (t) => {
  const { store, calls } = await fixture(t);
  assert.equal(await store.probe(), null);
  const executable = await store.install();
  assert.equal(executable, path.join(store.directory, "bin/chrome"));
  assert.equal(await store.probe(), executable);
  assert.equal(await store.install(), executable);
  assert.deepEqual(calls, { downloads: 1, extractions: 1 });
  assert.deepEqual(await fs.readdir(store.cacheDir), [path.basename(store.directory)]);
});

test("a replaced archive never reaches extraction or a discoverable directory", async (t) => {
  let extracted = false;
  const { store } = await fixture(t, {
    downloadArchive: (_url, archive) => fs.writeFile(archive, "replaced archive"),
    extractArchive: () => { extracted = true; },
  });
  await assert.rejects(store.install(), /archive SHA-256 mismatch/);
  assert.equal(extracted, false);
  assert.equal(await store.probe(), null);
  assert.deepEqual(await fs.readdir(store.cacheDir), []);
});

test("a mismatching extracted tree is never published", async (t) => {
  const { store } = await fixture(t, {
    extractArchive: async (_bytes, tree) => {
      await fixtureTree(tree);
      await fs.writeFile(path.join(tree, "bin/injected-library"), "injected");
    },
  });
  await assert.rejects(store.install(), /installed-tree SHA-256 mismatch/);
  assert.equal(await store.probe(), null);
  assert.deepEqual(await fs.readdir(store.cacheDir), []);
});

for (const phase of ["downloadArchive", "extractArchive"]) {
  test(`an interrupted ${phase} is cleaned up and a later attempt succeeds`, async (t) => {
    const setup = await fixture(t);
    const original = setup.options[phase];
    let interrupted = true;
    setup.options[phase] = async (...args) => {
      if (interrupted) {
        interrupted = false;
        if (phase === "downloadArchive") await fs.writeFile(args[1], "partial");
        else await fs.writeFile(path.join(args[1], "partial"), "partial");
        throw new Error("fixture interruption");
      }
      return original(...args);
    };
    const store = new ManagedEngineStore(setup.options);
    await assert.rejects(store.install(), /fixture interruption/);
    assert.equal(await store.probe(), null);
    assert.deepEqual(await fs.readdir(store.cacheDir), []);
    assert.equal(await store.install(), path.join(store.directory, "bin/chrome"));
  });
}

test("a stage left by a terminated installer is never discovered", async (t) => {
  const { store } = await fixture(t);
  const abandoned = path.join(store.cacheDir, ".verified-install-abandoned");
  await fs.mkdir(abandoned, { recursive: true, mode: 0o700 });
  await fs.writeFile(path.join(abandoned, "INSTALLATION_COMPLETE"), "forged receipt");
  assert.equal(await store.probe(), null);
  assert.equal(await store.install(), path.join(store.directory, "bin/chrome"));
});

test("tampering after installation fails both discovery and reuse without downloading", async (t) => {
  const { store, calls } = await fixture(t);
  const executable = await store.install();
  await fs.writeFile(executable, "replaced executable");
  await assert.rejects(store.probe(), /installed-tree SHA-256 mismatch/);
  await assert.rejects(store.install(), /installed-tree SHA-256 mismatch/);
  assert.deepEqual(calls, { downloads: 1, extractions: 1 });
});

test("a completion marker and a forged hash receipt confer no cache identity", async (t) => {
  const { store, calls } = await fixture(t);
  await fs.mkdir(store.directory, { recursive: true, mode: 0o700 });
  await fixtureTree(store.directory);
  await fs.writeFile(path.join(store.directory, "INSTALLATION_COMPLETE"), pin.treeSha256);
  await assert.rejects(store.probe(), /installed-tree SHA-256 mismatch/);
  assert.deepEqual(calls, { downloads: 0, extractions: 0 });
});

test("cache directory and published tree symlinks are rejected", async (t) => {
  const { store, root } = await fixture(t);
  const outside = path.join(root, "outside");
  await fs.mkdir(outside);
  await fs.symlink(outside, store.cacheDir, "dir");
  await assert.rejects(store.install(), /not a real directory/);
  await fs.unlink(store.cacheDir);
  await fs.mkdir(store.cacheDir, { mode: 0o700 });
  await fs.symlink(outside, store.directory, "dir");
  await assert.rejects(store.probe(), /not a real directory/);
});

test("a cache writable by other users is refused", { skip: process.platform === "win32" ? "POSIX ownership/mode fixture" : false }, async (t) => {
  const { store } = await fixture(t);
  await fs.mkdir(store.cacheDir, { mode: 0o777 });
  await fs.chmod(store.cacheDir, 0o777);
  await assert.rejects(store.install(), /owner-only/);
});

test("concurrent installs verify the winner instead of trusting a rename collision", async (t) => {
  const setup = await fixture(t);
  let count = 0;
  let release;
  const bothExtracted = new Promise((resolve) => { release = resolve; });
  setup.options.extractArchive = async (bytes, tree) => {
    assert.deepEqual(bytes, ARCHIVE);
    await fixtureTree(tree);
    if (++count === 2) release();
    await bothExtracted;
  };
  const first = new ManagedEngineStore(setup.options);
  const second = new ManagedEngineStore(setup.options);
  const result = await Promise.all([first.install(), second.install()]);
  assert.deepEqual(result, [path.join(first.directory, "bin/chrome"), path.join(first.directory, "bin/chrome")]);
  assert.deepEqual(await fs.readdir(first.cacheDir), [path.basename(first.directory)]);
});
