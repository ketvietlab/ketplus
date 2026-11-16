import assert from "node:assert/strict";
import test from "node:test";
import { BrowserService } from "../src/browser-service.mjs";
import { defaultBrowserCacheDir, enginePreference, probeEngine, selectEngine, systemBrowserCandidates } from "../src/engine.mjs";

function fakeProbes({ chrome = null, msedge = null, cached = null, install = null } = {}) {
  const calls = [];
  return {
    calls,
    findSystemBrowser: (channel) => {
      calls.push(`system:${channel}`);
      return { chrome, msedge }[channel];
    },
    findBundledBrowser: async (dir) => {
      calls.push(`probe:${dir}`);
      return cached;
    },
    installBundledBrowser: async (dir) => {
      calls.push(`install:${dir}`);
      if (install instanceof Error) throw install;
      return install;
    },
  };
}

test("auto prefers system Chrome, then Edge, without touching the cache", async () => {
  let probes = fakeProbes({ chrome: "/c", msedge: "/e" });
  assert.deepEqual(await selectEngine({ cacheDir: "/cache", probes }), { name: "chrome", source: "system", executablePath: "/c" });
  assert.deepEqual(probes.calls, ["system:chrome"]);

  probes = fakeProbes({ msedge: "/e" });
  assert.deepEqual(await selectEngine({ cacheDir: "/cache", probes }), { name: "msedge", source: "system", executablePath: "/e" });
  assert.deepEqual(probes.calls, ["system:chrome", "system:msedge"]);
});

test("auto falls back to cached, then downloaded Chrome for Testing", async () => {
  let probes = fakeProbes({ cached: "/cache/cft" });
  assert.deepEqual(await selectEngine({ cacheDir: "/cache", probes }), { name: "bundled", source: "ketplus-cache", executablePath: "/cache/cft" });
  assert.ok(!probes.calls.some((call) => call.startsWith("install")));

  probes = fakeProbes({ install: "/cache/new" });
  assert.deepEqual(await selectEngine({ cacheDir: "/cache", probes }), { name: "bundled", source: "downloaded", executablePath: "/cache/new" });
  assert.deepEqual(probes.calls.at(-1), "install:/cache");
});

test("offline or disabled downloads fail with BROWSER_UNAVAILABLE", async () => {
  await assert.rejects(selectEngine({ cacheDir: "/cache", probes: fakeProbes({ install: new Error("getaddrinfo ENOTFOUND") }) }), (error) => {
    assert.equal(error.code, "BROWSER_UNAVAILABLE");
    assert.match(error.message, /Neither Google Chrome nor Microsoft Edge/);
    assert.match(error.message, /ENOTFOUND/);
    return true;
  });
  const probes = fakeProbes();
  await assert.rejects(selectEngine({ cacheDir: "/cache", allowDownload: false, probes }), { code: "BROWSER_UNAVAILABLE" });
  assert.ok(!probes.calls.some((call) => call.startsWith("install")));
});

test("explicit engine preferences do not fall back", async () => {
  const probes = fakeProbes({ chrome: "/c", cached: "/cache/cft" });
  await assert.rejects(selectEngine({ preference: "msedge", probes }), { code: "BROWSER_UNAVAILABLE" });
  assert.equal((await selectEngine({ preference: "bundled", cacheDir: "/cache", probes })).name, "bundled");
  assert.equal((await selectEngine({ preference: "chrome", probes })).executablePath, "/c");
  assert.deepEqual(await selectEngine({ preference: "playwright", probes }), { name: "playwright", source: "playwright-cache", executablePath: null });
  assert.equal(enginePreference({ KETPLUS_BROWSER_ENGINE: " MSEdge " }), "msedge");
  assert.equal(enginePreference({}), "auto");
  assert.throws(() => enginePreference({ KETPLUS_BROWSER_ENGINE: "firefox" }), { code: "BROWSER_UNAVAILABLE" });
});

