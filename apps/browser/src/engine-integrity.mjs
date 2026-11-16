// Integrity primitives for an enrolled managed-engine identity. The caller must
// obtain the expected hashes from reviewed, independently authenticated metadata;
// neither an installation marker nor a cache-local receipt is a trust root.
import { createHash, timingSafeEqual } from "node:crypto";
import fs from "node:fs/promises";
import { constants } from "node:fs";
import path from "node:path";

const SHA256 = /^[a-f0-9]{64}$/;
const MAX_ARCHIVE_BYTES = 1_073_741_824;
const MAX_TREE_BYTES = 2_147_483_648;
const MAX_TREE_ENTRIES = 50_000;

export class EngineIntegrityError extends Error {
  constructor(message) {
    super(message);
    this.name = "EngineIntegrityError";
    this.code = "BROWSER_INTEGRITY";
  }
}

function requireHash(value, name) {
  if (typeof value !== "string" || !SHA256.test(value)) {
    throw new EngineIntegrityError(`Missing or invalid enrolled ${name}`);
  }
  return value;
}

function matchesHash(actual, expected) {
  return timingSafeEqual(Buffer.from(actual, "hex"), Buffer.from(expected, "hex"));
}

// No environment variable or cache path participates in identity selection.
export function enrolledEngineIdentity(manifest, { playwrightVersion, revision, browserVersion, platform, arch }) {
  const key = `${platform}-${arch}`;
  if (manifest?.schemaVersion !== 1 || manifest.playwrightVersion !== playwrightVersion ||
      manifest.revision !== revision || manifest.browserVersion !== browserVersion) {
    throw new EngineIntegrityError("Managed browser identity does not match the pinned Playwright revision");
  }
  const pin = manifest.platforms?.[key];
  if (!pin) throw new EngineIntegrityError(`No independently verified managed browser identity for ${key}`);
  requireHash(pin.archiveSha256, "archive SHA-256");
  requireHash(pin.treeSha256, "installed-tree SHA-256");
  let archiveUrl;
  try { archiveUrl = new URL(pin.archiveUrl); } catch { /* Rejected below. */ }
  if (!archiveUrl || archiveUrl.protocol !== "https:" || archiveUrl.username || archiveUrl.password || archiveUrl.hash) {
    throw new EngineIntegrityError(`Invalid enrolled archive URL for ${key}`);
  }
  if (typeof pin.executable !== "string" || !pin.executable || pin.executable.startsWith("/") ||
      /[\\\u0000-\u001f:]/u.test(pin.executable) || pin.executable.split("/").some((part) => !part || part === "." || part === "..")) {
    throw new EngineIntegrityError(`Invalid enrolled executable path for ${key}`);
  }
  // These fields preserve enrollment evidence; their presence does not authenticate
  // it. Maintainers must review that evidence before publishing the manifest.
  if (typeof pin.provenance?.url !== "string" || !pin.provenance.url.startsWith("https://") ||
      typeof pin.provenance?.revision !== "string" || !pin.provenance.revision) {
    throw new EngineIntegrityError(`Missing identity enrollment evidence for ${key}`);
  }
  return Object.freeze({
    platform: key,
    archiveUrl: archiveUrl.href,
    executable: pin.executable,
    archiveSha256: pin.archiveSha256,
    treeSha256: pin.treeSha256,
  });
}

