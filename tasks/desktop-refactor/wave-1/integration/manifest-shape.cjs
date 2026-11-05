const revision = /^[0-9a-f]{40}$/;
const hash = /^[0-9a-f]{64}$/;
const strings = names => Object.fromEntries(names.split(" ").map(name => [name, "string"]));
const viewport = { width: "number", height: "number" };
const schema = {
  ...strings("schemaVersion recordedAt updatedAt scope"),
  branch: { ...strings("branch base pr"), reviewedHead: revision },
  plan: { ...strings("pullRequest ref mergeRelation sourceUse"), exactSourceRevision: revision, observedDevelopMerge: revision },
  design: {
    path: "string", mergeCommit: revision, sourceDesignRevisionFromPlan: revision,
    artifactTreeSha: revision, atlasSha256: hash, viewportLogicalPixels: viewport,
    inventory: Object.fromEntries("screens flows states nodes edges".split(" ").map(k => [k, "number"])),
    statesByScreen: "object", nodesByFlow: "object", edgesByFlow: "object",
  },
  artifactPaths: strings("designAtlas designSystemLock planCoverage planChecker"),
  repoRefs: {
    ...Object.fromEntries("desktopHistoricalBaseline desktopOriginDevelopObservedForR00B desktopPlanPr17 desktopDesignPr16Head cmPinnedGitlink cmOriginDevelop backendIntegration backendDevelop mobileDevelop mobileMain".split(" ").map(k => [k, revision])),
    cmPinnedTag: /^cm-v\d+\.\d+\.\d+$/,
  },
  dependencyVersions: {
    macOS: strings("productVersion buildVersion architecture gpu metal display observedSystemTheme"),
    toolchain: strings("cmake ninja git node npm qt qmake qtpaths"),
    designSystem: { ...strings("package version"), revision, lockFileSha256: hash, registeredComponents: "number" },
    runtimeReferences: strings("ketAtlasCliFromReadme lucideStaticFromNoticesAndAssets interFromLicensing"),
  },
  referenceEnvironmentRequired: {
    ...strings("platform qt renderer webZoom typographyScale captureStatus"),
    contentRectLogicalPixels: viewport, dpr: "number", physicalPixels: viewport,
    themes: "array", fonts: strings("ui source"),
  },
  verificationPerformed: strings("planInventoryChecker designAtlasHeuristic jsonAndLinks gitDiffCheck nativePixel nativePerformance apiE2E providerQuotaTests"),
  assetHashes: "array", evidence: "array", readiness: "array",
};
function validateShape(value, errors, expected = schema, at = "manifest") {
  const isObject = v => v !== null && typeof v === "object" && !Array.isArray(v);
  let valid;
  if (expected instanceof RegExp) valid = typeof value === "string" && expected.test(value);
  else if (typeof expected === "string") {
    valid = expected === "array" ? Array.isArray(value) : expected === "object" ? isObject(value) :
      typeof value === expected && (expected === "string" ? value.trim().length > 0 : Number.isFinite(value));
  } else {
    valid = isObject(value);
    if (valid) for (const [key, child] of Object.entries(expected)) validateShape(value[key], errors, child, at + "." + key);
  }
  if (!valid) errors.push({ code: "schema", path: at, message: "missing or invalid required field" });
}
module.exports = { validateShape };
