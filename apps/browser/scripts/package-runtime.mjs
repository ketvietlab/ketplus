#!/usr/bin/env node
// Stages the self-contained KetPlus browser runtime:
//
//   <out>/
//     bin/node                     pinned Node.js (sha256 verified against runtime/node-runtime.json)
//     src/                         browser.v0 server (mcp.mjs, cli.mjs, …)
//     node_modules/                production dependencies only (npm ci --omit=dev), pruned
//     package.json, package-lock.json
//     .claude/skills/ketplus-browser/
//     THIRD_PARTY_NOTICES.md, licenses/
//     runtime.json
//
// Usage: node scripts/package-runtime.mjs --out <dir> [--node-cache <dir>] [--platform darwin-arm64] [--offline]
// The Node archive is taken from --node-cache (or $KETPLUS_NODE_CACHE) when present,
// otherwise downloaded from nodejs.org. Either way its sha256 must match the pin.
// npm comes from the pinned Node archive, so the host's node/npm versions do not matter.
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { sourceDigest } from "./source-digest.mjs";

const appDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const pin = JSON.parse(fs.readFileSync(path.join(appDir, "runtime", "node-runtime.json"), "utf8"));
const hostPlatform = `${process.platform}-${process.arch}`;

const { values } = parseArgs({
  options: {
    out: { type: "string" },
    "node-cache": { type: "string", default: process.env.KETPLUS_NODE_CACHE },
    platform: { type: "string", default: hostPlatform },
    offline: { type: "boolean", default: false },
  },
});
if (!values.out) {
  console.error("usage: package-runtime.mjs --out <dir> [--node-cache <dir>] [--platform darwin-arm64] [--offline]");
  process.exit(2);
}
const target = values.platform;
if (process.env.KETPLUS_PACKAGE_RUNTIME_CHECK_OUT_ONLY) {
  // Test hook: validate --out and exit without touching anything.
  try { checkedOut(values.out); process.exit(0); } catch (error) { console.error(error.message); process.exit(3); }
}
const expectedSha = pin.sha256[target];
if (!expectedSha) throw new Error(`No pinned Node.js hash for ${target}`);
if (target !== hostPlatform) throw new Error(`Cross-staging is not supported (host ${hostPlatform}, target ${target}): npm ci and the smoke test run the staged node`);

const log = (message) => process.stderr.write(`[package-runtime] ${message}\n`);
const run = (command, args, options = {}) => execFileSync(command, args, { stdio: ["ignore", "inherit", "inherit"], ...options });
const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

