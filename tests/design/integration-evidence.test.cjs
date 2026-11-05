const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { execFileSync } = require("node:child_process");
const { validate, resolveRepoPath } = require("../../tasks/desktop-refactor/wave-1/integration/evidence-validator.cjs");
const policy = require("../../tasks/desktop-refactor/wave-1/integration/gate-policy.cjs");
const { visualFixture, approve, writeArtifact, hash, png } = require("./integration-synthetic.cjs");
const { inspectPng } = require("../../tasks/desktop-refactor/wave-1/integration/png-integrity.cjs");
const repoRoot = path.resolve(__dirname, "../..");
const base = "tasks/desktop-refactor/wave-1/integration";
function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "ketplus-test-only-evidence-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  for (const p of ["docs", "tests/design", "tasks/desktop-refactor", "ketplus-desktop-redesign.ketatlas"]) fs.cpSync(path.join(repoRoot, p), path.join(root, p), { recursive: true });
  // Only read Git objects from the real repository; no refs or checkouts are changed.
  const gitDir = execFileSync("git", ["-C", repoRoot, "rev-parse", "--absolute-git-dir"], { encoding: "utf8" }).trim();
  fs.writeFileSync(path.join(root, ".git"), "gitdir: " + gitDir + "\n");
  const manifestPath = path.join(root, base, "baseline-manifest.json");
  const manifest = JSON.parse(fs.readFileSync(manifestPath));
  return { root, manifest, save: () => fs.writeFileSync(manifestPath, JSON.stringify(manifest)) };
}
const messages = r => r.errors.map(e => e.path + ": " + e.message).join("\n");
test("R00-D metrics publication gate rejects partial, failed and stale provenance", () => {
  const { validateReport } = require("../../tasks/desktop-refactor/wave-1/integration/check-characterization.cjs");
  // TEST ONLY metadata; never published as benchmark evidence.
  const sha = "a".repeat(40), digest = "b".repeat(64);
  const report = { status: "complete", baselineValid: true, testExitCode: 0, buildSha: sha,
    sourceDigest: digest, sourceDirty: false, buildType: "Release", samplesPerScenario: 3,
    completedTests: ["cloudLifecycle", "workloadCommandAdapter", "pagingGenerationDuplicatesAndReopen", "productionWorkloadSamples"],
    rawMeasurements: ["items-1k", "items-10k", "giant-turn"].flatMap(scenario => [0, 1, 2].map(sample => ({scenario, sample}))),
    compiler: "test-only", qt: "6.11.1", architecture: "test-only", platform: "test-only", publishedAt: "2026-09-12T00:00:00Z" };
  assert.equal(validateReport(report, sha, digest).ok, true);
  for (const mutate of [r => r.status = "failed", r => r.baselineValid = false, r => r.testExitCode = 1,
    r => r.rawMeasurements.pop(), r => r.rawMeasurements[0] = r.rawMeasurements[1], r => r.completedTests.pop(),
    r => r.sourceDirty = true, r => r.buildSha = "c".repeat(40), r => r.sourceDigest = "d".repeat(64),
    r => r.buildType = "Debug", r => r.rawMeasurements = null, r => delete r.qt]) {
    const invalid = structuredClone(report); mutate(invalid);
    assert.equal(validateReport(invalid, sha, digest).ok, false);
  }
  assert.equal(validateReport(null, sha, digest).ok, false);
});
// The design bundle evolves after its review; evidence stays bound to the pinned revision and
// live drift is reported as a warning.
function drifted(t, mutate, match) {
  const f = fixture(t); mutate(f.manifest, f.root);
  const result = validate({ root: f.root });
  assert.equal(messages(result), "");
  assert.equal(result.ok, true);
  assert.match(result.warnings.map(w => w.path + ": " + w.message).join("\n"), match);
  return result;
}
test("R00-D CLI reports a deleted connections HTML as drift from the reviewed design", t => {
  const f = fixture(t);
  fs.unlinkSync(path.join(f.root, "ketplus-desktop-redesign.ketatlas/screens/connections.html"));
  const result = require("node:child_process").spawnSync(process.execPath, [base + "/check-integration.cjs"], { cwd: f.root, encoding: "utf8" });
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stderr, /warning: drift renderingSource\.ketplus-desktop-redesign\.ketatlas\/screens\/connections\.html/);
});
for (const file of ["connections.html", "desktop.css", "core.js"]) {
  test("R00-D reports modified rendering source " + file + " as drift", t => drifted(t, (m, root) => {
    fs.appendFileSync(path.join(root, "ketplus-desktop-redesign.ketatlas/screens", file), "\n/* drift */\n");
  }, new RegExp("screens/" + file.replace(".", "\\.") + ": differs from pinned rendering source")));
}
test("R00-D rejects omitted source receipt entry", t => invalid(t, (m, root) => {
  const p = path.join(root, base, "rendering-source-inventory.json"), receipt = JSON.parse(fs.readFileSync(p));
  receipt.entries = receipt.entries.filter(e => !e.path.endsWith("connections.html"));
  fs.writeFileSync(p, JSON.stringify(receipt));
}, /receipt differs/));
test("R00-D local receipt hashes cannot hide source drift", t => invalid(t, (m, root) => {
  const p = path.join(root, base, "rendering-source-inventory.json"), receipt = JSON.parse(fs.readFileSync(p));
  const e = receipt.entries.find(e => e.path.endsWith("desktop.css"));
  fs.appendFileSync(path.join(root, e.path), "\n/* drift */\n");
  e.sha256 = hash(fs.readFileSync(path.join(root, e.path)));
  fs.writeFileSync(p, JSON.stringify(receipt));
}, /receipt differs/));
test("R00-D progress and documentation updates do not change rendering reference", t => {
  const f = fixture(t);
  for (const file of ["atlas.progress.json", "README.md", "assets/brand/ketplus-v1/README.md"]) fs.appendFileSync(path.join(f.root, "ketplus-desktop-redesign.ketatlas", file), "\n");
  const result = validate({ root: f.root });
  assert.equal(messages(result), "");
});
test("R00-D rejects reference receipt binding drift", t => invalid(t, (m, root) => {
  const { ref } = visualFixture(m, root); ref.renderingSource.renderingSourceSha256 = "0".repeat(64);
}, /reference rendering source binding mismatch/, { testOnly: true }));
test("R00-D reports a renamed source as drift", t => drifted(t, (m, root) => {
  const p = path.join(root, "ketplus-desktop-redesign.ketatlas/screens/connections.html");
  fs.renameSync(p, p + ".renamed");
}, /screens\/connections\.html: differs from pinned rendering source/));
test("R00-D rendering source symlink escape is rejected", t => invalid(t, (m, root) => {
  const relative = "ketplus-desktop-redesign.ketatlas/screens/connections.html";
  fs.unlinkSync(path.join(root, relative));
  fs.symlinkSync(path.join(repoRoot, relative), path.join(root, relative));
}, /resolves outside repository/));
function invalid(t, mutate, match, options = {}) {
  const f = fixture(t); mutate(f.manifest, f.root); f.save();
  const result = validate({ root: f.root, ...options });
  assert.equal(result.ok, false, "invalid evidence must fail");
  assert.ok(result.errors.every(e => typeof e.code === "string" && typeof e.path === "string" && typeof e.message === "string"));
  assert.equal(result.readiness.nativePixel, "BLOCKED");
  if (match) assert.match(messages(result), match);
  return result;
}

