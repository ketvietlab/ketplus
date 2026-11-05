// TEST ONLY: generated PNGs and approval records are parser fixtures, never QA evidence.
const { livePath } = require("../../tasks/desktop-refactor/wave-1/integration/evidence-validator.cjs");
const fs = require("node:fs");
const path = require("node:path");
const { createHash } = require("node:crypto");
const { deflateSync } = require("node:zlib");
const { crc32 } = require("../../tasks/desktop-refactor/wave-1/integration/png-integrity.cjs");
const hash = bytes => createHash("sha256").update(bytes).digest("hex");
const timestamp = "2026-09-12T08:00:00.000Z";
const revision = "5f60fca15a5c0f8d3b94109d808a8eaddea1c0ca";
function png(width = 2880, height = 1800) {
  function chunk(type, data) {
    const bytes = Buffer.alloc(data.length + 12);
    bytes.writeUInt32BE(data.length); bytes.write(type, 4); data.copy(bytes, 8);
    bytes.writeUInt32BE(crc32(bytes.subarray(4, bytes.length - 4)), bytes.length - 4);
    return bytes;
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width); header.writeUInt32BE(height, 4); header[8] = 8; header[9] = 6;
  return Buffer.concat([Buffer.from("89504e470d0a1a0a", "hex"), chunk("IHDR", header), chunk("IDAT", deflateSync(Buffer.alloc((width * 4 + 1) * height))), chunk("IEND", Buffer.alloc(0))]);
}
function writeArtifact(root, id, value, type = "report") {
  const relative = "test-only/" + id + (Buffer.isBuffer(value) ? ".png" : ".json");
  const bytes = Buffer.isBuffer(value) ? value : Buffer.from(JSON.stringify(value));
  fs.mkdirSync(path.join(root, "test-only"), { recursive: true });
  fs.writeFileSync(path.join(root, relative), bytes);
  return { id, type, path: relative, sha256: hash(bytes) };
}
function approve(root, e, role) {
  e.reviewer = "Synthetic test reviewer";
  e.provenance = "https://example.invalid/test-only";
  e.recordedAt = timestamp;
  e.testOnly = true;
  e.approval = writeArtifact(root, e.id + "-approval", {
    testOnly: true, evidenceId: e.id, revision: e.revision, reviewer: e.reviewer,
    role, decision: "approved", recordedAt: timestamp, provenance: e.provenance,
    artifactHashes: e.artifacts.map(a => a.sha256),
    ...(e.renderingSource ? { renderingSource: e.renderingSource } : {}),
  }, "approval-record");
}
function visualFixture(manifest, root) {
  const ref = manifest.evidence.find(e => e.id === "visual-reference");
  const native = manifest.evidence.find(e => e.id === "native-pixel");
  const atlas = JSON.parse(fs.readFileSync(path.join(root, livePath(manifest.artifactPaths.designAtlas))));
  const state = atlas.screens[0].url, theme = "dark";
  const fixture = writeArtifact(root, "workload", { testOnly: true, seed: "validator" });
  const environment = {
    platform: "macOS", architecture: "arm64", osVersion: "26.2", osBuild: "25C56",
    qt: "6.11.1", fontFamily: "Inter", fontVersion: "4.1",
    fontSha256: manifest.assetHashes.find(a => a.path.endsWith("inter-latin-standard-normal.woff2")).sha256,
    renderer: "synthetic-test-only", captureTool: "synthetic-test-only", captureToolVersion: "1",
    typographyScale: "115%", webZoom: "100%",
  };
  const bytes = png();
  const captures = ["reference", "native", "diff"].map(role => ({
    ...writeArtifact(root, role, bytes, "screenshot"), role, testOnly: true, mediaType: "image/png",
    dimensions: { width: 2880, height: 1800 },
    capture: { testOnly: true, state, theme, viewport: {width: 1440, height: 900}, dpr: 2,
      recordedAt: timestamp, revision, fixture, fixtureSha256: fixture.sha256, environment: structuredClone(environment) },
  }));
  ref.status = "approved"; ref.revision = revision; ref.artifacts = [captures[0]];
  const rendering = require("../../tasks/desktop-refactor/wave-1/integration/rendering-source.cjs");
  ref.renderingSource = rendering.binding(rendering.derive(root));
  approve(root, ref, "designer/user");
  native.status = "passed"; native.revision = revision; native.artifacts = captures.slice(1);
  native.referenceEvidenceId = ref.id; native.referenceRevision = ref.revision;
  const comparison = { id: "test-only-pair", state, theme, referenceId: "reference", nativeId: "native", diffId: "diff",
    referenceSha256: captures[0].sha256, nativeSha256: captures[1].sha256, diffSha256: captures[2].sha256 };
  comparison.report = writeArtifact(root, "comparison", { testOnly: true, comparisonId: comparison.id,
    referenceSha256: comparison.referenceSha256, nativeSha256: comparison.nativeSha256, diffSha256: comparison.diffSha256,
    result: "passed", tool: "synthetic-parser-test", toolVersion: "1", recordedAt: timestamp });
  native.comparisons = [comparison]; approve(root, native, "design-qa");
  return { ref, native, captures };
}
module.exports = { png, hash, writeArtifact, approve, visualFixture };
