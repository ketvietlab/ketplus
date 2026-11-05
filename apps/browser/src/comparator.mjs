import fs from "node:fs/promises";
import { createHash } from "node:crypto";
import path from "node:path";
import pixelmatch from "pixelmatch";
import { PNG } from "pngjs";
import { BrowserApiError } from "./errors.mjs";
import { LIMITS } from "./protocol.mjs";

const DEFAULT_STYLE_PROPERTIES = [
  "fontFamily",
  "fontSize",
  "fontWeight",
  "lineHeight",
  "letterSpacing",
  "color",
  "backgroundColor",
  "borderTopWidth",
  "borderTopColor",
  "borderRadius",
  "opacity",
];
const ENVIRONMENT_PROPERTIES = [
  "viewport",
  "deviceScaleFactor",
  "theme",
  "locale",
  "reducedMotion",
  "browserEngine",
  "browserVersion",
];
const SEVERITY_ORDER = { error: 0, warning: 1 };

const digest = (bytes) => createHash("sha256").update(bytes).digest("hex");
const semanticKey = (node) => `${node.role}\u0000${node.name}`;
const clip = (value, maxLength = 200) =>
  typeof value === "string" && value.length > maxLength ? `${value.slice(0, maxLength)}…` : value;

function uniqueBy(nodes, keyOf) {
  const groups = Map.groupBy(nodes.filter((node) => keyOf(node)), keyOf);
  return new Map(
    [...groups].filter(([, group]) => group.length === 1).map(([key, [node]]) => [key, node]),
  );
}

export function matchNodes(referenceNodes, candidateNodes, { pairRepeatedGroups = true } = {}) {
  const matched = [];
  const usedReferences = new Set();
  const usedCandidates = new Set();
  const pair = (reference, candidate, strategy, confidence) => {
    matched.push({ reference, candidate, strategy, confidence });
    usedReferences.add(reference.ref);
    usedCandidates.add(candidate.ref);
  };
  const remainingReferences = () => referenceNodes.filter((node) => !usedReferences.has(node.ref));
  const remainingCandidates = () => candidateNodes.filter((node) => !usedCandidates.has(node.ref));

  // A stable identity counts only when it is unique on both sides. Repeated
  // data-testid values are common and must not pair many nodes to one.
  const candidateIds = uniqueBy(candidateNodes, (node) => node.stableId);
  for (const [key, reference] of uniqueBy(referenceNodes, (node) => node.stableId)) {
    const candidate = candidateIds.get(key);
    if (candidate) pair(reference, candidate, "stable-id", 1);
  }

  const candidateKeys = uniqueBy(remainingCandidates(), semanticKey);
  for (const [key, reference] of uniqueBy(remainingReferences(), semanticKey)) {
    const candidate = candidateKeys.get(key);
    if (candidate) pair(reference, candidate, "role-name", 0.9);
  }

  // Repeated controls such as two navigation regions can share an accessible
  // name. Equal-sized groups are paired in document order at low confidence;
  // callers can disable this guess.
  if (pairRepeatedGroups) {
    const groupedCandidates = Map.groupBy(remainingCandidates(), semanticKey);
    for (const [key, references] of Map.groupBy(remainingReferences(), semanticKey)) {
      const candidates = groupedCandidates.get(key) || [];
      if (references.length !== candidates.length) continue;
      references.forEach((reference, index) => pair(reference, candidates[index], "role-name-order", 0.5));
    }
  }

  return { matched, missing: remainingReferences(), extra: remainingCandidates() };
}

function firstLineDifference(expected, actual) {
  const expectedLines = (expected ?? "").split("\n");
  const actualLines = (actual ?? "").split("\n");
  for (let index = 0; index < Math.max(expectedLines.length, actualLines.length); index += 1) {
    if (expectedLines[index] !== actualLines[index]) {
      return {
        line: index + 1,
        expected: clip(expectedLines[index] ?? null),
        actual: clip(actualLines[index] ?? null),
      };
    }
  }
  return null;
}