// Returns the verified archive bytes; the caller extracts exactly these bytes from a
// private copy, so the cache file can change after hashing without effect.
async function obtainNodeArchive() {
  const name = `node-${pin.version}-${target}.tar.gz`;
  const cacheDir = path.resolve(values["node-cache"] || path.join(os.tmpdir(), "ketplus-node-cache"));
  fs.mkdirSync(cacheDir, { recursive: true });
  const archive = path.join(cacheDir, name);
  if (fs.existsSync(archive)) {
    const cached = fs.readFileSync(archive);
    if (sha256(cached) === expectedSha) {
      log(`using cached ${archive}`);
      return cached;
    }
  }
  if (values.offline) throw new Error(`--offline: ${archive} is missing or does not match the pinned sha256`);
  const url = `${pin.baseUrl}/${pin.version}/${name}`;
  log(`downloading ${url}`);
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Download failed: ${response.status} ${url}`);
  const bytes = Buffer.from(await response.arrayBuffer());
  const actual = sha256(bytes);
  if (actual !== expectedSha) throw new Error(`sha256 mismatch for ${name}: expected ${expectedSha}, got ${actual}`);
  const partial = `${archive}.partial-${process.pid}`;
  fs.writeFileSync(partial, bytes);
  fs.renameSync(partial, archive);
  return bytes;
}

const PRUNE_DIRS = new Set(["test", "tests", "__tests__", "docs", "example", "examples", ".github", "types", ".bin"]);
const PRUNE_FILE = /(\.d\.[cm]?ts|\.map|\.tsbuildinfo)$|^(README|CHANGELOG|HISTORY)(\..*)?$/i;
// Playwright's trace viewer/recorder/report UIs and dependency installer scripts
// are never used by the browser.v0 server.
const PRUNE_PATHS = ["playwright-core/lib/vite", "playwright-core/bin", "playwright-core/types"];

function prune(dir) {
  let removed = 0;
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      if (PRUNE_DIRS.has(entry.name)) {
        removed += du(full);
        fs.rmSync(full, { recursive: true, force: true });
      } else removed += prune(full);
    } else if (PRUNE_FILE.test(entry.name)) {
      removed += fs.lstatSync(full).size;
      fs.rmSync(full, { force: true });
    }
  }
  return removed;
}

function du(dir) {
  let total = 0;
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    total += entry.isDirectory() ? du(full) : fs.lstatSync(full).size;
  }
  return total;
}

const LICENSE_NAME = /^(licen[cs]e|notice|copying|thirdpartynotices)(\..*)?$|\.licen[cs]e(\.txt|\.md)?$/i;
// Top-level license/notice files plus nested ones such as lib/foo.js.LICENSE (bundled
// third-party code), without descending into nested node_modules (listed separately).
function licenseFiles(dir, relative = "") {
  const found = [];
  for (const entry of fs.readdirSync(path.join(dir, relative), { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const child = path.join(relative, entry.name);
    if (entry.isDirectory()) {
      if (entry.name !== "node_modules") found.push(...licenseFiles(dir, child));
    } else if (entry.isFile() && (relative === "" ? LICENSE_NAME : /\.licen[cs]e(\.txt|\.md)?$/i).test(entry.name)) {
      found.push(path.join(dir, child));
    }
  }
  return found;
}

function productionPackages(modulesDir) {
  const packages = [];
  for (const entry of fs.readdirSync(modulesDir).sort()) {
    if (entry.startsWith(".")) continue;
    const dirs = entry.startsWith("@") ? fs.readdirSync(path.join(modulesDir, entry)).sort().map((n) => path.join(entry, n)) : [entry];
    for (const relative of dirs) {
      const dir = path.join(modulesDir, relative);
      const manifest = path.join(dir, "package.json");
      if (!fs.existsSync(manifest)) continue;
      const pkg = JSON.parse(fs.readFileSync(manifest, "utf8"));
      packages.push({ name: pkg.name, version: pkg.version, license: pkg.license ?? "UNKNOWN", homepage: pkg.homepage ?? pkg.repository?.url ?? "", dir });
      const nested = path.join(dir, "node_modules");
      if (fs.existsSync(nested)) packages.push(...productionPackages(nested));
    }
  }
  return packages;
}

function writeNotices(stage, nodeLicense) {
  const licensesDir = path.join(stage, "licenses");
  fs.mkdirSync(licensesDir, { recursive: true });
  const sections = [
    "# KetPlus browser runtime third-party notices",
    "",
    "This directory is distributed inside KetPlus.app. The browser.v0 server source",
    "(`src/`, `.claude/skills/ketplus-browser/`) is proprietary KetPlus code. The",
    "components below are redistributed under their own licenses; full texts are in",
    "`licenses/`. No browser binary is included: KetPlus uses the installed Google",
    "Chrome or Microsoft Edge, or downloads Chrome for Testing into the user's data",
    "directory on first need.",
    "",
    "| Component | Version | License | Files |",
    "| --- | --- | --- | --- |",
  ];
  const copy = (label, baseDir, files) =>
    files.map((file) => {
      const name = `${label}-${path.relative(baseDir, file)}`.replace(/[^a-zA-Z0-9.@_-]+/g, "_");
      fs.copyFileSync(file, path.join(licensesDir, name));
      return `licenses/${name}`;
    });
  fs.writeFileSync(path.join(licensesDir, "Node.js-LICENSE"), fs.readFileSync(nodeLicense));
  sections.push(`| Node.js | ${pin.version} | MIT (plus bundled third-party licenses listed in the file) | licenses/Node.js-LICENSE |`);
  const missing = [];
  for (const pkg of productionPackages(path.join(stage, "node_modules"))) {
    const files = copy(`${pkg.name}@${pkg.version}`, pkg.dir, licenseFiles(pkg.dir));
    if (!files.length) missing.push(pkg.name);
    sections.push(`| ${pkg.name} | ${pkg.version} | ${pkg.license} | ${files.join("<br>") || "(no license file shipped; see package.json)"} |`);
  }
  sections.push(
    "",
    "Playwright (`playwright-core`) is Apache-2.0; its `NOTICE` and `ThirdPartyNotices.txt` are",
    "reproduced in `licenses/` as required by section 4(d) of the Apache License.",
    "",
  );
  fs.writeFileSync(path.join(stage, "THIRD_PARTY_NOTICES.md"), sections.join("\n"));
  if (missing.length) throw new Error(`Production packages without a license file (cannot redistribute): ${missing.join(", ")}`);
}

// --out is deleted and replaced: refuse anything that is obviously not a staging directory.
function checkedOut(value) {
  if (!value || !value.trim()) throw new Error("--out must not be empty");
  const out = path.resolve(value);
  const real = (file) => { try { return fs.realpathSync(file); } catch { return path.resolve(file); } };
  const repoRoot = path.resolve(appDir, "..", "..");
  const forbidden = new Set([path.parse(out).root, os.homedir(), repoRoot, appDir, os.tmpdir()].flatMap((dir) => [path.resolve(dir), real(dir)]));
  if (forbidden.has(out) || forbidden.has(real(out))) throw new Error(`Refusing to replace --out ${out}`);
  if (path.relative(out, repoRoot) === "" || !path.relative(out, repoRoot).startsWith("..")) throw new Error(`--out ${out} contains the repository`);
  if (path.relative(out, os.homedir()) === "" || !path.relative(out, os.homedir()).startsWith("..")) throw new Error(`--out ${out} contains the home directory`);
  return out;
}

async function main() {
  const out = checkedOut(values.out);
  const archiveBytes = await obtainNodeArchive();
  const work = fs.mkdtempSync(path.join(os.tmpdir(), "ketplus-browser-runtime-"));
  try {
    const archive = path.join(work, "node.tar.gz");
    fs.writeFileSync(archive, archiveBytes, { mode: 0o600 });
    run("tar", ["-xzf", archive, "-C", work]);
    const nodeDist = path.join(work, `node-${pin.version}-${target}`);
    const stage = path.join(work, "stage");
    fs.mkdirSync(path.join(stage, "bin"), { recursive: true });
    const stagedNode = path.join(stage, "bin", process.platform === "win32" ? "node.exe" : "node");
    fs.copyFileSync(path.join(nodeDist, "bin", "node"), stagedNode);
    fs.chmodSync(stagedNode, 0o755);

    for (const item of ["src", "package.json", "package-lock.json"]) {
      fs.cpSync(path.join(appDir, item), path.join(stage, item), { recursive: true });
    }
    fs.cpSync(path.join(appDir, ".claude", "skills", "ketplus-browser"), path.join(stage, ".claude", "skills", "ketplus-browser"), { recursive: true });

    log("npm ci --omit=dev with the pinned npm");
    const npmCli = path.join(nodeDist, "lib", "node_modules", "npm", "bin", "npm-cli.js");
    run(stagedNode, [npmCli, "ci", "--omit=dev", "--ignore-scripts", "--no-audit", "--no-fund", "--loglevel=error", "--update-notifier=false"], {
      cwd: stage,
      env: { ...process.env, PATH: `${path.dirname(stagedNode)}${path.delimiter}${process.env.PATH}`, PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD: "1", npm_config_cache: path.join(work, "npm-cache") },
    });
    for (const relative of PRUNE_PATHS) fs.rmSync(path.join(stage, "node_modules", relative), { recursive: true, force: true });
    const pruned = prune(path.join(stage, "node_modules"));
    log(`pruned ${(pruned / 1e6).toFixed(1)} MB of docs, types and maps`);

    writeNotices(stage, path.join(nodeDist, "LICENSE"));
    const pkg = JSON.parse(fs.readFileSync(path.join(appDir, "package.json"), "utf8"));
    const lockHash = createHash("sha256").update(fs.readFileSync(path.join(appDir, "package-lock.json"))).digest("hex");
    const manifest = {
      name: "ketplus-browser",
      version: pkg.version,
      nodeVersion: pin.version,
      platform: target,
      node: path.relative(stage, stagedNode),
      protocols: ["browser.v0"],
      entrypoints: { mcp: "src/mcp.mjs", jsonrpc: "src/cli.mjs" },
      skill: ".claude/skills/ketplus-browser/SKILL.md",
      playwrightCore: pkg.dependencies["playwright-core"],
      packageLockSha256: lockHash,
      sourceSha256: sourceDigest(appDir),
    };
    fs.writeFileSync(path.join(stage, "runtime.json"), `${JSON.stringify(manifest, null, 2)}\n`);

    // Smoke test with the staged node only: loads the server graph and Playwright's registry.
    const smoke = execFileSync(
      stagedNode,
      ["--input-type=module", "-e", 'await import("./src/browser-service.mjs"); const { createRequire } = await import("node:module"); createRequire(process.cwd() + "/").resolve("playwright-core/lib/coreBundle"); console.log(process.version)'],
      { cwd: stage, env: { PATH: "/usr/bin:/bin" }, encoding: "utf8" },
    ).trim();
    if (smoke !== pin.version) throw new Error(`Staged runtime smoke test failed: ${smoke}`);

    fs.rmSync(out, { recursive: true, force: true });
    fs.mkdirSync(path.dirname(out), { recursive: true });
    fs.cpSync(stage, out, { recursive: true, verbatimSymlinks: true });
    log(`staged ${out} (${(du(out) / 1e6).toFixed(1)} MB)`);
  } finally {
    fs.rmSync(work, { recursive: true, force: true });
  }
}

main().catch((error) => {
  console.error(`[package-runtime] ${error.message}`);
  process.exit(1);
});
