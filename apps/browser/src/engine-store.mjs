// Managed-engine lifecycle, separate from network and ZIP adapters. Both adapters
// must be supplied by the application; engine selection must never accept them
// from a tool request, environment variable or a cache-local configuration file.
import fs from "node:fs/promises";
import { constants } from "node:fs";
import path from "node:path";
import { EngineIntegrityError, verifiedEngineArchive, verifyEngineTree } from "./engine-integrity.mjs";

async function realDirectory(dir, create = false) {
  if (create) await fs.mkdir(dir, { recursive: true, mode: 0o700 });
  const info = await fs.lstat(dir);
  if (!info.isDirectory() || info.isSymbolicLink()) throw new EngineIntegrityError("Managed browser cache is not a real directory");
  if (process.platform !== "win32" && (info.uid !== process.getuid() || (info.mode & 0o077))) {
    throw new EngineIntegrityError("Managed browser cache must be owned by the current user and owner-only");
  }
}

async function executableInTree(dir, identity) {
  const executable = path.join(dir, ...identity.executable.split("/"));
  if (!(await fs.lstat(executable)).isFile()) throw new EngineIntegrityError("Managed browser executable is not a regular file");
  await fs.access(executable, constants.X_OK);
  return executable;
}

export class ManagedEngineStore {
  constructor({ cacheDir, identity, downloadArchive, extractArchive }) {
    this.cacheDir = path.resolve(cacheDir);
    // identity is the frozen result of enrolledEngineIdentity, never a receipt
    // read from this cache. A content-addressed directory avoids version aliases.
    this.identity = identity;
    this.directory = path.join(this.cacheDir, `verified-${identity.platform}-${identity.treeSha256}`);
    this.downloadArchive = downloadArchive;
    this.extractArchive = extractArchive;
  }

  async probe() {
    try { await fs.lstat(this.cacheDir); } catch (error) {
      if (error.code === "ENOENT") return null;
      throw error;
    }
    await realDirectory(this.cacheDir);
    try { await fs.lstat(this.directory); } catch (error) {
      if (error.code === "ENOENT") return null;
      throw error;
    }
    await verifyEngineTree(this.directory, this.identity.treeSha256);
    return executableInTree(this.directory, this.identity);
  }

  async install() {
    // Corrupt caches fail explicitly. They are not silently executed, deleted or
    // promoted by a marker. A caller may offer a separate, explicit repair action.
    const installed = await this.probe();
    if (installed) return installed;
    await realDirectory(this.cacheDir, true);
    await fs.chmod(this.cacheDir, 0o700);
    const stage = await fs.mkdtemp(path.join(this.cacheDir, ".verified-install-"));
    const archive = path.join(stage, "archive.zip");
    const tree = path.join(stage, "tree");
    try {
      await this.downloadArchive(this.identity.archiveUrl, archive);
      const authenticated = await verifiedEngineArchive(archive, this.identity.archiveSha256);
      await fs.mkdir(tree, { mode: 0o700 });
      // Extract exactly these bytes, never the original download path. The ZIP
      // adapter must also enforce path containment and expansion limits.
      await this.extractArchive(authenticated, tree);
      await verifyEngineTree(tree, this.identity.treeSha256);
      await executableInTree(tree, this.identity);
      // Only a completely verified tree is published. An interrupted stage is
      // invisible to discovery; a later install starts in a different stage.
      try {
        await fs.rename(tree, this.directory);
      } catch (error) {
        if (!["EEXIST", "ENOTEMPTY", "EPERM"].includes(error.code)) throw error;
        // Another installer may have won. Verify its bytes rather than trusting
        // its directory name or the fact that rename reported a collision.
        const concurrent = await this.probe();
        if (concurrent) return concurrent;
        throw error;
      }
      return await this.probe();
    } finally {
      await fs.rm(stage, { recursive: true, force: true });
    }
  }
}