function compareStructured(reference, candidate, policy) {
  const findings = [];
  const geometryTolerance = policy.geometryTolerance ?? 1;
  const styleProperties = policy.styleProperties ?? DEFAULT_STYLE_PROPERTIES;
  const nodes = matchNodes(reference.nodes, candidate.nodes, {
    pairRepeatedGroups: policy.pairRepeatedGroups ?? true,
  });

  if (reference.ariaSnapshotFull !== candidate.ariaSnapshotFull) {
    findings.push({
      kind: "accessibility-tree",
      severity: "error",
      expectedSha256: digest(Buffer.from(reference.ariaSnapshotFull || "")),
      actualSha256: digest(Buffer.from(candidate.ariaSnapshotFull || "")),
      firstDifference: firstLineDifference(reference.ariaSnapshotFull, candidate.ariaSnapshotFull),
    });
  }

  for (const node of nodes.missing) {
    if (!node.stableId && node.role === "generic") continue;
    findings.push({
      kind: "missing-element",
      severity: "error",
      referenceNode: node.ref,
      stableId: node.stableId,
      role: node.role,
      name: clip(node.name),
    });
  }
  for (const node of nodes.extra) {
    if (!node.stableId && node.role === "generic") continue;
    findings.push({
      kind: "extra-element",
      severity: "warning",
      candidateNode: node.ref,
      stableId: node.stableId,
      role: node.role,
      name: clip(node.name),
    });
  }

  for (const { reference: expected, candidate: actual } of nodes.matched) {
    const pairRefs = { referenceNode: expected.ref, candidateNode: actual.ref };
    if (expected.role !== actual.role || expected.name !== actual.name) {
      findings.push({
        kind: "semantic",
        severity: "error",
        ...pairRefs,
        expected: { role: expected.role, name: clip(expected.name) },
        actual: { role: actual.role, name: clip(actual.name) },
      });
    }
    if (expected.text !== actual.text) {
      findings.push({ kind: "text", severity: "error", ...pairRefs, expected: clip(expected.text), actual: clip(actual.text) });
    }
    const delta = Object.fromEntries(
      ["x", "y", "width", "height"].map((key) => [
        key,
        Math.round((actual.bounds[key] - expected.bounds[key]) * 100) / 100,
      ]),
    );
    if (Object.values(delta).some((value) => Math.abs(value) > geometryTolerance)) {
      findings.push({ kind: "geometry", severity: "error", ...pairRefs, delta });
    }
    if (expected.clipped !== actual.clipped) {
      findings.push({ kind: "clipping", severity: "error", ...pairRefs, expected: expected.clipped, actual: actual.clipped });
    }
    for (const property of styleProperties) {
      if (expected.styles[property] === actual.styles[property]) continue;
      findings.push({
        kind: "style",
        severity: "error",
        ...pairRefs,
        property,
        expected: clip(expected.styles[property]),
        actual: clip(actual.styles[property]),
      });
    }
  }
  return { findings, matched: nodes.matched };
}

// Regions are CSS pixels, like observation bounds. Captures are device
// pixels, so every region is scaled by the shared deviceScaleFactor.
const scaleRegion = (region, scale) => ({
  x: Math.round(region.x * scale),
  y: Math.round(region.y * scale),
  width: Math.round(region.width * scale),
  height: Math.round(region.height * scale),
});

export async function compareRaster(referenceCapture, candidateCapture, diffPath, policy) {
  const scale = referenceCapture.environment.deviceScaleFactor;
  const [referenceBytes, candidateBytes] = await Promise.all([
    fs.readFile(referenceCapture.path),
    fs.readFile(candidateCapture.path),
  ]);
  let reference = PNG.sync.read(referenceBytes);
  let candidate = PNG.sync.read(candidateBytes);
  const crop = (image, region, side) => {
    if (!region) return image;
    const { x, y, width, height } = scaleRegion(region, scale);
    if (width <= 0 || height <= 0 || x + width > image.width || y + height > image.height) {
      throw new BrowserApiError("INVALID_RASTER_REGION", `${side} raster region is outside its capture`, {
        region,
        capture: { width: image.width / scale, height: image.height / scale },
      });
    }
    const result = new PNG({ width, height });
    PNG.bitblt(image, result, x, y, width, height, 0, 0);
    return result;
  };
  reference = crop(reference, policy.rasterRegions?.reference, "Reference");
  candidate = crop(candidate, policy.rasterRegions?.candidate, "Candidate");
  if (reference.width !== candidate.width || reference.height !== candidate.height) {
    throw new BrowserApiError("CAPTURE_DIMENSION_MISMATCH", "Capture dimensions do not match", {
      reference: { width: reference.width, height: reference.height },
      candidate: { width: candidate.width, height: candidate.height },
    });
  }
  // Ignored regions are relative to the compared (possibly cropped) surface.
  const ignored = new Uint8Array(reference.width * reference.height);
  let ignoredPixels = 0;
  for (const region of policy.ignoredRegions ?? []) {
    const left = Math.max(0, Math.floor(region.x * scale));
    const top = Math.max(0, Math.floor(region.y * scale));
    const right = Math.min(reference.width, Math.ceil((region.x + region.width) * scale));
    const bottom = Math.min(reference.height, Math.ceil((region.y + region.height) * scale));
    for (let y = top; y < bottom; y += 1) {
      for (let x = left; x < right; x += 1) {
        const pixel = y * reference.width + x;
        if (!ignored[pixel]) {
          ignored[pixel] = 1;
          ignoredPixels += 1;
        }
        const offset = pixel * 4;
        candidate.data[offset] = reference.data[offset];
        candidate.data[offset + 1] = reference.data[offset + 1];
        candidate.data[offset + 2] = reference.data[offset + 2];
        candidate.data[offset + 3] = reference.data[offset + 3];
      }
    }
  }
  const diff = new PNG({ width: reference.width, height: reference.height });
  const differentPixels = pixelmatch(
    reference.data,
    candidate.data,
    diff.data,
    reference.width,
    reference.height,
    { threshold: policy.pixelThreshold ?? 0.1, includeAA: false },
  );
  const diffBytes = PNG.sync.write(diff);
  await fs.mkdir(path.dirname(diffPath), { recursive: true, mode: 0o700 });
  await fs.writeFile(diffPath, diffBytes, { mode: 0o600 });
  const comparedPixels = reference.width * reference.height - ignoredPixels;
  return {
    width: reference.width,
    height: reference.height,
    scale,
    differentPixels,
    comparedPixels,
    ignoredPixels,
    mismatchRatio: differentPixels / Math.max(1, comparedPixels),
    diffPath,
    diffSha256: digest(diffBytes),
  };
}

