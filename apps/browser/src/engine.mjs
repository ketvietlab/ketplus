// Browser engine selection. KetPlus never downloads a browser when a usable one
// is installed: it prefers the system Google Chrome, then Microsoft Edge, and
// only then a KetPlus-managed Chrome for Testing that Playwright's registry
// downloads over HTTPS on first need. Every engine is launched with a fresh
// temporary profile by Playwright; the user's personal profile is never used.
import { execFile } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { BrowserApiError } from "./errors.mjs";

export const ENGINE_PREFERENCES = ["auto", "chrome", "msedge", "bundled", "playwright"];
const INSTALLER = path.join(path.dirname(fileURLToPath(import.meta.url)), "engine-installer.mjs");

// Mirrors QStandardPaths::GenericDataLocation + Ketsuite/KetPlus/browser-runtime/chrome,
// which the desktop and daemon pass as KETPLUS_BROWSER_CACHE.
export function defaultBrowserCacheDir({ platform = process.platform, env = process.env, homedir = os.homedir() } = {}) {
  let base;
  if (platform === "darwin") base = path.join(homedir, "Library", "Application Support");
  else if (platform === "win32") base = env.LOCALAPPDATA || path.join(homedir, "AppData", "Local");
  else base = env.XDG_DATA_HOME || path.join(homedir, ".local", "share");
  return path.join(base, "Ketsuite", "KetPlus", "browser-runtime", "chrome");
}

export function systemBrowserCandidates(channel, { platform = process.platform, env = process.env, homedir = os.homedir() } = {}) {
  const mac = { chrome: "Google Chrome", msedge: "Microsoft Edge" }[channel];
  if (!mac) return [];
  if (platform === "darwin") {
    const bundle = `${mac}.app/Contents/MacOS/${mac}`;
    return [path.join("/Applications", bundle), path.join(homedir, "Applications", bundle)];
  }
  if (platform === "win32") {
    const relative = channel === "chrome" ? "Google\\Chrome\\Application\\chrome.exe" : "Microsoft\\Edge\\Application\\msedge.exe";
    return [env.LOCALAPPDATA, env.PROGRAMFILES, env["PROGRAMFILES(X86)"]].filter(Boolean).map((root) => path.win32.join(root, relative));
  }
  return channel === "chrome" ? ["/opt/google/chrome/chrome"] : ["/opt/microsoft/msedge/msedge"];
}

const isExecutable = (file) => {
  try {
    fs.accessSync(file, fs.constants.X_OK);
    return fs.statSync(file).isFile();
  } catch {
    return false;
  }
};

// Environment for the installer child. Playwright does not checksum browser archives,
// so integrity rests on HTTPS from its CDN: every PLAYWRIGHT_* variable (download host
// mirrors, PLAYWRIGHT_PROXY_BYPASS_FOR_TESTING, …), NODE_OPTIONS (--require/--import
// preloads) and TLS-weakening Node variables are dropped. HTTPS_PROXY and friends are
// kept: a proxy still has to present the CDN's certificate, and corporate networks need it.
const UNSAFE_INSTALLER_ENV = /^(PLAYWRIGHT_.*|NODE_OPTIONS|NODE_TLS_REJECT_UNAUTHORIZED|NODE_PATH|NODE_REPL_EXTERNAL_MODULE|NODE_V8_COVERAGE)$/i;
export function installerEnv(cacheDir, env = process.env) {
  const result = {};
  for (const [name, value] of Object.entries(env)) if (!UNSAFE_INSTALLER_ENV.test(name)) result[name] = value;
  result.PLAYWRIGHT_BROWSERS_PATH = cacheDir;
  return result;
}

export function runInstaller(mode, cacheDir, timeoutMs, env = process.env) {
  return new Promise((resolve, reject) => {
    execFile(
      process.execPath,
      [INSTALLER, mode],
      { env: installerEnv(cacheDir, env), timeout: timeoutMs, maxBuffer: 1024 * 1024 },
      (error, stdout, stderr) => {
        if (error) {
          const detail = String(stderr || error.message).trim().split("\n").filter(Boolean).at(-1) ?? "unknown error";
          reject(new Error(detail));
          return;
        }
        resolve(stdout.trim() || null);
      },
    );
  });
}

