const fs = require("node:fs");
const path = require("node:path");
const crypto = require("node:crypto");
const { execFileSync } = require("node:child_process");
const { isDeepStrictEqual: equal } = require("node:util");
const policy = require("./gate-policy.cjs");
const { inspectPng } = require("./png-integrity.cjs");
const { validateShape } = require("./manifest-shape.cjs");
const rendering = require("./rendering-source.cjs");
const BASE = "tasks/desktop-refactor/wave-1/integration";
const DESIGN = "ketplus-desktop-redesign.ketatlas";
const DESIGN_REVISION = "d0591869113ad4420961bff2ef2f2ac0ea840b58";
const HASH = /^[0-9a-f]{64}$/;
const REV = /^[0-9a-f]{40}$/;
const STAMP = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{3})?(?:Z|[+-]\d{2}:\d{2})$/;
const object = v => v !== null && typeof v === "object" && !Array.isArray(v);
const string = v => typeof v === "string" && v.trim().length > 0;
const hash = b => crypto.createHash("sha256").update(b).digest("hex");
const stamped = v => typeof v === "string" && STAMP.test(v) && +v.slice(11,13) < 24 && +v.slice(14,16) < 60 && +v.slice(17,19) < 60 && Number.isFinite(Date.parse(v)) &&
  new Date(Date.UTC(+v.slice(0,4), +v.slice(5,7)-1, +v.slice(8,10))).toISOString().slice(0,10) === v.slice(0,10);
function issue(errors, at, message, code = "invalid") { errors.push({ code, path: at, message }); }
function contained(root, target) {
  const rel = path.relative(root, target);
  return rel === "" || (rel !== ".." && !rel.startsWith(`..${path.sep}`) && !path.isAbsolute(rel));
}
function resolveRepoPath(root, relative, label, errors) {
  if (!string(relative) || path.isAbsolute(relative) || relative.includes("\\") || relative.includes("\0")) {
    issue(errors, label, "must be a repository-relative path"); return null;
  }
  const target = path.resolve(root, relative);
  if (!contained(root, target)) { issue(errors, label, "escapes repository"); return null; }
  try {
    if (!contained(fs.realpathSync(root), fs.realpathSync(target))) { issue(errors, label, "resolves outside repository"); return null; }
    if (!fs.statSync(target).isFile()) { issue(errors, label, "must be a regular file"); return null; }
    return target;
  } catch { issue(errors, label, "does not exist or cannot be read"); return null; }
}
function computeAtlasInventory(atlas) {
  const screenUrls = new Map();
  const screenIds = new Set();
  const allUrls = new Set();
  const nodesByFlow = {};
  const edgesByFlow = {};
  let nodes = 0;
  let edges = 0;

  for (const screen of atlas.screens || []) {
    screenIds.add(screen.id);
    if (!screenUrls.has(screen.id)) screenUrls.set(screen.id, new Set());
    if (screen.url) {
      screenUrls.get(screen.id).add(screen.url);
      allUrls.add(screen.url);
    }
  }

  for (const flow of atlas.flows || []) {
    const flowNodes = flow.nodes || [];
    const flowEdges = flow.edges || [];
    nodesByFlow[flow.id] = flowNodes.length;
    edgesByFlow[flow.id] = flowEdges.length;
    nodes += flowNodes.length;
    edges += flowEdges.length;

    for (const node of flowNodes) {
      if (!screenIds.has(node.screen)) {
        continue;
      }
      const fallback = (atlas.screens || []).find((screen) => screen.id === node.screen)?.url;
      const url = node.url || fallback;
      if (url) {
        if (!screenUrls.has(node.screen)) screenUrls.set(node.screen, new Set());
        screenUrls.get(node.screen).add(url);
        allUrls.add(url);
      }
    }
  }

  const statesByScreen = {};
  for (const [screen, urls] of screenUrls.entries()) {
    statesByScreen[screen] = urls.size;
  }

  return {
    screens: (atlas.screens || []).length,
    flows: (atlas.flows || []).length,
    states: allUrls.size,
    nodes,
    edges,
    statesByScreen,
    nodesByFlow,
    edgesByFlow,
  };
}