export async function compareSnapshots({
  referenceSnapshot,
  candidateSnapshot,
  referenceCapture,
  candidateCapture,
  diffPath,
  policy = {},
}) {
  const referenceEnvironment = referenceCapture.environment;
  const candidateEnvironment = candidateCapture.environment;
  const regions = policy.rasterRegions;
  const widthMismatchAllowed =
    policy.allowViewportWidthMismatch === true &&
    Boolean(regions?.reference && regions?.candidate) &&
    regions.reference.width === regions.candidate.width &&
    regions.reference.height === regions.candidate.height &&
    referenceEnvironment.viewport?.height === candidateEnvironment.viewport?.height;
  const maxPixelMismatchRatio = policy.maxPixelMismatchRatio ?? 0.05;
  const maxFindings = policy.maxFindings ?? LIMITS.defaultMaxFindings;
  const resolvedPolicy = {
    geometryTolerance: policy.geometryTolerance ?? 1,
    pixelThreshold: policy.pixelThreshold ?? 0.1,
    maxPixelMismatchRatio,
    ignoredRegions: policy.ignoredRegions ?? [],
    rasterRegions: regions ?? null,
    allowViewportWidthMismatch: policy.allowViewportWidthMismatch === true,
    pairRepeatedGroups: policy.pairRepeatedGroups ?? true,
    maxFindings,
    regionUnits: "css-px",
  };

  const environmentMismatch = ENVIRONMENT_PROPERTIES.filter(
    (property) =>
      !(property === "viewport" && widthMismatchAllowed) &&
      JSON.stringify(referenceEnvironment[property]) !== JSON.stringify(candidateEnvironment[property]),
  );
  if (environmentMismatch.length) {
    return {
      status: "blocked",
      environmentMismatch,
      policy: resolvedPolicy,
      summary: { errors: 0, warnings: 0, byKind: {} },
      findings: [],
      findingsTotal: 0,
      findingsTruncated: false,
    };
  }

  const structured = compareStructured(referenceSnapshot, candidateSnapshot, policy);
  const raster = await compareRaster(referenceCapture, candidateCapture, diffPath, policy);
  const ordered = structured.findings.toSorted(
    (left, right) => SEVERITY_ORDER[left.severity] - SEVERITY_ORDER[right.severity],
  );
  const summary = { errors: 0, warnings: 0, byKind: {} };
  for (const finding of ordered) {
    summary[finding.severity === "error" ? "errors" : "warnings"] += 1;
    summary.byKind[finding.kind] = (summary.byKind[finding.kind] || 0) + 1;
  }
  return {
    status: summary.errors > 0 || raster.mismatchRatio > maxPixelMismatchRatio ? "failed" : "passed",
    policy: resolvedPolicy,
    summary,
    matchedCount: structured.matched.length,
    ...(policy.includeMatching
      ? {
          matching: structured.matched.map(({ reference, candidate, strategy, confidence }) => ({
            referenceNode: reference.ref,
            candidateNode: candidate.ref,
            strategy,
            confidence,
          })),
        }
      : {}),
    findings: ordered.slice(0, maxFindings),
    findingsTotal: ordered.length,
    findingsTruncated: ordered.length > maxFindings,
    raster,
  };
}
