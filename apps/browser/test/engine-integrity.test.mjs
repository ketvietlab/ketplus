import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { enrolledEngineIdentity, measureEngineTree, verifiedEngineArchive, verifyEngineTree } from "../src/engine-integrity.mjs";

// Synthetic bytes only. These are fixed contract snapshots, not browser identities.
const ARCHIVE = Buffer.from("fixture-archive\0v1\n");
const ARCHIVE_HASH = "07f0877bcc28df52a334578989727262868a8e16749e9ba6fad1cf873bbf9903";
const TREE_HASH = process.platform === "win32"
  ? "87fd10f8d9a43c85330cdccff1d39d9ccbffe9bfb59b7a0db5d40a24d9a4ba52"
  : "2250e1c0e46a173dd506f3caac37a70f75050708fe8eb9790be7eb25341d164c";
const identity = { playwrightVersion: "1.63.0", revision: "1243", browserVersion: "153.0.8010.12", platform: "darwin", arch: "arm64" };
const manifest = () => ({
  schemaVersion: 1,
  playwrightVersion: identity.playwrightVersion,
  revision: identity.revision,
  browserVersion: identity.browserVersion,
  platforms: {
    "darwin-arm64": {
      archiveSha256: ARCHIVE_HASH,
      treeSha256: TREE_HASH,
      archiveUrl: "https://example.invalid/fixture-archive.zip",
      executable: "bin/chrome",
      provenance: { url: "https://example.invalid/fixture-identity", revision: "fixture-v1" },
    },
  },
});

async function temporary(t) {
  const dir = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-engine-integrity-"));
  t.after(() => fs.rm(dir, { recursive: true, force: true }));
  return dir;
}

async function fixtureTree(t) {
  const dir = await temporary(t);
  await fs.mkdir(path.join(dir, "bin"));
  await fs.mkdir(path.join(dir, "resources"));
  await fs.writeFile(path.join(dir, "bin/chrome"), "fixture-browser\n", { mode: 0o755 });
  await fs.chmod(path.join(dir, "bin/chrome"), 0o755);
  await fs.writeFile(path.join(dir, "resources/data"), "fixture-data\n", { mode: 0o644 });
  await fs.chmod(path.join(dir, "resources/data"), 0o644);
  return dir;
}

test("identity selection requires exact package, browser revision, version and platform", () => {
  const selected = enrolledEngineIdentity(manifest(), identity);
  assert.equal(selected.archiveSha256, ARCHIVE_HASH);
  assert.equal(selected.treeSha256, TREE_HASH);
  assert.ok(Object.isFrozen(selected));
  for (const change of [{ playwrightVersion: "1.64.0" }, { revision: "1244" }, { browserVersion: "0.0.0.0" }, { platform: "linux" }, { arch: "x64" }]) {
    assert.throws(() => enrolledEngineIdentity(manifest(), { ...identity, ...change }), { code: "BROWSER_INTEGRITY" });
  }
});

test("missing pins and enrollment evidence cannot become trust on first use", () => {
  for (const value of [undefined, "", "0".repeat(63), "0".repeat(65), "g".repeat(64)]) {
    for (const field of ["archiveSha256", "treeSha256"]) {
      const input = manifest();
      input.platforms["darwin-arm64"][field] = value;
      assert.throws(() => enrolledEngineIdentity(input, identity), { code: "BROWSER_INTEGRITY" });
    }
  }
  const input = manifest();
  delete input.platforms["darwin-arm64"].provenance;
  assert.throws(() => enrolledEngineIdentity(input, identity), /enrollment evidence/);
  assert.throws(() => enrolledEngineIdentity({ ...manifest(), platforms: {} }, identity), /No independently verified/);
});

test("enrolled URLs and executable paths cannot redirect to local or escaping inputs", () => {
  for (const archiveUrl of ["http://example.invalid/a.zip", "file:///archive.zip", "https://user:password@example.invalid/a.zip", "https://example.invalid/a.zip#fragment", "invalid"]) {
    const input = manifest();
    input.platforms["darwin-arm64"].archiveUrl = archiveUrl;
    assert.throws(() => enrolledEngineIdentity(input, identity), /Invalid enrolled archive URL/);
  }
  for (const executable of ["../chrome", "/chrome", "bin/../chrome", "C:\\chrome", "bin//chrome", "bin/./chrome", ""]) {
    const input = manifest();
    input.platforms["darwin-arm64"].executable = executable;
    assert.throws(() => enrolledEngineIdentity(input, identity), /Invalid enrolled executable path/);
  }
});

test("archive verification returns authenticated bytes even if the original path is replaced", async (t) => {
  const dir = await temporary(t);
  const archive = path.join(dir, "archive.zip");
  await fs.writeFile(archive, ARCHIVE);
  const authenticated = await verifiedEngineArchive(archive, ARCHIVE_HASH);
  await fs.writeFile(archive, "substituted archive");
  assert.deepEqual(authenticated, ARCHIVE);
  await assert.rejects(verifiedEngineArchive(archive, ARCHIVE_HASH), /archive SHA-256 mismatch/);
  await assert.rejects(verifiedEngineArchive(archive, undefined), /Missing or invalid/);
});

