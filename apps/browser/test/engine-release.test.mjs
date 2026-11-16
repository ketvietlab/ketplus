import assert from "node:assert/strict";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { bundledEngineRelease, launchVerifiedManagedEngine } from "../src/engine-release.mjs";
import { ManagedEngineStore } from "../src/engine-store.mjs";

// Synthetic contents and scope labels only; no Windows artifact or signing root.
const ARCHIVE = Buffer.from("fixture-archive\0v1\n");
const ARCHIVE_HASH = "07f0877bcc28df52a334578989727262868a8e16749e9ba6fad1cf873bbf9903";
const TREE_HASH = process.platform === "win32"
  ? "87fd10f8d9a43c85330cdccff1d39d9ccbffe9bfb59b7a0db5d40a24d9a4ba52"
  : "2250e1c0e46a173dd506f3caac37a70f75050708fe8eb9790be7eb25341d164c";
const binding = { releaseId: "fixture-app-release-1", playwrightVersion: "1.63.0", platform: "win32", arch: "x64" };
const revisions = { chromium: "1243", "chromium-headless-shell": "1243", ffmpeg: "1011", winldd: "1007" };

function request(component) {
  return { component, platform: "win32-x64", revision: revisions[component], browserVersion: component.startsWith("chromium") ? "153.0.8010.12" : null };
}

function manifest() {
  return {
    schemaVersion: 1, kind: "ketplus-managed-engine-release", ...binding,
    revoked: { identityIds: [], archiveSha256: [], treeSha256: [] },
    artifacts: Object.keys(revisions).map((component) => ({
      ...request(component), id: `fixture-${component}`, status: "enrolled",
      archiveUrl: `https://example.invalid/${component}.zip`, archiveSha256: ARCHIVE_HASH,
      executable: "bin/chrome", treeSha256: TREE_HASH,
      provenance: { url: "https://example.invalid/fixture-evidence", revision: "fixture-v1" },
    })),
  };
}

const plan = (browser = "chromium") => ({ browser: request(browser), helpers: [request("ffmpeg"), request("winldd")] });
const policy = (input = manifest(), context = binding) => bundledEngineRelease(Buffer.from(JSON.stringify(input)), context);

async function harness(t, { afterExtract = async () => {} } = {}) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), "ketplus-release-gate-"));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  const calls = { stores: 0, downloads: 0, extractions: 0, launches: 0 };
  const stores = new Map();
  const createStore = (identity) => {
    calls.stores++;
    assert.ok(Object.isFrozen(identity));
    const store = new ManagedEngineStore({
      cacheDir: path.join(root, identity.component), identity,
      downloadArchive: async (_url, archive) => { calls.downloads++; await fs.writeFile(archive, ARCHIVE); },
      extractArchive: async (bytes, tree) => {
        calls.extractions++;
        assert.deepEqual(bytes, ARCHIVE);
        await fs.mkdir(path.join(tree, "bin"));
        await fs.mkdir(path.join(tree, "resources"));
        await fs.writeFile(path.join(tree, "bin/chrome"), "fixture-browser\n", { mode: 0o755 });
        await fs.chmod(path.join(tree, "bin/chrome"), 0o755);
        await fs.writeFile(path.join(tree, "resources/data"), "fixture-data\n", { mode: 0o644 });
        await fs.chmod(path.join(tree, "resources/data"), 0o644);
        await afterExtract(identity, tree, stores);
      },
    });
    stores.set(identity.component, store);
    return store;
  };
  return {
    calls, stores,
    run: (release = policy(), launchPlan = plan()) => launchVerifiedManagedEngine({
      policy: release, plan: launchPlan, createStore,
      launch: (options) => {
        calls.launches++;
        assert.ok(Object.isFrozen(options));
        assert.ok(Object.isFrozen(options.helpers));
        return { marker: "fake-launch-only", ...options };
      },
    }),
  };
}