test("cache and system paths follow the platform conventions", () => {
  assert.equal(
    defaultBrowserCacheDir({ platform: "darwin", env: {}, homedir: "/Users/u" }),
    "/Users/u/Library/Application Support/Ketsuite/KetPlus/browser-runtime/chrome",
  );
  assert.equal(
    defaultBrowserCacheDir({ platform: "linux", env: { XDG_DATA_HOME: "/x" }, homedir: "/h" }),
    "/x/Ketsuite/KetPlus/browser-runtime/chrome",
  );
  assert.deepEqual(systemBrowserCandidates("chrome", { platform: "darwin", homedir: "/Users/u" }), [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Users/u/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  ]);
  assert.deepEqual(systemBrowserCandidates("firefox"), []);
});

test("an unavailable engine surfaces as BROWSER_UNAVAILABLE from session.create", async () => {
  const service = new BrowserService({
    engine: { preference: "auto", cacheDir: "/cache", allowDownload: false, probes: fakeProbes() },
  });
  try {
    assert.equal(service.capabilities().engine.preference, "auto");
    assert.equal(service.capabilities().engine.name, null);
    await assert.rejects(service.dispatch("browser.session.create", {}), { code: "BROWSER_UNAVAILABLE" });
  } finally {
    await service.close();
  }
});

test("installer child env drops download host overrides and Node preloads", async () => {
  const { installerEnv } = await import("../src/engine.mjs");
  const env = installerEnv("/cache", {
    PATH: "/usr/bin",
    HTTPS_PROXY: "http://proxy:3128",
    NODE_OPTIONS: "--require /tmp/evil.js",
    NODE_TLS_REJECT_UNAUTHORIZED: "0",
    PLAYWRIGHT_DOWNLOAD_HOST: "https://evil.example",
    PLAYWRIGHT_CHROMIUM_DOWNLOAD_HOST: "https://evil.example",
    playwright_download_host: "https://evil.example",
    PLAYWRIGHT_BROWSERS_PATH: "/elsewhere",
  });
  assert.deepEqual(env, { PATH: "/usr/bin", HTTPS_PROXY: "http://proxy:3128", PLAYWRIGHT_BROWSERS_PATH: "/cache" });
});

test("a Chrome for Testing install without INSTALLATION_COMPLETE is not used and is redone", async () => {
  const fsp = await import("node:fs/promises");
  const os = await import("node:os");
  const path = await import("node:path");
  const { runInstaller } = await import("../src/engine.mjs");
  const tmp = await fsp.mkdtemp(path.join(os.tmpdir(), "ketplus-cft-cache-"));
  const cache = path.join(tmp, "browser-runtime", "chrome");
  try {
    const where = JSON.parse(await runInstaller("--where", cache, 15_000, {}));
    assert.ok(where.executable.startsWith(where.directory + path.sep));
    await fsp.mkdir(path.dirname(where.executable), { recursive: true });
    await fsp.writeFile(where.executable, "#!/bin/sh\n", { mode: 0o755 });

    // Executable present, marker missing: a partial install.
    assert.equal(await runInstaller("--probe", cache, 15_000, {}), null);
    // --install removes it before downloading; an unreachable proxy makes the download fail fast.
    await assert.rejects(runInstaller("--install", cache, 120_000, { PATH: process.env.PATH, HTTPS_PROXY: "http://127.0.0.1:9", HTTP_PROXY: "http://127.0.0.1:9" }));
    await assert.rejects(fsp.access(where.executable));
    assert.equal((await fsp.stat(cache)).mode & 0o777, 0o700);

    // Marker present: reused as is.
    await fsp.mkdir(path.dirname(where.executable), { recursive: true });
    await fsp.writeFile(where.executable, "#!/bin/sh\n", { mode: 0o755 });
    await fsp.writeFile(where.marker, "");
    assert.equal(await runInstaller("--probe", cache, 15_000, {}), where.executable);
    assert.equal(await runInstaller("--install", cache, 15_000, { HTTPS_PROXY: "http://127.0.0.1:9" }), where.executable);
  } finally {
    await fsp.rm(tmp, { recursive: true, force: true });
  }
});