test("archive readers reject directories, symlinks and oversized sparse files", async (t) => {
  const dir = await temporary(t);
  await assert.rejects(verifiedEngineArchive(dir, ARCHIVE_HASH), /not a regular file/);
  const file = path.join(dir, "archive.zip");
  await fs.writeFile(file, ARCHIVE);
  const link = path.join(dir, "archive-link.zip");
  await fs.symlink(file, link);
  await assert.rejects(verifiedEngineArchive(link, ARCHIVE_HASH), /not a regular file/);
  await fs.truncate(file, 1_073_741_825);
  await assert.rejects(verifiedEngineArchive(file, ARCHIVE_HASH), /bounded regular file/);
});

test("a FIFO substituted after the path precheck is rejected within a watchdog", { skip: process.platform === "win32" ? "POSIX FIFO fixture" : false }, async (t) => {
  const dir = await temporary(t);
  const file = path.join(dir, "archive.zip");
  await fs.writeFile(file, ARCHIVE);
  const moduleUrl = new URL("../src/engine-integrity.mjs", import.meta.url).href;
  const script = `
    import fs from "node:fs/promises";
    import { execFileSync } from "node:child_process";
    import { verifiedEngineArchive } from ${JSON.stringify(moduleUrl)};
    const open = fs.open.bind(fs);
    fs.open = async (file, ...args) => {
      await fs.unlink(file);
      execFileSync("mkfifo", [file]);
      return open(file, ...args);
    };
    try { await verifiedEngineArchive(process.argv[1], process.argv[2]); process.exitCode = 1; }
    catch (error) { if (error.code !== "BROWSER_INTEGRITY") throw error; }
  `;
  const child = spawnSync(process.execPath, ["--input-type=module", "-e", script, file, ARCHIVE_HASH], {
    timeout: 5_000, killSignal: "SIGKILL", encoding: "utf8", env: { PATH: process.env.PATH },
  });
  assert.equal(child.error, undefined, child.error?.message);
  assert.equal(child.signal, null);
  assert.equal(child.status, 0, child.stderr);
});

test("the installed-tree hash is a stable enrollment contract", async (t) => {
  const dir = await fixtureTree(t);
  assert.equal(await measureEngineTree(dir), TREE_HASH);
  await verifyEngineTree(dir, TREE_HASH);
  // Times and non-executable permission changes do not alter content identity.
  await fs.utimes(path.join(dir, "resources/data"), 1, 1);
  await fs.chmod(path.join(dir, "resources/data"), 0o600);
  await verifyEngineTree(dir, TREE_HASH);
});

for (const [name, mutate] of [
  ["executable replacement", (dir) => fs.writeFile(path.join(dir, "bin/chrome"), "substituted browser\n")],
  ["resource replacement", (dir) => fs.writeFile(path.join(dir, "resources/data"), "substituted resource\n")],
  ["injected loader", (dir) => fs.writeFile(path.join(dir, "bin/loader.dylib"), "substituted loader\n")],
  ["missing resource", (dir) => fs.unlink(path.join(dir, "resources/data"))],
  ["cache-local receipt", (dir) => fs.writeFile(path.join(dir, "INSTALLATION_COMPLETE"), ARCHIVE_HASH)],
  ["unexpected empty directory", (dir) => fs.mkdir(path.join(dir, "extra"))],
  ["changed execute permission", (dir) => fs.chmod(path.join(dir, "resources/data"), 0o755)],
]) {
  test(`installed-tree verification rejects ${name}`, {
    skip: name === "changed execute permission" && process.platform === "win32" ? "Windows has no POSIX execute permission bit" : false,
  }, async (t) => {
    const dir = await fixtureTree(t);
    await mutate(dir);
    await assert.rejects(verifyEngineTree(dir, TREE_HASH), /installed-tree SHA-256 mismatch/);
  });
}

test("tree validation never follows root links or links outside the authenticated tree", async (t) => {
  const dir = await fixtureTree(t);
  const link = `${dir}-link`;
  t.after(() => fs.unlink(link));
  await fs.symlink(dir, link, "dir");
  await assert.rejects(verifyEngineTree(link, TREE_HASH), /not a real directory/);
  await fs.symlink("../../outside", path.join(dir, "resources/link"));
  await assert.rejects(verifyEngineTree(dir, TREE_HASH), /link escapes/);
});

test("tree links are measured without following them", async (t) => {
  const dir = await fixtureTree(t);
  await fs.symlink("data", path.join(dir, "resources/link"));
  const enrolledWithLink = await measureEngineTree(dir);
  assert.notEqual(enrolledWithLink, TREE_HASH);
  await verifyEngineTree(dir, enrolledWithLink);
  await fs.unlink(path.join(dir, "resources/link"));
  await fs.symlink("missing", path.join(dir, "resources/link"));
  await assert.rejects(verifyEngineTree(dir, enrolledWithLink), /installed-tree SHA-256 mismatch/);
});

test("non-regular tree entries fail before being read", { skip: process.platform === "win32" ? "POSIX FIFO fixture" : false }, async (t) => {
  const dir = await fixtureTree(t);
  const fifo = path.join(dir, "resources/pipe");
  assert.equal(spawnSync("mkfifo", [fifo]).status, 0);
  await assert.rejects(verifyEngineTree(dir, TREE_HASH), /non-regular entry/);
});
