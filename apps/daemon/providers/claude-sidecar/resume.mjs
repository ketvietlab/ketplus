import { copyFile, mkdir, readdir, stat } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

// Claude Code keeps a session's transcript under a folder named after its cwd.
export function projectFolder(cwd) {
  return cwd.replace(/[^a-zA-Z0-9]/g, "-");
}

// A conversation resumes in another cwd. Use the newest persisted transcript, including
// when returning to a project folder with an older copy; every original stays put.
export async function ensureResumable(sessionId, cwd, configDir = process.env.CLAUDE_CONFIG_DIR || path.join(os.homedir(), ".claude")) {
  if (!sessionId || !cwd || !/^[A-Za-z0-9-]+$/.test(sessionId)) return false;
  const projects = path.join(configDir, "projects");
  const file = `${sessionId}.jsonl`;
  const target = path.join(projects, projectFolder(cwd), file);
  let folders;
  try {
    folders = await readdir(projects);
  } catch {
    return false;
  }
  let latest = null;
  for (const folder of folders) {
    const source = path.join(projects, folder, file);
    let info;
    try { info = await stat(source); } catch { continue; }
    if (!info.isFile()) continue;
    if (!latest || info.mtimeMs > latest.mtime || (info.mtimeMs === latest.mtime && source === target))
      latest = { source, mtime: info.mtimeMs };
  }
  if (!latest) return false;
  if (latest.source !== target) {
    await mkdir(path.dirname(target), { recursive: true });
    await copyFile(latest.source, target);
  }
  return true;
}