test("bundled metadata must match the application and pinned package release", () => {
  assert.throws(() => policy(manifest(), { ...binding, releaseId: "another-app-release" }), /application release/);
  assert.throws(() => policy(manifest(), { ...binding, playwrightVersion: "1.64.0" }), /application release/);
  assert.throws(() => bundledEngineRelease("{", binding), /manifest JSON/);
  assert.throws(() => bundledEngineRelease(" ".repeat(131_073), binding), /manifest bytes/);
});

for (const flavor of ["chromium", "chromium-headless-shell"]) {
  test(`the ${flavor} plan verifies every declared helper before a fake launch`, async (t) => {
    const setup = await harness(t);
    const result = await setup.run(policy(), plan(flavor));
    assert.equal(result.marker, "fake-launch-only");
    assert.equal(result.executablePath, path.join(setup.stores.get(flavor).directory, "bin/chrome"));
    assert.deepEqual(Object.keys(result.helpers), ["ffmpeg", "winldd"]);
    assert.deepEqual(setup.calls, { stores: 3, downloads: 3, extractions: 3, launches: 1 });
  });
}

for (const component of Object.keys(revisions)) {
  test(`a missing ${component} enrollment prevents all side effects`, async (t) => {
    const setup = await harness(t);
    const input = manifest();
    input.artifacts = input.artifacts.filter((entry) => entry.component !== component);
    await assert.rejects(setup.run(policy(input), plan(component === "chromium-headless-shell" ? component : "chromium")), /No enrolled identity/);
    assert.deepEqual(setup.calls, { stores: 0, downloads: 0, extractions: 0, launches: 0 });
  });
}

for (const component of Object.keys(revisions)) {
  test(`a revoked ${component} identity is refused before cache discovery`, async (t) => {
    const setup = await harness(t);
    const input = manifest();
    input.revoked.identityIds.push(`fixture-${component}`);
    await assert.rejects(setup.run(policy(input), plan(component === "chromium-headless-shell" ? component : "chromium")), /revoked/);
    assert.equal(setup.calls.stores, 0);
    assert.equal(setup.calls.launches, 0);
  });
}

for (const field of ["archiveSha256", "treeSha256"]) {
  test(`revocation by ${field} cannot be bypassed by an identity alias`, async (t) => {
    const setup = await harness(t);
    const input = manifest();
    input.artifacts[0].id = "renamed-identity";
    input.revoked[field].push(field === "archiveSha256" ? ARCHIVE_HASH : TREE_HASH);
    await assert.rejects(setup.run(policy(input)), /revoked/);
    assert.equal(setup.calls.stores, 0);
  });
}

test("a new app release's revocation defeats an unchanged cache and its receipts", async (t) => {
  const setup = await harness(t);
  await setup.run();
  const before = { ...setup.calls };
  const input = manifest();
  input.releaseId = "fixture-app-release-2";
  input.revoked.identityIds.push("fixture-ffmpeg");
  // A cache-local receipt claims permission from the old release. The gate does
  // not read it, even when the cached executable bytes still match that release.
  await fs.writeFile(path.join(setup.stores.get("ffmpeg").cacheDir, "receipt.json"), JSON.stringify({ permitted: true, releaseId: binding.releaseId }));
  await assert.rejects(setup.run(policy(input, { ...binding, releaseId: input.releaseId })), /revoked/);
  assert.deepEqual(setup.calls, before);
});

for (const changed of ["chromium", "ffmpeg"]) {
  test(`${changed} cache contents are rechecked after helper installation before fake launch`, async (t) => {
    const setup = await harness(t, {
      afterExtract: async (identity, _tree, stores) => {
        if (identity.component === "winldd") await fs.writeFile(path.join(stores.get(changed).directory, "bin/chrome"), "replaced while installing helper");
      },
    });
    await assert.rejects(setup.run(), /installed-tree SHA-256 mismatch/);
    assert.equal(setup.calls.launches, 0);
  });
}

