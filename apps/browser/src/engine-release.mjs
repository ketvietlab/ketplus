// Preparation for an immutable manifest shipped inside an authenticated app
// release. Structural validation here does not authenticate the app, its signer,
// or the initial upstream artifact. Those enrollment gates remain external.
import { EngineIntegrityError, enrolledEngineIdentity } from "./engine-integrity.mjs";

const COMPONENTS = new Set(["chromium", "chromium-headless-shell", "ffmpeg", "winldd"]);
const BROWSERS = new Set(["chromium", "chromium-headless-shell"]);
const ID = /^[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}$/;
const HASH = /^[a-f0-9]{64}$/;
const VERSION = /^\d+\.\d+\.\d+\.\d+$/;
const MAX_MANIFEST_BYTES = 131_072;

function reject(message) { throw new EngineIntegrityError(message); }

function revocationSet(value, pattern) {
  if (!Array.isArray(value) || value.length > 1_024 || value.some((entry) => typeof entry !== "string" || !pattern.test(entry))) {
    reject("Invalid bundled engine revocation inventory");
  }
  return new Set(value);
}

function scope(entry) {
  if (!COMPONENTS.has(entry?.component) || typeof entry.platform !== "string" || !ID.test(entry.platform) ||
      typeof entry.revision !== "string" || !/^\d+$/.test(entry.revision) ||
      (entry.component === "winldd" && entry.platform !== "win32-x64") ||
      (BROWSERS.has(entry.component) ? typeof entry.browserVersion !== "string" || !VERSION.test(entry.browserVersion) : entry.browserVersion !== null)) {
    reject("Invalid bundled engine artifact scope");
  }
  return JSON.stringify([entry.component, entry.platform, entry.revision, entry.browserVersion]);
}

// bytes must come from the application-owned manifest, not a cache receipt,
// download response or environment override. Expected release/package bindings
// must likewise come from authenticated application code. No remote updater,
// signature ceremony, trust-on-first-use or production pins are provided here.
export function bundledEngineRelease(bytes, { releaseId, playwrightVersion, platform = process.platform, arch = process.arch }) {
  if (!(typeof bytes === "string" || Buffer.isBuffer(bytes)) || Buffer.byteLength(bytes) > MAX_MANIFEST_BYTES) {
    reject("Invalid bundled engine manifest bytes");
  }
  let manifest;
  try { manifest = JSON.parse(bytes.toString()); } catch { reject("Invalid bundled engine manifest JSON"); }
  if (!manifest || manifest.schemaVersion !== 1 || manifest.kind !== "ketplus-managed-engine-release" ||
      typeof releaseId !== "string" || !ID.test(releaseId) || manifest.releaseId !== releaseId ||
      typeof playwrightVersion !== "string" || !playwrightVersion || manifest.playwrightVersion !== playwrightVersion ||
      !Array.isArray(manifest.artifacts) || manifest.artifacts.length > 64) {
    reject("Bundled engine manifest does not match this application release");
  }
  const revokedIds = revocationSet(manifest.revoked?.identityIds, ID);
  const revokedArchives = revocationSet(manifest.revoked?.archiveSha256, HASH);
  const revokedTrees = revocationSet(manifest.revoked?.treeSha256, HASH);
  const artifacts = new Map();
  const ids = new Set();
  for (const entry of manifest.artifacts) {
    const key = scope(entry);
    if (typeof entry.id !== "string" || !ID.test(entry.id) || ids.has(entry.id) || artifacts.has(key) || entry.status !== "enrolled") {
      reject("Duplicate, missing or unenrolled bundled engine identity");
    }
    ids.add(entry.id);
    artifacts.set(key, entry);
  }
  const select = (request) => {
    if (request?.platform !== `${platform}-${arch}`) reject("Engine artifact does not match the application host platform");
    const key = scope(request);
    const entry = artifacts.get(key);
    if (!entry) reject(`No enrolled identity for requested engine component ${request.component}`);
    if (revokedIds.has(entry.id) || revokedArchives.has(entry.archiveSha256) || revokedTrees.has(entry.treeSha256)) {
      reject(`Managed engine identity is revoked: ${entry.id}`);
    }
    const identity = enrolledEngineIdentity({
      schemaVersion: 1, playwrightVersion, revision: entry.revision, browserVersion: entry.browserVersion,
      platforms: { [entry.platform]: entry },
    }, { playwrightVersion, revision: request.revision, browserVersion: request.browserVersion, platform, arch });
    return Object.freeze({ ...identity, id: entry.id, component: entry.component, releaseId });
  };
  return Object.freeze({
    releaseId,
    playwrightVersion,
    plan({ browser, helpers = [] }) {
      if (!BROWSERS.has(browser?.component) || !Array.isArray(helpers) || helpers.length > 2 ||
          helpers.some((entry) => !COMPONENTS.has(entry?.component) || BROWSERS.has(entry.component)) ||
          new Set(helpers.map((entry) => entry.component)).size !== helpers.length) {
        reject("Invalid managed engine launch plan");
      }
      // Resolve every helper/flavor before the first cache read or download.
      return Object.freeze([select(browser), ...helpers.map(select)]);
    },
  });
}

// A focused integration seam for the store and launcher. Production callers must
// derive the complete plan from the pinned driver and fence every launch path;
// a tool request cannot supply this policy, callbacks or helper inventory.
export async function launchVerifiedManagedEngine({ policy, plan, createStore, launch }) {
  if (typeof createStore !== "function" || typeof launch !== "function") reject("Missing managed engine adapters");
  const identities = policy.plan(plan);
  const stores = identities.map(createStore);
  const installed = [];
  for (const store of stores) installed.push(await store.install());
  const verified = [];
  for (let index = 0; index < stores.length; index++) {
    const executable = await stores[index].probe();
    if (typeof executable !== "string" || !executable || executable !== installed[index]) {
      reject("Managed engine executable changed before launch");
    }
    verified.push(executable);
  }
  return launch(Object.freeze({
    executablePath: verified[0],
    helpers: Object.freeze(Object.fromEntries(identities.slice(1).map((identity, index) => [identity.component, verified[index + 1]]))),
  }));
}
