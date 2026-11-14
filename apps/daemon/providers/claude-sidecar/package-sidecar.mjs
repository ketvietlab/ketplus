#!/usr/bin/env node
// Stages the Claude Agent SDK sidecar for KetPlus.app:
//
//   <out>/
//     index.mjs, package.json, package-lock.json
//     node_modules/     production deps including the platform Claude CLI
//     runtime.json
//
// Node itself is not copied: the daemon runs this tree with the bundled
// ketplus-browser Node (Contents/Resources/ketplus-browser/bin/node).
//
// Usage: node package-sidecar.mjs --out <dir>
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";

const sidecarDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)));
const { values } = parseArgs({
  options: {
    out: { type: "string" },
  },
});
if (!values.out) {
  console.error("usage: package-sidecar.mjs --out <dir>");
  process.exit(2);
}

const out = path.resolve(values.out);
const log = (message) => process.stderr.write(`[package-claude-sidecar] ${message}\n`);

function runNpm(args, options) {
  const npmCli = process.env.npm_execpath;
  if (npmCli && (npmCli.endsWith(".js") || npmCli.endsWith(".cjs") || npmCli.endsWith(".mjs"))) {
    execFileSync(process.execPath, [npmCli, ...args], options);
    return;
  }
  execFileSync(npmCli || "npm", args, options);
}

const required = ["session-usage.mjs", "authentication.mjs", "index.mjs", "transcript.mjs", "input.mjs", "tasks.mjs", "resume.mjs", "session-delete.mjs", "review-guard.mjs", "package.json", "package-lock.json"];
for (const name of required) {
  const source = path.join(sidecarDir, name);
  if (!fs.existsSync(source)) {
    throw new Error(`Missing ${source}`);
  }
}

const staging = fs.mkdtempSync(path.join(os.tmpdir(), "ketplus-claude-sidecar-"));
try {
  for (const name of required) {
    fs.copyFileSync(path.join(sidecarDir, name), path.join(staging, name));
  }
  log(`npm ci --omit=dev in ${staging}`);
  runNpm(["ci", "--omit=dev", "--ignore-scripts=false"], {
    cwd: staging,
    stdio: ["ignore", "inherit", "inherit"],
    env: { ...process.env, npm_config_audit: "false", npm_config_fund: "false" },
  });
  const sdkDir = path.join(staging, "node_modules", "@anthropic-ai", "claude-agent-sdk");
  if (!fs.existsSync(sdkDir)) {
    throw new Error("npm ci did not install @anthropic-ai/claude-agent-sdk");
  }
  const packageJson = JSON.parse(fs.readFileSync(path.join(staging, "package.json"), "utf8"));
  const sdkPackage = JSON.parse(fs.readFileSync(path.join(sdkDir, "package.json"), "utf8"));
  fs.writeFileSync(
    path.join(staging, "runtime.json"),
    `${JSON.stringify(
      {
        name: "ketplus-claude-sidecar",
        version: packageJson.version || "0.1.0",
        sdk: sdkPackage.name,
        sdkVersion: sdkPackage.version,
        claudeCodeVersion: sdkPackage.claudeCodeVersion,
        entry: "index.mjs",
      },
      null,
      2,
    )}\n`,
  );
  fs.rmSync(out, { recursive: true, force: true });
  fs.mkdirSync(path.dirname(out), { recursive: true });
  // Keep npm's relative bin links relocatable after the temporary tree is removed.
  fs.cpSync(staging, out, { recursive: true, verbatimSymlinks: true });
  log(`staged ${out}`);
} finally {
  fs.rmSync(staging, { recursive: true, force: true });
}