function validate({ root = process.cwd(), manifestPath = BASE + "/baseline-manifest.json", testOnly = false, checkMatrix = true } = {}) {
  const errors = [], warnings = [];
  const readiness = { visualReference: "BLOCKED", nativePixel: "BLOCKED" };
  let manifest, atlasInventory, taskReadiness, renderingInventory;
  const check = (ok, at, message) => { if (!ok) issue(errors, at, message); return ok; };
  const git = (...args) => execFileSync("git", ["-C", root, ...args], { maxBuffer: 64*1024*1024, stdio: ["ignore", "pipe", "pipe"] });
  const json = (bytes, at) => { try { return JSON.parse(bytes.toString()); } catch { issue(errors, at, "invalid JSON"); return null; } };
  const read = (p, at) => { const f = resolveRepoPath(root, p, at, errors); return f ? fs.readFileSync(f) : null; };
  // The design bundle keeps evolving after its review. Evidence binds to the pinned
  // revision, so a live copy that differs (or is gone) is reported but does not fail.
  // A missing file is drift; a path that escapes the repository is still an error.
  const readLive = (p, at) => {
    const found = [], file = resolveRepoPath(root, p, at, found);
    for (const e of found) if (e.message !== "does not exist or cannot be read") errors.push(e);
    return file && !found.length ? fs.readFileSync(file) : null;
  };
  const designDrift = (p, pinned, at) => {
    const live = readLive(p, at);
    if (!live || !live.equals(pinned)) issue(warnings, at, "live design differs from reviewed revision " + DESIGN_REVISION.slice(0, 12), "drift");
  };
  const blob = (rev, p, at) => {
    if (!check(typeof rev === "string" && REV.test(rev), at, "invalid immutable revision")) return null;
    try { return git("show", rev + ":" + p); } catch { issue(errors, at, "immutable source unavailable; fetch required revision"); return null; }
  };
  const unique = (list, key, at) => {
    if (!check(Array.isArray(list), at, "must be an array")) return [];
    const seen = new Set();
    for (const row of list) {
      if (!check(object(row) && string(row[key]), at, "object with nonempty " + key + " required")) continue;
      check(!seen.has(row[key]), at, "duplicate " + key + ": " + row[key]); seen.add(row[key]);
    }
    return list.filter(object);
  };
  const sameSet = (actual, expected, at) => check(Array.isArray(actual) && equal([...actual].sort(), [...expected].sort()), at, "required set mismatch");
  function artifact(a, at) {
    if (!check(object(a), at, "artifact object required")) return null;
    check(["source", "report", "approval-record", "screenshot"].includes(a.type), at, "invalid artifact type");
    if (typeof a.path === "string" && a.path.split("/").includes("test-only")) check(testOnly, at, "test-only artifact prohibited outside synthetic tests");
    const pinnedDesign = typeof a.path === "string" && a.path.startsWith(DESIGN + "/") && a.sourceRevision !== undefined;
    let bytes = pinnedDesign ? null : read(a.path, at);
    check(HASH.test(a.sha256 || "") || REV.test(a.sourceRevision || ""), at, "digest or verified immutable source required");
    if (a.sha256 !== undefined) check(typeof a.sha256 === "string" && HASH.test(a.sha256), at, "invalid sha256");
    if (a.sourceRevision !== undefined) {
      const source = blob(a.sourceRevision, a.path, at + ".sourceRevision");
      if (pinnedDesign) {
        bytes = source;
        if (source) designDrift(a.path, source, at);
      } else if (bytes && source) check(bytes.equals(source), at, "artifact differs from immutable source");
    }
    if (bytes && a.sha256) check(hash(bytes) === a.sha256, at, "artifact hash mismatch");
    return bytes;
  }
  function approval(e, role) {
    const at = "evidence." + e.id + ".approval";
    if (!check(object(e.approval), at, "approval record required")) return;
    check(e.approval.type === "approval-record", at, "approval-record type required");
    const record = json(artifact(e.approval, at), at);
    if (!check(object(record), at, "approval object required")) return;
    check(record.evidenceId === e.id && record.revision === e.revision, at, "approval evidence/revision mismatch");
    check(record.decision === "approved" && record.reviewer === e.reviewer && string(record.reviewer), at, "approval reviewer/decision mismatch");
    check(e.artifacts.every(a => typeof a.sha256 === "string" && HASH.test(a.sha256)), at, "approval must bind artifact digests");
    check(string(role) && record.role === role, at, "required reviewer role: " + role);
    check(stamped(record.recordedAt) && Date.parse(record.recordedAt) <= Date.parse(e.recordedAt), at, "invalid approval timestamp/order");
    check(typeof record.provenance === "string" && /^https:\/\/[^\s]+$/.test(record.provenance), at, "approval provenance URL required");
    if (record.testOnly || e.testOnly) check(testOnly && record.testOnly === true && e.testOnly === true, at, "test-only approval prohibited outside synthetic tests");
    sameSet(record.artifactHashes, e.artifacts.map(a => a.sha256), at + ".artifactHashes");
    if (e.type === "visual-reference") check(equal(record.renderingSource, rendering.binding(renderingInventory)), at, "approval rendering source binding mismatch");
  }
  const states = new Set();
  function capture(a, e) {
    const at = "evidence." + e.id + ".capture." + a.id;
    const bytes = artifact(a, at);
    check(a.type === "screenshot" && a.mediaType === "image/png" && typeof a.path === "string" && a.path.endsWith(".png"), at, "PNG screenshot required");
    check(typeof a.sha256 === "string" && HASH.test(a.sha256), at, "screenshot digest required");
    let dimensions;
    try { dimensions = inspectPng(bytes); } catch (error) { issue(errors, at, error.message); }
    const m = a.capture;
    if (!check(object(m), at, "capture metadata required")) return;
    check(states.has(m.state) && ["light", "dark"].includes(m.theme), at, "unknown state/theme");
    check(equal(m.viewport, { width: 1440, height: 900 }) && m.dpr === 2, at, "logical viewport/DPR mismatch");
    check(dimensions?.width === 2880 && dimensions?.height === 1800 && equal(a.dimensions, dimensions), at, "image dimensions mismatch");
    check(stamped(m.recordedAt), at, "invalid capture timestamp");
    check(REV.test(m.revision || "") && m.revision === e.revision, at, "capture revision mismatch");
    check(typeof m.fixtureSha256 === "string" && HASH.test(m.fixtureSha256), at, "fixture digest required");
    const fixture = artifact(m.fixture, at + ".fixture");
    if (fixture) check(hash(fixture) === m.fixtureSha256, at, "fixture digest mismatch");
    const env = m.environment;
    if (!check(object(env), at, "capture environment required")) return;
    for (const field of ["osVersion", "osBuild", "qt", "fontVersion", "fontFamily", "renderer", "captureTool", "captureToolVersion"]) check(string(env[field]), at + "." + field, "required environment string");
    check(env.platform === "macOS" && env.architecture === "arm64" && env.qt === "6.11.1" && env.typographyScale === "115%" && env.webZoom === "100%", at, "environment differs from locked reference");
    check(typeof env.fontSha256 === "string" && HASH.test(env.fontSha256) && manifest.assetHashes.some(a => a.sha256 === env.fontSha256 && a.path.endsWith(".woff2")), at, "font must match pinned asset");
    check(["reference", "native", "diff"].includes(a.role), at, "invalid capture role");
    if (a.testOnly || m.testOnly || e.testOnly) check(testOnly && a.testOnly === true && m.testOnly === true && e.testOnly === true, at, "test-only capture prohibited outside synthetic tests");
  }
  function fullCoverage(e) {
    const keys = e.type === "pixel-comparison" ? (e.comparisons || []).map(c => c.state + "|" + c.theme) :
      (e.artifacts || []).map(a => a.capture?.state + "|" + a.capture?.theme);
    return keys.length === states.size * 2 && new Set(keys).size === keys.length &&
      [...states].every(s => ["light", "dark"].every(t => keys.includes(s + "|" + t)));
  }
  try {
    root = fs.realpathSync(root);
    manifest = json(read(manifestPath, "manifest"), "manifest");
    if (!check(object(manifest), "manifest", "must be an object")) return { ok: false, errors, warnings, readiness };
    validateShape(manifest, errors);
    renderingInventory = rendering.derive(root);
    const receipt = json(read(BASE + "/rendering-source-inventory.json", "renderingSource"), "renderingSource");
    check(equal(receipt, renderingInventory), "renderingSource", "receipt differs from immutable rendering source inventory");
    for (const entry of renderingInventory.entries.filter(e => e.category === "rendering-source")) {
      const live = readLive(entry.path, "renderingSource." + entry.path);
      if (!live || hash(live) !== entry.sha256) issue(warnings, "renderingSource." + entry.path, "differs from pinned rendering source", "drift");
    }
    check(manifest.schemaVersion === "ketplus.desktop.refactor.integration-baseline.v1", "schemaVersion", "invalid schemaVersion");
    for (const key of ["recordedAt", "updatedAt"]) check(stamped(manifest[key]), key, "invalid timestamp");
    for (const [key, value] of Object.entries({ "branch.reviewedHead": manifest.branch?.reviewedHead, "plan.exactSourceRevision": manifest.plan?.exactSourceRevision, "design.mergeCommit": manifest.design?.mergeCommit })) check(typeof value === "string" && REV.test(value), key, "40-char revision required");
    check(manifest.design?.mergeCommit === DESIGN_REVISION, "design.mergeCommit", "pinned design revision cannot change through evidence");
    for (const key of ["design", "artifactPaths", "repoRefs", "dependencyVersions", "referenceEnvironmentRequired", "verificationPerformed"]) check(object(manifest[key]), key, "required object");
    for (const key of ["desktopHistoricalBaseline", "desktopOriginDevelopObservedForR00B", "desktopPlanPr17", "desktopDesignPr16Head", "cmPinnedGitlink", "cmOriginDevelop", "backendIntegration", "backendDevelop", "mobileDevelop", "mobileMain"]) check(typeof manifest.repoRefs?.[key] === "string" && REV.test(manifest.repoRefs[key]), "repoRefs." + key, "required immutable revision");
    for (const key of ["sourceDesignRevisionFromPlan", "artifactTreeSha"]) check(typeof manifest.design?.[key] === "string" && REV.test(manifest.design[key]), "design." + key, "required revision/hash");
    check(equal(manifest.design?.viewportLogicalPixels, { width: 1440, height: 900 }), "design.viewportLogicalPixels", "locked viewport required");
    const environment = manifest.referenceEnvironmentRequired;
    check(environment?.platform === "macOS arm64" && environment.dpr === 2 && equal(environment.contentRectLogicalPixels, {width: 1440, height: 900}) && equal(environment.physicalPixels, {width: 2880, height: 1800}) && environment.typographyScale === "115%" && environment.webZoom === "100%" && equal(environment.themes, ["light", "dark"]), "referenceEnvironmentRequired", "locked environment mismatch");
    for (const key of ["macOS", "toolchain", "designSystem", "runtimeReferences"]) check(object(manifest.dependencyVersions?.[key]), "dependencyVersions." + key, "required object");
    check(REV.test(manifest.dependencyVersions?.designSystem?.revision || "") && HASH.test(manifest.dependencyVersions?.designSystem?.lockFileSha256 || ""), "dependencyVersions.designSystem", "required revision/hash");
    const expectedPaths = { designAtlas: DESIGN + "/atlas.json", designSystemLock: DESIGN + "/design-system.lock.json", planCoverage: "tasks/desktop-refactor/coverage.json", planChecker: "tasks/desktop-refactor/check-plan.cjs" };
    for (const [key, p] of Object.entries(expectedPaths)) check(manifest.artifactPaths?.[key] === p, "artifactPaths." + key, "required pinned path");
    for (const [key, p] of Object.entries(manifest.artifactPaths || {})) resolveRepoPath(root, p, "artifactPaths." + key, errors);
    // The inventory comes from the reviewed atlas, not from the live one.
    const atlasBytes = manifest.artifactPaths?.designAtlas === DESIGN + "/atlas.json" ? blob(DESIGN_REVISION, DESIGN + "/atlas.json", "atlas.source") : null;
    if (atlasBytes) designDrift(DESIGN + "/atlas.json", atlasBytes, "atlas");
    const atlas = atlasBytes ? json(atlasBytes, "atlas") : null;
    if (object(atlas)) {
      check(HASH.test(manifest.design?.atlasSha256 || "") && hash(atlasBytes) === manifest.design?.atlasSha256, "design.atlasSha256", "hash mismatch");
      const screens = unique(atlas.screens, "id", "atlas.screens");
      const flows = unique(atlas.flows, "id", "atlas.flows");
      const screenMap = new Map(screens.map(s => [s.id, s]));
      for (const s of screens) { check(string(s.url), "atlas.screen." + s.id, "state URL required"); states.add(s.url); }
      for (const f of flows) {
        const nodes = unique(f.nodes, "id", "atlas.flow." + f.id);
        for (const n of nodes) {
          if (n.type === "note" || n.type === "external") continue;
          check(screenMap.has(n.screen), "atlas.node." + n.id, "unknown screen/state");
          const url = n.url || screenMap.get(n.screen)?.url;
          check(string(url), "atlas.node." + n.id, "state URL required"); states.add(url);
        }
        const ids = new Set(nodes.map(n => n.id));
        check(Array.isArray(f.edges), "atlas.edges", "array required");
        for (const edge of f.edges || []) check(object(edge) && ids.has(edge.from) && ids.has(edge.to), "atlas.edges", "unknown node endpoint");
        check(ids.has(f.start) && Array.isArray(f.ends) && f.ends.every(id => ids.has(id)), "atlas.flow", "unknown start/end");
      }
      atlasInventory = computeAtlasInventory(atlas);
      for (const key of ["screens", "flows", "states", "nodes", "edges"]) check(atlasInventory[key] === manifest.design?.inventory?.[key], "design.inventory." + key, "expected " + atlasInventory[key] + " from atlas");
      for (const key of ["statesByScreen", "nodesByFlow", "edgesByFlow"]) check(equal(atlasInventory[key], manifest.design?.[key]), "design." + key, "per-ID inventory mismatch");
    } else check(false, "atlas", "required object");
    const assets = unique(manifest.assetHashes, "id", "assetHashes");
    check(new Set(assets.map(a => a.path)).size === assets.length, "assetHashes", "duplicate asset path");
    // Inventory is read from the pinned design tree, not the submitted hash list.
    let required = [];
    try { required = git("ls-tree", "-r", "--name-only", DESIGN_REVISION, DESIGN + "/assets").toString().trim().split("\n").filter(Boolean); } catch { issue(errors, "assetHashes", "pinned design tree unavailable"); }
    required.push(DESIGN + "/design-system.lock.json");
    const lockBytes = blob(DESIGN_REVISION, DESIGN + "/design-system.lock.json", "designSystem.lock");
    const lock = json(lockBytes, "designSystem.lock");
    const ds = manifest.dependencyVersions?.designSystem;
    check(ds?.revision === lock?.revision && ds?.version === lock?.version && ds?.package === lock?.source && ds?.registeredComponents === lock?.registeredComponents && lockBytes && ds?.lockFileSha256 === hash(lockBytes), "dependencyVersions.designSystem", "pinned dependency manifest mismatch");
    sameSet(assets.map(a => a.path), required, "assetHashes");
    for (const a of assets) {
      const at = "asset " + a.id;
      check(typeof a.sha256 === "string" && HASH.test(a.sha256), at, "invalid hash");
      // Retain historical documentation receipts without making future doc edits rendering drift.
      const documentation = renderingInventory.entries.some(e => e.path === a.path && e.category === "documentation");
      const bytes = documentation ? blob(DESIGN_REVISION, a.path, at) : read(a.path, at);
      const source = bytes && blob(DESIGN_REVISION, a.path, at);
      if (bytes) check(hash(bytes) === a.sha256, at, "hash mismatch");
      if (source) check(hash(source) === a.sha256, at, "differs from pinned design asset");
    }
    const evidence = unique(manifest.evidence, "id", "evidence");
    sameSet(evidence.map(e => e.id), Object.keys(policy.evidenceTypes), "evidence");
    const byId = new Map(evidence.map(e => [e.id, e]));
    for (const e of evidence) {
      const at = "evidence." + e.id;
      if (e.id === "visual-reference" && e.status === "approved") check(equal(e.renderingSource, rendering.binding(renderingInventory)), at, "reference rendering source binding mismatch");
      check(e.type === policy.evidenceTypes[e.id], at, "wrong evidence category");
      check(["pending", "not-run", "missing", "passed", "blocked", "reviewed", "approved"].includes(e.status), at, "invalid evidence status");
      check(stamped(e.recordedAt), at, "invalid recordedAt");
      check(typeof e.revision === "string" && REV.test(e.revision), at, "invalid immutable revision");
      const artifacts = unique(e.artifacts, "id", at + ".artifacts");
      if (["passed", "reviewed", "approved"].includes(e.status)) {
        check(string(e.reviewer), at, "cannot be " + e.status + " without reviewer");
        check(artifacts.length > 0, at, "cannot be " + e.status + " without artifacts");
        check(string(e.provenance), at, "review provenance required");
      } else check(string(e.reason), at, "reason required");
      if (e.testOnly) check(testOnly && e.testOnly === true, at, "synthetic evidence is test-only");
      for (const a of artifacts) artifact(a, at + ".artifact." + a.id);
      if (e.type === "handoff-review" && e.status === "reviewed") {
        const receiptArtifact = artifacts.find(a => a.path === BASE + "/owner-review-sources.json");
        const receipt = receiptArtifact && json(artifact(receiptArtifact, at + ".receipt"), at);
        const repositories = { "backend-handoff-review": "ketvietlab/ketviet", "cm-handoff-review": "ketvietlab/ketplus", "core-handoff-review": "ketvietlab/ketplus-desktop" };
        const source = receipt?.sources?.find(s => s.repository === repositories[e.id]);
        check(source?.revision === e.revision && receipt?.reviewer === e.reviewer && stamped(receipt?.recordedAt), at, "handoff receipt revision/reviewer mismatch");
        const sources = unique(source?.artifacts, "path", at + ".sources");
        check(sources.length > 0, at, "source receipts required");
        for (const s of sources) check(typeof s.sha256 === "string" && HASH.test(s.sha256) && s.url === "https://github.com/" + source.repository + "/blob/" + source.revision + "/" + s.path, at, "invalid immutable source receipt");
      }
      const p = Object.values(policy.prerequisites).find(p => p.evidence === e.id);
      if (e.status === "approved" || (e.type === "pixel-comparison" && e.status === "passed")) approval(e, p?.role);
      if (e.type === "visual-reference" && ["approved", "passed"].includes(e.status)) {
        check(e.status === "approved", at, "reference needs explicit approval");
        for (const a of artifacts) { capture(a, e); check(a.role === "reference", at, "reference role required"); }
      }
      if (e.type === "pixel-comparison" && ["passed", "approved"].includes(e.status)) {
        const ref = byId.get(e.referenceEvidenceId);
        check(ref?.id === "visual-reference" && ref.status === "approved" && ref.revision === e.referenceRevision, at, "pixel requires matching approved visual reference ID/revision");
        const pairs = unique(e.comparisons, "id", at + ".comparisons");
        check(pairs.length > 0, at, "comparison records required");
        const used = new Set();
        for (const c of pairs) {
          const r = ref?.artifacts?.find(a => a.id === c.referenceId), n = artifacts.find(a => a.id === c.nativeId), d = artifacts.find(a => a.id === c.diffId);
          check(r && n && d && n !== d, at, "comparison artifacts missing or aliased");
          for (const a of [r, n, d].filter(Boolean)) {
            check(a.capture?.state === c.state && a.capture?.theme === c.theme, at, "comparison state/theme mismatch");
            check(!used.has(a.id), at, "duplicate comparison pairing"); used.add(a.id);
          }
          check(n?.role === "native" && d?.role === "diff", at, "comparison role mismatch");
          check(r?.sha256 === c.referenceSha256 && n?.sha256 === c.nativeSha256 && d?.sha256 === c.diffSha256, at, "comparison digest mismatch");
          if (r && n && d) {
            for (const field of ["viewport", "dpr", "fixtureSha256"]) check(equal(r.capture?.[field], n.capture?.[field]) && equal(n.capture?.[field], d.capture?.[field]), at, "comparison " + field + " mismatch");
            // Browser/native/diff tools and renderers differ; shared reference inputs must not.
            for (const field of ["platform", "architecture", "osVersion", "osBuild", "qt", "fontVersion", "fontFamily", "fontSha256", "typographyScale", "webZoom"]) check(equal(r.capture?.environment?.[field], n.capture?.environment?.[field]) && equal(n.capture?.environment?.[field], d.capture?.environment?.[field]), at, "comparison environment " + field + " mismatch");
            check(new Set([r.path, n.path, d.path]).size === 3, at, "comparison files must be distinct");
          }
          const report = json(artifact(c.report, at + ".comparison.report"), at);
          check(object(report) && report.comparisonId === c.id && report.referenceSha256 === c.referenceSha256 && report.nativeSha256 === c.nativeSha256 && report.diffSha256 === c.diffSha256 && report.result === "passed" && string(report.tool) && string(report.toolVersion) && stamped(report.recordedAt), at, "comparison report pairing/provenance mismatch");
        }
        for (const a of artifacts) { capture(a, e); check(used.has(a.id), at, "unpaired pixel artifact"); }
      }
    }
    taskReadiness = policy.rows(evidence, fullCoverage);
    const rows = unique(manifest.readiness, "task", "readiness");
    sameSet(rows.map(r => r.task), policy.tasks.map(r => r.task), "readiness");
    for (const row of rows) check(equal(row, taskReadiness.find(r => r.task === row.task)), "readiness." + row.task, "prerequisites/reviewer/status differ from enforced task policy");
    if (checkMatrix) {
      const matrix = read(BASE + "/task-readiness.md", "readiness Markdown");
      if (matrix) check(matrix.toString() === policy.markdown(taskReadiness), "readiness Markdown", "matrix differs from manifest/policy");
    }
    if (errors.length === 0) {
      readiness.visualReference = byId.get("visual-reference")?.status === "approved" && fullCoverage(byId.get("visual-reference")) ? "READY" : "BLOCKED";
      readiness.nativePixel = byId.get("native-pixel")?.status === "passed" && readiness.visualReference === "READY" && fullCoverage(byId.get("native-pixel")) ? "READY" : "BLOCKED";
    }
  } catch (error) { issue(errors, "input", "malformed or unreadable validation data: " + error.message, "malformed"); }
  if (errors.length) taskReadiness = taskReadiness?.map(row => ({ ...row, status: "BLOCKED" }));
  return { ok: errors.length === 0, errors, warnings, manifest, atlasInventory, readiness, taskReadiness };
}
module.exports = { validate, computeAtlasInventory, resolveRepoPath };
