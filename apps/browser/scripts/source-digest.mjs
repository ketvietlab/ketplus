import { createHash } from "node:crypto";
import fs from "node:fs";
import path from "node:path";

// The sources a staged runtime was built from: src/, the skill, package.json and
// package-lock.json. Each file contributes "<relative path>\n<sha256>\n" in sorted path
// order. cmake/KetPlusBrowserRuntime.cmake computes the same digest, so a prebuilt
// runtime from another commit is refused at configure time.
export function sourceDigest(appDir) {
  const files = ["package.json", "package-lock.json"];
  const walk = (relative) => {
    for (const entry of fs.readdirSync(path.join(appDir, relative), { withFileTypes: true })) {
      const child = `${relative}/${entry.name}`;
      if (entry.isDirectory()) walk(child);
      else if (entry.isFile()) files.push(child);
    }
  };
  walk("src");
  walk(".claude/skills/ketplus-browser");
  files.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  let manifest = "";
  for (const file of files) {
    const hash = createHash("sha256").update(fs.readFileSync(path.join(appDir, file))).digest("hex");
    manifest += `${file}\n${hash}\n`;
  }
  return createHash("sha256").update(manifest).digest("hex");
}