test("valid historical baseline passes structure; independent slices READY and visual gates BLOCKED", t => {
  const f = fixture(t), r = validate({ root: f.root });
  assert.equal(messages(r), ""); assert.equal(r.ok, true);
  assert.deepEqual(r.readiness, { visualReference: "BLOCKED", nativePixel: "BLOCKED" });
  assert.equal(r.manifest.verificationPerformed.nativePixel, "not run");
  assert.equal(r.taskReadiness.find(r => r.task === "R03-R05-R06-Headless-Characterization").status, "READY");
  assert.ok(r.taskReadiness.filter(r => /Implementation|Acceptance/.test(r.task)).every(r => r.status === "BLOCKED"));
});
test("reproducer A1: inventory cannot open legacy consumer implementation", t => invalid(t, m => {
  m.readiness.push({ task: "R03-R13-Consumer-Implementation", evidence: "structure-inventory", status: "READY" });
}, /required set mismatch|enforced task policy/));
test("inventory cannot replace any required contract prerequisite", t => invalid(t, m => {
  const row = m.readiness.find(r => r.task === "R03-Contract-Dependent-Implementation");
  row.prerequisites = ["inventory"]; row.status = "READY";
}, /enforced task policy/));
test("a READY claim with the correct IDs still fails until ALL prerequisites are satisfied", t => invalid(t, m => {
  m.readiness.find(r => r.task === "R03-Contract-Dependent-Implementation").status = "READY";
}, /enforced task policy/));
test("reproducer A2: JSON arrays do not open pixel gate with reference blocked", t => invalid(t, m => {
  const e = m.evidence.find(e => e.id === "native-pixel");
  e.status = "passed"; e.reviewer = "reviewer";
  e.artifacts = [{ id: "fake", type: "source", path: m.artifactPaths.designAtlas, sha256: m.design.atlasSha256 }];
  e.visualEvidence = Object.fromEntries(["referenceArtifacts", "nativeArtifacts", "diffArtifacts"].map(k => [k, e.artifacts]));
}, /approved visual reference ID\/revision/));
test("reproducer A3: truncated assets and empty readiness rejected", t => invalid(t, m => {
  m.assetHashes = m.assetHashes.slice(0, 1); m.readiness = [];
}, /required set mismatch/));
for (const [name, mutate, match] of [
  ["wrong total", m => m.design.inventory.states--, /design.inventory.states: expected 112/],
  ["wrong screen count", m => m.design.statesByScreen.workspace--, /statesByScreen/],
  ["wrong flow node count", m => m.design.nodesByFlow["agent-journey"]--, /nodesByFlow/],
  ["wrong flow edge count", m => m.design.edgesByFlow["agent-journey"]--, /edgesByFlow/],
  ["wrong asset hash", m => m.assetHashes[0].sha256 = "0".repeat(64), /asset design-system-lock: hash mismatch/],
  ["missing task", m => m.readiness.pop(), /readiness: required set mismatch/],
  ["missing evidence", m => m.evidence.pop(), /evidence: required set mismatch/],
  ["duplicate task", m => m.readiness.push(m.readiness[0]), /duplicate task/],
  ["duplicate evidence", m => m.evidence.push(m.evidence[0]), /duplicate id/],
  ["duplicate asset", m => m.assetHashes.push(m.assetHashes[0]), /duplicate id/],
  ["wrong category", m => m.evidence[0].type = "contract-freeze", /wrong evidence category/],
  ["missing plan revision", m => delete m.plan.exactSourceRevision, /plan.exactSourceRevision/],
  ["missing historical revision", m => delete m.repoRefs.mobileDevelop, /repoRefs.mobileDevelop/],
  ["missing branch name", m => delete m.branch.branch, /manifest.branch.branch/],
  ["missing font descriptor", m => delete m.referenceEnvironmentRequired.fonts.ui, /fonts.ui/],
  ["wrong dependency type", m => m.dependencyVersions.toolchain.qt = 611, /toolchain.qt/],
  ["dependency pin drift", m => m.dependencyVersions.designSystem.revision = "0".repeat(40), /pinned dependency manifest mismatch/],
  ["bad timestamp", m => m.evidence[0].recordedAt = "yesterday", /invalid recordedAt/],
  ["impossible timestamp", m => m.recordedAt = "2026-02-30T00:00:00Z", /invalid timestamp/],
  ["malformed hash", m => m.assetHashes[0].sha256 = true, /invalid hash/],
  ["path escape", m => m.artifactPaths.designAtlas = "../outside/atlas.json", /escapes repository/],
  ["artifact missing digest and source", m => { delete m.evidence[0].artifacts[0].sha256; delete m.evidence[0].artifacts[0].sourceRevision; }, /digest or verified immutable source/],
  ["unavailable source revision even with valid digest", m => m.evidence[0].artifacts[0].sourceRevision = "0".repeat(40), /immutable source unavailable/],
  ["artifact hash mismatch", m => m.evidence[0].artifacts[0].sha256 = "0".repeat(64), /artifact hash mismatch/],
  ["reference passed is not approval", m => m.evidence.find(e => e.id === "visual-reference").status = "passed", /reference needs explicit approval/],
  ["approval boolean is insufficient", m => { const e = m.evidence.find(e => e.id === "transcript-freeze"); e.status = "approved"; e.approval = true; }, /approval record required/],
  ["handoff revision must match receipt", m => m.evidence.find(e => e.id === "backend-handoff-review").revision = "0".repeat(40), /receipt revision\/reviewer mismatch/],
]) test("rejects " + name, t => invalid(t, mutate, match));