export function systemProbes() {
  return {
    findSystemBrowser: (channel) => systemBrowserCandidates(channel).find(isExecutable) ?? null,
    // Chrome for Testing already present in the KetPlus cache, without downloading.
    findBundledBrowser: (cacheDir) => runInstaller("--probe", cacheDir, 15_000),
    installBundledBrowser: (cacheDir) => runInstaller("--install", cacheDir, 15 * 60_000),
  };
}

export function enginePreference(env = process.env) {
  const value = String(env.KETPLUS_BROWSER_ENGINE || "auto").trim().toLowerCase();
  if (!ENGINE_PREFERENCES.includes(value)) {
    throw new BrowserApiError(
      "BROWSER_UNAVAILABLE",
      `KETPLUS_BROWSER_ENGINE must be one of ${ENGINE_PREFERENCES.join(", ")}; got "${value.slice(0, 40)}"`,
    );
  }
  return value;
}

const unavailable = (message, data) => new BrowserApiError("BROWSER_UNAVAILABLE", message, data);

export async function probeEngine({ env = process.env, probes = systemProbes(), playwrightExecutable } = {}) {
  try {
    const preference = enginePreference(env);
    if (preference === "playwright") {
      const available = Boolean(playwrightExecutable && isExecutable(playwrightExecutable));
      return { available, downloadRequired: false, reason: available ? "" : "Playwright Chromium is not installed." };
    }
    const engine = await selectEngine({
      preference,
      cacheDir: env.KETPLUS_BROWSER_CACHE || defaultBrowserCacheDir({ env }),
      allowDownload: false,
      probes,
    }).catch((error) => {
      if (error.code === "BROWSER_UNAVAILABLE" && env.KETPLUS_BROWSER_DOWNLOAD !== "0" &&
          (preference === "auto" || preference === "bundled")) return null;
      throw error;
    });
    return { available: true, downloadRequired: engine === null, engine };
  } catch (error) {
    return { available: false, downloadRequired: false, reason: String(error.message) };
  }
}

// Returns { name, source, executablePath } where executablePath is null for the
// Playwright-managed default Chromium (development only).
export async function selectEngine({
  preference = "auto",
  cacheDir = defaultBrowserCacheDir(),
  allowDownload = true,
  probes = systemProbes(),
} = {}) {
  const system = (name) => {
    const executablePath = probes.findSystemBrowser(name);
    return executablePath ? { name, source: "system", executablePath } : null;
  };
  if (preference === "playwright") return { name: "playwright", source: "playwright-cache", executablePath: null };
  if (preference === "chrome" || preference === "msedge") {
    const found = system(preference);
    if (found) return found;
    throw unavailable(
      `${preference === "chrome" ? "Google Chrome" : "Microsoft Edge"} is not installed (KETPLUS_BROWSER_ENGINE=${preference})`,
      { engine: preference },
    );
  }
  if (preference === "auto") {
    const found = system("chrome") ?? system("msedge");
    if (found) return found;
  }
  const existing = await probes.findBundledBrowser(cacheDir).catch(() => null);
  if (existing) return { name: "bundled", source: "ketplus-cache", executablePath: existing };
  const prefix = preference === "auto" ? "Neither Google Chrome nor Microsoft Edge is installed, and " : "";
  if (!allowDownload) {
    throw unavailable(`${prefix}Chrome for Testing is not in ${cacheDir} and downloads are disabled (KETPLUS_BROWSER_DOWNLOAD=0)`, {
      engine: "bundled",
    });
  }
  let installed;
  try {
    installed = await probes.installBundledBrowser(cacheDir);
  } catch (error) {
    throw unavailable(
      `${prefix}Chrome for Testing could not be downloaded into ${cacheDir} (offline?): ${String(error?.message ?? error).slice(0, 200)}. Install Google Chrome or retry when online.`,
      { engine: "bundled" },
    );
  }
  if (!installed) throw unavailable(`Chrome for Testing installation in ${cacheDir} did not produce an executable`, { engine: "bundled" });
  return { name: "bundled", source: "downloaded", executablePath: installed };
}
