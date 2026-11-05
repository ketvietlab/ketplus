const fs = require("node:fs");
const path = require("node:path");
const { execFileSync } = require("node:child_process");
const { digest } = require("./rendering-source.cjs");
function sourceDigest(root) {
  const files = ["cloud/CloudEventProjection", "storage/LocalJournal", "protocol/DaemonProtocol"]
    .flatMap(p => ["cpp", "h"].map(ext => "apps/daemon/src/" + p + "." + ext));
  files.push("apps/daemon/src/storage/LocalJournalActivityGroups.cpp",
    "apps/daemon/src/storage/LocalJournalLargeEvents.cpp");
  const base = "tests/daemon/characterization/";
  files.push(...fs.readdirSync(path.join(root, base)).filter(p => fs.statSync(path.join(root, base, p)).isFile()).map(p => base + p));
  return digest(files.sort().map(p => p + ":" + digest(fs.readFileSync(path.join(root, p))) + "\n").join(""));
}
function validateReport(report, checkoutSha, expectedDigest) {
  const errors = [];
  const check = (ok, message) => { if (!ok) errors.push(message); };
  check(report?.status === "complete" && report.baselineValid === true && report.testExitCode === 0, "baseline must be complete and successful");
  check(/^[a-f0-9]{40}$/.test(checkoutSha) && report?.buildSha === checkoutSha, "build SHA must equal exact checkout");
  check(report?.buildType === "Release" && report.sourceDirty === false, "clean Release build required");
  check(/^[a-f0-9]{64}$/.test(expectedDigest) && report?.sourceDigest === expectedDigest, "sourceDigest mismatch");
  const slots = ["cloudLifecycle", "workloadCommandAdapter", "pagingGenerationDuplicatesAndReopen", "productionWorkloadSamples"].sort();
  check(Array.isArray(report?.completedTests) && JSON.stringify([...report.completedTests].sort()) === JSON.stringify(slots), "four completed slots required");
  const rows = report?.rawMeasurements;
  const pairs = new Set();
  if (Array.isArray(rows)) for (const r of rows) {
    check(r && ["items-1k", "items-10k", "giant-turn"].includes(r.scenario) && [0, 1, 2].includes(r.sample), "invalid sample");
    pairs.add(r?.scenario + ":" + r?.sample);
  }
  check(Array.isArray(rows) && rows.length === 9 && pairs.size === 9 && report.samplesPerScenario === 3, "nine unique samples required");
  for (const key of ["compiler", "qt", "architecture", "platform", "publishedAt"]) check(typeof report?.[key] === "string" && report[key].length > 0, "missing " + key);
  check(Number.isFinite(Date.parse(report?.publishedAt)), "invalid publication time");
  return { ok: errors.length === 0, errors };
}
if (require.main === module) {
  try {
    const root = process.cwd();
    const checkoutSha = execFileSync("git", ["rev-parse", "HEAD"], { encoding: "utf8" }).trim();
    const dirty = execFileSync("git", ["status", "--porcelain", "--untracked-files=normal"], { encoding: "utf8" }).trim();
    if (dirty) throw new Error("checkout is dirty:\n" + dirty);
    const bytes = fs.readFileSync(process.argv[2]);
    const report = JSON.parse(bytes);
    const result = validateReport(report, checkoutSha, sourceDigest(root));
    if (!result.ok) throw new Error(result.errors.join("; "));
    console.log(JSON.stringify({ checkoutSha, buildSha: report.buildSha, sourceDigest: report.sourceDigest, artifactSha256: digest(bytes), status: report.status, baselineValid: report.baselineValid, samples: 9 }, null, 2));
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
module.exports = { sourceDigest, validateReport };