test("missing asset file rejected", t => invalid(t, (m, root) => {
  fs.unlinkSync(path.join(root, m.assetHashes[1].path));
}, /does not exist/));
test("symlink escape rejected even with valid content/hash", t => invalid(t, (m, root) => {
  const a = m.evidence[0].artifacts[0], target = path.join(root, a.path);
  fs.unlinkSync(target); fs.symlinkSync(path.join(repoRoot, a.path), target);
}, /resolves outside repository/));
test("prefix-sibling escape rejected", t => {
  const f = fixture(t), errors = [];
  resolveRepoPath(f.root, "../" + path.basename(f.root) + "-outside/file", "fixture", errors);
  assert.match(JSON.stringify(errors), /escapes repository/);
});
test("directory names beginning with two dots within root are valid", t => {
  const f = fixture(t), errors = [];
  fs.mkdirSync(path.join(f.root, "..inside")); fs.writeFileSync(path.join(f.root, "..inside/data"), "x");
  assert.ok(resolveRepoPath(f.root, "..inside/data", "fixture", errors)); assert.deepEqual(errors, []);
});
test("Markdown parity checked against the same policy rows", t => invalid(t, (m, root) => {
  fs.appendFileSync(path.join(root, base, "task-readiness.md"), "| extra task | READY |\n");
}, /matrix differs/));
for (const value of [null, [], "text", true, 7]) test("malformed root " + JSON.stringify(value) + " returns structured errors", t => {
  const f = fixture(t);
  fs.writeFileSync(path.join(f.root, base, "baseline-manifest.json"), JSON.stringify(value));
  const r = validate({ root: f.root }); assert.equal(r.ok, false); assert.ok(r.errors.every(e => e.code && e.path && e.message));
});
for (const key of ["readiness", "evidence", "assetHashes", "design", "artifactPaths"]) {
  for (const value of [null, {}, "bad", [null]]) test("malformed " + key + " " + JSON.stringify(value), t => invalid(t, m => m[key] = value));
}
for (const mutate of [
  a => a.flows[0].nodes.pop(),
  a => a.flows[0].nodes[0].screen = "missing",
  a => a.flows[0].nodes[1].id = a.flows[0].nodes[0].id,
  a => a.flows[0].edges[0].to = "missing",
  a => a.screens[0].url = "./screens/nonexistent.html",
]) test("live atlas identity or state changes do not move the reviewed inventory", t => {
  const r = drifted(t, (m, root) => {
    const p = path.join(root, m.artifactPaths.designAtlas), a = JSON.parse(fs.readFileSync(p));
    mutate(a); fs.writeFileSync(p, JSON.stringify(a));
  }, /^atlas: live design differs/m);
  assert.equal(r.atlasInventory.screens, r.manifest.design.inventory.screens);
});
test("a broken live atlas is drift, not a broken inventory", t => drifted(t, (m, root) => {
  fs.writeFileSync(path.join(root, m.artifactPaths.designAtlas), "{broken");
}, /^atlas: live design differs/m));
test("atlas inventory path cannot move away from the reviewed atlas", t => invalid(t, m => {
  m.artifactPaths.designAtlas = "tasks/desktop-refactor/coverage.json";
}, /artifactPaths\.designAtlas: required pinned path/));