// O_NONBLOCK prevents a substituted FIFO from hanging before descriptor validation.
// Hash the bytes from the opened descriptor, not a second path-based read.
async function readRegular(file, limit) {
  let handle;
  try {
    if (!(await fs.lstat(file)).isFile()) {
      throw new EngineIntegrityError("Managed browser data is not a regular file");
    }
    handle = await fs.open(file, constants.O_RDONLY | (constants.O_NONBLOCK ?? 0) | (constants.O_NOFOLLOW ?? 0));
    const before = await handle.stat({ bigint: true });
    if (!before.isFile() || before.size > BigInt(limit)) {
      throw new EngineIntegrityError("Managed browser data is not a bounded regular file");
    }
    const chunks = [];
    let size = 0;
    for (;;) {
      const chunk = Buffer.allocUnsafe(Math.min(65_536, limit - size + 1));
      const { bytesRead } = await handle.read(chunk, 0, chunk.length, null);
      if (!bytesRead) break;
      size += bytesRead;
      if (size > limit) throw new EngineIntegrityError("Managed browser data exceeds its byte limit");
      chunks.push(chunk.subarray(0, bytesRead));
    }
    const after = await handle.stat({ bigint: true });
    if (before.size !== BigInt(size) || after.size !== before.size ||
        after.mtimeNs !== before.mtimeNs || after.ctimeNs !== before.ctimeNs) {
      throw new EngineIntegrityError("Managed browser data changed during verification");
    }
    return { bytes: Buffer.concat(chunks, size), executable: Boolean(Number(after.mode) & 0o111) };
  } catch (error) {
    if (error instanceof EngineIntegrityError) throw error;
    throw new EngineIntegrityError(`Cannot read managed browser data (${error.code ?? "IO_ERROR"})`);
  } finally {
    await handle?.close();
  }
}

// Return the exact authenticated bytes for extraction into a new private staging
// directory. Callers must not reopen the original archive after this check.
export async function verifiedEngineArchive(file, expectedSha256) {
  requireHash(expectedSha256, "archive SHA-256");
  const { bytes } = await readRegular(file, MAX_ARCHIVE_BYTES);
  const actual = createHash("sha256").update(bytes).digest("hex");
  if (!matchesHash(actual, expectedSha256)) throw new EngineIntegrityError("Managed browser archive SHA-256 mismatch");
  return bytes;
}

// The tree hash covers every file, directory and link, including injected loaders
// beside the executable. Completion markers/receipts belong outside this tree.
// Only enrollment tooling may turn this measurement into a trusted expected hash,
// after authenticating the source archive through a separate trust root.
export async function measureEngineTree(root) {
  const rootInfo = await fs.lstat(root);
  if (!rootInfo.isDirectory() || rootInfo.isSymbolicLink()) {
    throw new EngineIntegrityError("Managed browser tree is not a real directory");
  }
  const hash = createHash("sha256");
  let totalBytes = 0;
  let entries = 0;
  const record = (row) => hash.update(`${JSON.stringify(row)}\n`);
  const visit = async (relative) => {
    const dir = path.join(root, relative);
    const names = (await fs.readdir(dir)).sort();
    for (const name of names) {
      if (!name || /[\\\u0000-\u001f:]/u.test(name)) throw new EngineIntegrityError("Invalid managed browser entry name");
      if (++entries > MAX_TREE_ENTRIES) throw new EngineIntegrityError("Managed browser tree has too many entries");
      const child = relative ? `${relative}/${name}` : name;
      const file = path.join(root, child);
      const info = await fs.lstat(file);
      if (info.isSymbolicLink()) {
        const target = await fs.readlink(file);
        const resolved = path.posix.normalize(path.posix.join(path.posix.dirname(child), target));
        if (!target || /[\\\u0000-\u001f:]/u.test(target) || path.posix.isAbsolute(target) ||
            resolved === ".." || resolved.startsWith("../")) {
          throw new EngineIntegrityError("Managed browser link escapes its tree");
        }
        record([child, "link", target]);
      } else if (info.isDirectory()) {
        record([child, "directory"]);
        await visit(child);
      } else if (info.isFile()) {
        const { bytes, executable } = await readRegular(file, MAX_TREE_BYTES - totalBytes);
        totalBytes += bytes.length;
        record([child, "file", bytes.length, executable, createHash("sha256").update(bytes).digest("hex")]);
      } else {
        throw new EngineIntegrityError("Managed browser tree contains a non-regular entry");
      }
    }
  };
  await visit("");
  return hash.digest("hex");
}

export async function verifyEngineTree(root, expectedSha256) {
  requireHash(expectedSha256, "installed-tree SHA-256");
  const actual = await measureEngineTree(root);
  if (!matchesHash(actual, expectedSha256)) throw new EngineIntegrityError("Managed browser installed-tree SHA-256 mismatch");
}
