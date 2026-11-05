const { execFileSync } = require("node:child_process");
const { createHash } = require("node:crypto");
const DESIGN = "ketplus-desktop-redesign.ketatlas";
// Transcript V2 changes the Atlas rendering reference. Pin the exact committed
// source revision so later documentation/progress edits cannot silently move it.
const REVISION = "d0591869113ad4420961bff2ef2f2ac0ea840b58";
const digest = bytes => createHash("sha256").update(bytes).digest("hex");

// Exclusions are policy, never supplied by the receipt under validation.
function category(p) {
  if (p === DESIGN + "/atlas.progress.json") return "progress-metadata";
  if (p.endsWith(".md") || p.startsWith(DESIGN + "/licenses/")) return "documentation";
  return "rendering-source";
}
function derive(root) {
  const git = (...args) => execFileSync("git", ["-C", root, ...args], { maxBuffer: 64 * 1024 * 1024 });
  const entries = git("ls-tree", "-rz", REVISION, "--", DESIGN).toString().split("\0").filter(Boolean).map(line => {
    const [metadata, p] = line.split("\t");
    const [mode, type, gitBlob] = metadata.split(" ");
    if (mode !== "100644" || type !== "blob") throw new Error("unsupported pinned source entry: " + p);
    return { path: p, gitBlob, category: category(p) };
  });
  if (!entries.length) throw new Error("empty pinned design tree");
  const batch = execFileSync("git", ["-C", root, "cat-file", "--batch"], {
    input: entries.map(e => e.gitBlob).join("\n") + "\n", maxBuffer: 64 * 1024 * 1024,
  });
  let offset = 0;
  for (const entry of entries) {
    const end = batch.indexOf(10, offset);
    const [oid, type, size] = batch.subarray(offset, end).toString().split(" ");
    if (oid !== entry.gitBlob || type !== "blob" || !/^\d+$/.test(size)) throw new Error("invalid Git blob response");
    const bytes = batch.subarray(end + 1, end + 1 + Number(size));
    entry.sha256 = digest(bytes);
    offset = end + 2 + Number(size);
  }
  const sources = entries.filter(e => e.category === "rendering-source");
  const renderingSourceSha256 = digest(JSON.stringify(sources));
  return {
    revision: REVISION,
    designTree: git("rev-parse", REVISION + ":" + DESIGN).toString().trim(),
    renderingSourceSha256,
    entries,
  };
}
function binding(inventory) {
  return { revision: inventory.revision, designTree: inventory.designTree, renderingSourceSha256: inventory.renderingSourceSha256 };
}
module.exports = { derive, binding, digest, REVISION };