test("synthetic scoped reference/native pair passes integrity but cannot open full visual coverage", t => {
  const f = fixture(t); visualFixture(f.manifest, f.root); f.save();
  const r = validate({ root: f.root, testOnly: true });
  assert.equal(messages(r), ""); assert.equal(r.ok, true);
  assert.deepEqual(r.readiness, { visualReference: "BLOCKED", nativePixel: "BLOCKED" });
  assert.equal(r.manifest.verificationPerformed.nativePixel, "not run");
});
test("synthetic evidence rejected in production validation", t => invalid(t, (m, root) => visualFixture(m, root), /test-only/));
for (const [name, mutate, match] of [
  ["reference blocked with pixel passed", ({ref}) => ref.status = "blocked", /approved visual reference ID\/revision/],
  ["wrong reference revision", ({native}) => native.referenceRevision = "0".repeat(40), /approved visual reference ID\/revision/],
  ["missing reviewer", ({ref}) => ref.reviewer = "", /without reviewer/],
  ["missing approval record", ({ref}) => delete ref.approval, /approval record required/],
  ["wrong image type", ({captures}) => captures[1].type = "report", /PNG screenshot required/],
  ["image hash mismatch", ({captures}) => captures[1].sha256 = "0".repeat(64), /artifact hash mismatch/],
  ["dimension metadata mismatch", ({captures}) => captures[1].dimensions.width = 1, /image dimensions mismatch/],
  ["wrong state", ({native}) => native.comparisons[0].state = "invented", /state\/theme mismatch/],
  ["missing pairing", ({native}) => native.comparisons = [], /comparison records required/],
  ["wrong pair digest", ({native}) => native.comparisons[0].nativeSha256 = "0".repeat(64), /comparison digest mismatch/],
  ["wrong pairing role", ({captures}) => captures[2].role = "native", /comparison role mismatch/],
  ["aliased pairing", ({native}) => native.comparisons[0].diffId = "native", /missing or aliased/],
  ["missing environment", ({captures}) => delete captures[0].capture.environment, /capture environment required/],
  ["OS environment mismatch", ({captures}) => captures[1].capture.environment.osBuild = "different", /environment osBuild mismatch/],
  ["unpaired extra image", ({native, captures}) => native.artifacts.push({...captures[1], id: "unpaired"}), /unpaired pixel artifact/],
  ["wrong DPR", ({captures}) => captures[0].capture.dpr = 1, /viewport\/DPR mismatch/],
]) test("synthetic pixel rejects " + name, t => invalid(t, (m, root) => {
  mutate(visualFixture(m, root));
}, match, {testOnly: true}));
test("JSON renamed PNG with matching digest is rejected by decoder", t => invalid(t, (m, root) => {
  const {captures} = visualFixture(m, root), a = captures[1];
  fs.writeFileSync(path.join(root, a.path), "{}"); a.sha256 = hash(Buffer.from("{}"));
}, /invalid PNG signature/, {testOnly: true}));
test("valid PNG of wrong dimensions is rejected", t => invalid(t, (m, root) => {
  const {captures} = visualFixture(m, root), a = captures[1], bytes = png(1, 1);
  fs.writeFileSync(path.join(root, a.path), bytes); a.sha256 = hash(bytes);
}, /image dimensions mismatch/, {testOnly: true}));
test("approval record mismatch is checked even after its digest is updated", t => invalid(t, (m, root) => {
  const {ref} = visualFixture(m, root), p = path.join(root, ref.approval.path);
  const record = JSON.parse(fs.readFileSync(p)); record.reviewer = "other";
  const bytes = Buffer.from(JSON.stringify(record)); fs.writeFileSync(p, bytes); ref.approval.sha256 = hash(bytes);
}, /approval reviewer\/decision mismatch/, {testOnly: true}));
test("distinct browser/native capture tools remain valid with shared reference inputs", t => {
  const f = fixture(t), {captures} = visualFixture(f.manifest, f.root);
  captures[0].capture.environment.renderer = "browser-test-only";
  captures[1].capture.environment.captureTool = "native-test-only"; f.save();
  const r = validate({root: f.root, testOnly: true}); assert.equal(messages(r), "");
});
test("PNG corrupted CRC and incomplete image rejected", () => {
  const valid = png(1, 1); assert.deepEqual(inspectPng(valid), {width: 1, height: 1});
  const corrupt = Buffer.from(valid); corrupt[20] ^= 1;
  assert.throws(() => inspectPng(corrupt), /CRC/);
  assert.throws(() => inspectPng(valid.subarray(0, valid.length - 12)), /missing PNG end/);
});
test("all frozen prerequisites can open only the corresponding implementation slice in TEST ONLY metadata", t => {
  const f = fixture(t);
  for (const id of ["transcript", "relay", "implementationScope"]) {
    const p = policy.prerequisites[id], e = f.manifest.evidence.find(e => e.id === p.evidence);
    e.status = "approved"; e.artifacts = [writeArtifact(f.root, id + "-contract", {testOnly: true, proposal: id})];
    approve(f.root, e, p.role);
  }
  f.manifest.readiness = policy.rows(f.manifest.evidence);
  fs.writeFileSync(path.join(f.root, base, "task-readiness.md"), policy.markdown(f.manifest.readiness));
  f.save();
  const r = validate({root: f.root, testOnly: true});
  assert.equal(messages(r), ""); assert.equal(r.ok, true);
  assert.equal(r.taskReadiness.find(r => r.task === "R03-Contract-Dependent-Implementation").status, "READY");
  assert.equal(r.taskReadiness.find(r => r.task === "R05-Contract-Dependent-Implementation").status, "BLOCKED");
  assert.equal(r.readiness.visualReference, "BLOCKED");
});
