// Child process used by engine.mjs. Playwright fixes its browser registry
// directory from PLAYWRIGHT_BROWSERS_PATH when it loads, so the KetPlus cache is
// handled in a separate process instead of mutating the server's environment.
//   --probe    print the Chrome for Testing executable if a complete install exists
//   --install  download it with Playwright's registry (install lock, retries), then print it
//   --where    print {"executable","directory","marker"} as JSON (tests; no side effects)
//
// An install counts only when Playwright's INSTALLATION_COMPLETE marker exists in the
// browser directory. Playwright writes it after the archive is fully extracted, so a
// download that was interrupted (timeout, quit, full disk) is removed and redone
// instead of being launched forever.
//
// Integrity: Playwright 1.63 does not verify a checksum or signature for browser
// archives. It relies on HTTPS to Playwright's CDN; engine.mjs strips the
// environment variables that could redirect the download host (see installerEnv).
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const mode = process.argv[2];
if (!process.env.PLAYWRIGHT_BROWSERS_PATH || !["--probe", "--install", "--where"].includes(mode)) {
  process.stderr.write("usage: PLAYWRIGHT_BROWSERS_PATH=<dir> engine-installer.mjs --probe|--install|--where\n");
  process.exit(2);
}

const { registry } = require("playwright-core/lib/coreBundle").registry;
const executable = registry.findExecutable("chromium");
const directory = executable?.directory;
const marker = directory ? path.join(directory, "INSTALLATION_COMPLETE") : null;
const installed = () => {
  const file = executable?.executablePath();
  return file && marker && fs.existsSync(marker) && fs.existsSync(file) ? file : null;
};

try {
  if (mode === "--where") {
    process.stdout.write(`${JSON.stringify({ executable: executable?.executablePath() ?? null, directory, marker })}\n`);
    process.exit(0);
  }
  if (mode === "--install" && !installed()) {
    // Progress goes to stderr; stdout carries only the resulting path.
    console.log = (...args) => process.stderr.write(`${args.join(" ")}\n`);
    fs.mkdirSync(process.env.PLAYWRIGHT_BROWSERS_PATH, { recursive: true, mode: 0o700 });
    fs.chmodSync(process.env.PLAYWRIGHT_BROWSERS_PATH, 0o700);
    // Anything left without the marker is a partial download or extraction.
    if (directory && fs.existsSync(directory)) fs.rmSync(directory, { recursive: true, force: true });
    await registry.install([executable], { gc: false });
  }
  const file = installed();
  if (file) process.stdout.write(`${file}\n`);
  else if (mode === "--install") throw new Error("Chrome for Testing installation did not complete");
} catch (error) {
  process.stderr.write(`${String(error?.message ?? error).split("\n")[0]}\n`);
  process.exit(1);
}
