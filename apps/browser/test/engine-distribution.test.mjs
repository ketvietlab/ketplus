import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import { createRequire } from "node:module";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { enrolledEngineIdentity } from "../src/engine-integrity.mjs";

const appDir = fileURLToPath(new URL("../", import.meta.url));
const require = createRequire(import.meta.url);
const observed = JSON.parse(fs.readFileSync(new URL("./fixtures/engine-distribution.json", import.meta.url)));
const packageDir = path.dirname(require.resolve("playwright-core/package.json"));
const descriptor = JSON.parse(fs.readFileSync(path.join(packageDir, "browsers.json"))).browsers.find((entry) => entry.name === "chromium");

test("distribution observations bind to the exact lockfile-pinned registry", () => {
  const lock = JSON.parse(fs.readFileSync(path.join(appDir, "package-lock.json")));
  assert.equal(require("playwright-core/package.json").version, observed.playwrightVersion);
  assert.equal(lock.packages["node_modules/playwright-core"].version, observed.playwrightVersion);
  assert.equal(descriptor.revision, observed.playwrightRevision);
  assert.equal(descriptor.browserVersion, observed.browserVersion);
  assert.notEqual(observed.cftRevision, observed.playwrightRevision);
});

for (const mapping of observed.mappings) {
  test(`the pinned registry maps ${mapping.hostPlatform} to the observed CfT artifact path`, (t) => {
    const cache = fs.mkdtempSync(path.join(os.tmpdir(), "ketplus-registry-metadata-"));
    t.after(() => fs.rmSync(cache, { recursive: true, force: true }));
    const child = spawnSync(process.execPath, ["--input-type=module", "-e", `
      import { createRequire } from "node:module";
      const require = createRequire(import.meta.url);
      const executable = require("playwright-core/lib/coreBundle").registry.registry.findExecutable("chromium");
      process.stdout.write(JSON.stringify({ revision: executable.revision, version: executable.browserVersion, urls: executable.downloadURLs }));
    `], {
      cwd: appDir, encoding: "utf8", timeout: 5_000,
      env: {
        PATH: process.env.PATH,
        // Metadata inspection only; production installerEnv strips this override.
        PLAYWRIGHT_HOST_PLATFORM_OVERRIDE: mapping.hostPlatform,
        PLAYWRIGHT_BROWSERS_PATH: cache,
      },
    });
    assert.equal(child.error, undefined, child.error?.message);
    assert.equal(child.status, 0, child.stderr);
    const metadata = JSON.parse(child.stdout);
    assert.equal(metadata.revision, observed.playwrightRevision);
    assert.equal(metadata.version, observed.browserVersion);
    assert.deepEqual(metadata.urls, [`https://cdn.playwright.dev/builds/cft/${observed.browserVersion}/${mapping.cftPlatform}/${mapping.archive}`]);
    // This checks a distribution mapping, not archive byte equality or trust.
    assert.deepEqual(fs.readdirSync(cache), []);
  });
}

test("distribution metadata cannot be promoted to an enrolled engine identity", () => {
  assert.equal(observed.identityEnrolled, false);
  assert.equal(observed.kind, "distribution-metadata-not-identity");
  assert.throws(() => enrolledEngineIdentity(observed, {
    playwrightVersion: observed.playwrightVersion, revision: observed.playwrightRevision,
    browserVersion: observed.browserVersion, platform: "darwin", arch: "arm64",
  }), { code: "BROWSER_INTEGRITY" });
});