test("visual.compare blocks captures from different browser engines", async () => {
  const fsp = await import("node:fs/promises");
  const os = await import("node:os");
  const path = await import("node:path");
  const { compareSnapshots } = await import("../src/comparator.mjs");
  const environment = { viewport: { width: 10, height: 10 }, deviceScaleFactor: 1, theme: "light", locale: "en-US", reducedMotion: "reduce", browserVersion: "140.0" };
  const snapshot = { nodes: [] };
  const dir = await fsp.mkdtemp(path.join(os.tmpdir(), "ketplus-engine-compare-"));
  try {
    const result = await compareSnapshots({
      referenceSnapshot: snapshot,
      candidateSnapshot: snapshot,
      referenceCapture: { environment: { ...environment, browserEngine: "chrome" } },
      candidateCapture: { environment: { ...environment, browserEngine: "bundled" } },
      diffPath: path.join(dir, "diff.png"),
    });
    assert.equal(result.status, "blocked");
    assert.deepEqual(result.environmentMismatch, ["browserEngine"]);
  } finally {
    await fsp.rm(dir, { recursive: true, force: true });
  }
});

test("package-runtime refuses dangerous --out directories", async () => {
  const { execFileSync } = await import("node:child_process");
  const os = await import("node:os");
  const path = await import("node:path");
  const { fileURLToPath } = await import("node:url");
  const appDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
  const check = (out) => {
    try {
      execFileSync(process.execPath, [path.join(appDir, "scripts", "package-runtime.mjs"), "--out", out], {
        env: { PATH: process.env.PATH, HOME: os.homedir(), TMPDIR: os.tmpdir(), KETPLUS_PACKAGE_RUNTIME_CHECK_OUT_ONLY: "1" },
        stdio: "pipe",
      });
      return 0;
    } catch (error) {
      return error.status;
    }
  };
  for (const out of [" ", "/", os.homedir(), path.dirname(os.homedir()), path.resolve(appDir, "..", ".."), appDir, os.tmpdir()]) {
    assert.equal(check(out), 3, out);
  }
  assert.equal(check(path.join(os.tmpdir(), "ketplus-stage-check", "ketplus-browser")), 0);
});


test("Settings probe follows engine selection and never installs a browser", async () => {
  for (const available of [{ chrome: "/chrome" }, { msedge: "/edge" }, { cached: "/cached" }]) {
    const probes = fakeProbes(available);
    const result = await probeEngine({ env: {}, probes });
    assert.equal(result.available, true);
    assert.equal(result.downloadRequired, false);
    assert.ok(!probes.calls.some((call) => call.startsWith("install:")));
  }
  const probes = fakeProbes();
  assert.deepEqual((await probeEngine({ env: {}, probes })).downloadRequired, true);
  assert.equal((await probeEngine({ env: { KETPLUS_BROWSER_DOWNLOAD: "0" }, probes })).available, false);
  assert.equal((await probeEngine({ env: { KETPLUS_BROWSER_ENGINE: "chrome" }, probes })).available, false);
  assert.equal((await probeEngine({ env: { KETPLUS_BROWSER_ENGINE: "playwright" }, probes })).available, false);
  assert.equal((await probeEngine({ env: { KETPLUS_BROWSER_ENGINE: "invalid" }, probes })).available, false);
  assert.ok(!probes.calls.some((call) => call.startsWith("install:")));
});

test("package-runtime rejects a different target architecture before staging", async () => {
  const { spawnSync } = await import("node:child_process");
  const fs = await import("node:fs");
  const os = await import("node:os");
  const path = await import("node:path");
  const { fileURLToPath } = await import("node:url");
  const appDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "ketplus-cross-runtime-"));
  try {
    const out = path.join(root, "runtime");
    const target = `darwin-${process.arch === "arm64" ? "x64" : "arm64"}`;
    const result = spawnSync(process.execPath, [path.join(appDir, "scripts/package-runtime.mjs"),
      "--out", out, "--platform", target, "--offline"], {
      encoding: "utf8", timeout: 10000,
      env: { PATH: process.env.PATH, HOME: os.homedir() },
    });
    assert.notEqual(result.status, 0);
    assert.match(result.stderr, /Cross-staging is not supported/);
    assert.equal(fs.existsSync(out), false);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});