test("removing an identity in the next release defeats a previously verified cache", async (t) => {
  const setup = await harness(t);
  await setup.run();
  const before = { ...setup.calls };
  const input = manifest();
  input.artifacts = input.artifacts.filter((entry) => entry.component !== "chromium");
  input.releaseId = "fixture-app-release-2";
  await assert.rejects(setup.run(policy(input, { ...binding, releaseId: input.releaseId })), /No enrolled identity/);
  assert.deepEqual(setup.calls, before);
});

test("an interrupted helper install prevents launch and a later attempt recovers", async (t) => {
  let interrupted = true;
  const setup = await harness(t, {
    afterExtract: async (identity) => {
      if (identity.component === "winldd" && interrupted) throw new Error("fixture helper interruption");
    },
  });
  await assert.rejects(setup.run(), /fixture helper interruption/);
  assert.equal(setup.calls.launches, 0);
  assert.equal(await setup.stores.get("winldd").probe(), null);
  interrupted = false;
  await setup.run();
  assert.equal(setup.calls.launches, 1);
  assert.equal(setup.calls.downloads, 4);
});

test("a caller cannot remove helpers from an in-flight plan after selection", async (t) => {
  const launchPlan = plan();
  const setup = await harness(t, {
    afterExtract: async (identity) => {
      if (identity.component === "chromium") launchPlan.helpers.length = 0;
    },
  });
  const result = await setup.run(policy(), launchPlan);
  assert.deepEqual(Object.keys(result.helpers), ["ffmpeg", "winldd"]);
  assert.equal(setup.calls.downloads, 3);
});

test("a changed path returned at the final probe never reaches the launcher", async () => {
  let launched = false;
  await assert.rejects(launchVerifiedManagedEngine({
    policy: policy(), plan: plan(),
    createStore: () => ({ install: async () => "/fixture/original", probe: async () => "/fixture/replaced" }),
    launch: () => { launched = true; },
  }), /executable changed before launch/);
  assert.equal(launched, false);
});

test("a mismatching helper pin prevents all side effects", async (t) => {
  const setup = await harness(t);
  const input = manifest();
  delete input.artifacts.find((entry) => entry.component === "ffmpeg").archiveSha256;
  await assert.rejects(setup.run(policy(input)), /Missing or invalid/);
  assert.equal(setup.calls.stores, 0);
});

test("unreviewed flavors, helper versions and host overrides cannot select another identity", async (t) => {
  const setup = await harness(t);
  for (const changed of [
    { ...plan(), browser: { ...request("chromium"), component: "custom" } },
    { ...plan(), helpers: [{ ...request("ffmpeg"), revision: "9999" }] },
    { ...plan(), browser: { ...request("chromium"), platform: "linux-x64" } },
    { ...plan(), helpers: [{ ...request("ffmpeg"), platform: "darwin-arm64" }] },
    { ...plan(), helpers: [request("ffmpeg"), request("ffmpeg")] },
  ]) await assert.rejects(setup.run(policy(), changed), { code: "BROWSER_INTEGRITY" });
  assert.equal(setup.calls.stores, 0);
});

test("ambiguous identities and malformed revocation inventories fail closed", () => {
  const duplicate = manifest();
  duplicate.artifacts.push({ ...duplicate.artifacts[0], id: "different-id-same-scope" });
  assert.throws(() => policy(duplicate), /Duplicate/);
  for (const revoked of [undefined, {}, { identityIds: [], archiveSha256: ["not-a-hash"], treeSha256: [] }]) {
    assert.throws(() => policy({ ...manifest(), revoked }), /revocation inventory/);
  }
  const pending = manifest();
  pending.artifacts[0].status = "pending";
  assert.throws(() => policy(pending), /unenrolled/);
});

test("the parsed release inventory is isolated from later mutation of caller buffers", () => {
  const input = manifest();
  const bytes = Buffer.from(JSON.stringify(input));
  const selected = bundledEngineRelease(bytes, binding);
  bytes.fill(0);
  input.artifacts.length = 0;
  assert.equal(selected.plan(plan()).length, 3);
  assert.ok(Object.isFrozen(selected));
});
