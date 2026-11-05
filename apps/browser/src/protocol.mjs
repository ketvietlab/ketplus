import * as z from "zod/v4";
import { BrowserApiError } from "./errors.mjs";

// The canonical browser.v0 wire contract. Every transport validates against
// these schemas through BrowserService.dispatch; engine adapters never see
// unvalidated parameters.

export const PROTOCOL_VERSION = "browser.v0";

export const LIMITS = Object.freeze({
  maxViewportSize: 4096,
  maxDeviceScaleFactor: 3,
  defaultMaxNodes: 300,
  maxNodes: 1000,
  defaultMaxAriaChars: 20_000,
  maxAriaChars: 100_000,
  // A summary observation (the default) leaves out computed styles, clips node text and the
  // ARIA snapshot, and stops adding nodes at a byte budget; detail "full" returns everything.
  defaultSummaryNodes: 150,
  defaultSummaryAriaChars: 6_000,
  summaryNodeTextChars: 160,
  summaryMaxBytes: 32_000,
  maxTimeoutMs: 120_000,
  defaultMaxFindings: 200,
  maxFindings: 1000,
  maxAllowedOrigins: 32,
  maxEvidenceContextBytes: 4096,
  // Page diagnostics (browser.logs, browser.network) keep a bounded buffer per page.
  maxLogEntries: 500,
  defaultLogEntries: 100,
  logTextChars: 2_000,
  logStackChars: 4_000,
  maxNetworkEntries: 1_000,
  defaultNetworkEntries: 100,
  // browser.inspect reads at most this many elements and fields per call.
  defaultInspectElements: 10,
  maxInspectElements: 50,
  maxInspectFields: 32,
  inspectMaxAttributes: 64,
  inspectAttributeChars: 500,
  inspectValueChars: 2_000,
  inspectTextChars: 1_000,
});

const isHttpUrl = (value) => {
  try {
    return ["http:", "https:"].includes(new URL(value).protocol);
  } catch {
    return false;
  }
};

// Playwright selector engines (text=, xpath=, role=, >> chains, internal:) are
// not part of the protocol. Only standard CSS selectors are accepted.
const ENGINE_SELECTOR = />>|^\s*[a-z_-]+\s*=|^\s*\/\/|^\s*\.\.|internal:/i;

const id = z.string().min(1).max(160);
const timeoutMs = z.number().int().positive().max(LIMITS.maxTimeoutMs);
const httpUrl = z.string().max(4096).refine(isHttpUrl, "must be an absolute http(s) URL");
const label = z.string().max(120);
const waitUntil = z.enum(["commit", "domcontentloaded", "load", "networkidle"]);
const cssSelector = z
  .string()
  .min(1)
  .max(512)
  .refine((value) => !ENGINE_SELECTOR.test(value), "must be a standard CSS selector");
const nodeTarget = z.object({ nodeRef: id }).strict();
const region = (minimumSize) =>
  z
    .object({
      x: z.number().nonnegative(),
      y: z.number().nonnegative(),
      width: minimumSize === "positive" ? z.number().positive() : z.number().nonnegative(),
      height: minimumSize === "positive" ? z.number().positive() : z.number().nonnegative(),
    })
    .strict();
const evidenceContext = z
  .record(z.string().max(64), z.union([z.string().max(512), z.number(), z.boolean(), z.null()]))
  .refine(
    (value) => Buffer.byteLength(JSON.stringify(value)) <= LIMITS.maxEvidenceContextBytes,
    `must serialize to at most ${LIMITS.maxEvidenceContextBytes} bytes`,
  );
const pageParams = { sessionId: id, pageId: id };

export const ACTION_SCHEMA = z.discriminatedUnion("type", [
  z.object({ type: z.literal("click"), target: nodeTarget, timeoutMs: timeoutMs.optional() }).strict(),
  z.object({ type: z.literal("fill"), target: nodeTarget, value: z.string().max(100_000), timeoutMs: timeoutMs.optional() }).strict(),
  z.object({ type: z.literal("press"), target: nodeTarget, key: z.string().min(1).max(64), timeoutMs: timeoutMs.optional() }).strict(),
  z.object({ type: z.literal("hover"), target: nodeTarget, timeoutMs: timeoutMs.optional() }).strict(),
  z.object({ type: z.literal("check"), target: nodeTarget, checked: z.boolean().optional(), timeoutMs: timeoutMs.optional() }).strict(),
  z.object({
    type: z.literal("select"),
    target: nodeTarget,
    value: z.string().max(1000).optional(),
    values: z.array(z.string().max(1000)).max(100).optional(),
    timeoutMs: timeoutMs.optional(),
  }).strict(),
  z.object({
    type: z.literal("scroll"),
    target: nodeTarget.optional(),
    deltaX: z.number().min(-100_000).max(100_000).optional(),
    deltaY: z.number().min(-100_000).max(100_000).optional(),
    timeoutMs: timeoutMs.optional(),
  }).strict(),
  z.object({ type: z.literal("navigate"), url: httpUrl, waitUntil: waitUntil.optional(), timeoutMs: timeoutMs.optional() }).strict(),
  // Session history, as the browser's Back and Forward buttons.
  z.object({
    type: z.literal("history"),
    direction: z.enum(["back", "forward"]),
    waitUntil: waitUntil.optional(),
    timeoutMs: timeoutMs.optional(),
  }).strict(),
  z.object({ type: z.literal("reload"), ignoreCache: z.boolean().optional(), waitUntil: waitUntil.optional(), timeoutMs: timeoutMs.optional() }).strict(),
]);

export const LOG_LEVELS = ["error", "warning", "info", "log", "debug"];
export const LOG_KINDS = ["console", "pageerror", "unhandledrejection", "requestfailed", "http"];
const cursor = z.number().int().nonnegative();
const inspectField = z.union([
  z.enum(["attributes", "scroll", "value", "bounds", "text"]),
  z.string().regex(/^computedStyle:(?:--[A-Za-z0-9_-]{1,64}|[A-Za-z-]{1,64})$/, "computedStyle:<css property>"),
]);

export const WAIT_CONDITION_SCHEMA = z.discriminatedUnion("type", [
  z.object({ type: z.literal("load"), state: z.enum(["domcontentloaded", "load", "networkidle"]).optional() }).strict(),
  z.object({
    type: z.literal("text"),
    text: z.string().min(1).max(500),
    exact: z.boolean().optional(),
    state: z.enum(["attached", "detached", "visible", "hidden"]).optional(),
  }).strict(),
  z.object({ type: z.literal("url"), equals: httpUrl.optional(), startsWith: z.string().min(1).max(4096).optional() }).strict(),
  z.object({ type: z.literal("settled") }).strict(),
]);

const comparePolicy = z.object({
  geometryTolerance: z.number().nonnegative().max(10_000).optional(),
  pixelThreshold: z.number().min(0).max(1).optional(),
  maxPixelMismatchRatio: z.number().min(0).max(1).optional(),
  styleProperties: z.array(z.string().max(64)).max(64).optional(),
  ignoredRegions: z.array(region("nonnegative")).max(64).optional(),
  rasterRegions: z.object({ reference: region("positive"), candidate: region("positive") }).strict().optional(),
  allowViewportWidthMismatch: z.boolean().optional(),
  pairRepeatedGroups: z.boolean().optional(),
  maxFindings: z.number().int().positive().max(LIMITS.maxFindings).optional(),
  includeMatching: z.boolean().optional(),
}).strict();

const evidenceRef = z.object({ snapshotId: id, captureId: id }).strict();

export const PARAMS = Object.freeze({
  "browser.capabilities": z.object({}).strict(),
  "browser.protocol.schema": z.object({}).strict(),
  "browser.session.create": z.object({
    viewport: z.object({
      width: z.number().int().min(1).max(LIMITS.maxViewportSize),
      height: z.number().int().min(1).max(LIMITS.maxViewportSize),
    }).strict().optional(),
    deviceScaleFactor: z.number().positive().max(LIMITS.maxDeviceScaleFactor).optional(),
    theme: z.enum(["light", "dark", "no-preference"]).optional(),
    locale: z.string().regex(/^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$/).optional(),
    reducedMotion: z.enum(["reduce", "no-preference"]).optional(),
    allowedOrigins: z.array(z.string().min(1).max(512)).max(LIMITS.maxAllowedOrigins).optional(),
    evidenceContext: evidenceContext.optional(),
  }).strict(),
  "browser.session.request": z.object({
    sessionId: id,
    url: httpUrl,
    method: z.enum(["GET", "POST", "PUT", "PATCH", "DELETE"]).optional(),
    headers: z.record(z.string().min(1).max(128), z.string().max(8192)).optional(),
    json: z.unknown().optional(),
    timeoutMs: timeoutMs.optional(),
  }).strict(),
  "browser.session.close": z.object({ sessionId: id }).strict(),
  "browser.pages": z.object({ sessionId: id }).strict(),
  "browser.open": z.object({
    sessionId: id,
    url: httpUrl,
    waitUntil: waitUntil.optional(),
    timeoutMs: timeoutMs.optional(),
  }).strict(),
  "browser.page.close": z.object(pageParams).strict(),
  "browser.observe": z.object({
    ...pageParams,
    maxNodes: z.number().int().positive().max(LIMITS.maxNodes).optional(),
    maxAriaChars: z.number().int().nonnegative().max(LIMITS.maxAriaChars).optional(),
    includeScreenshot: z.boolean().optional(),
    label: label.optional(),
    rootSelector: cssSelector.optional(),
    detail: z.enum(["summary", "full"]).optional(),
    // Keep only nodes inside the viewport and inside the visible area of every clipping
    // (overflow) ancestor, such as rows of a scrolled grid.
    visibleOnly: z.boolean().optional(),
  }).strict(),
  "browser.act": z.object({
    ...pageParams,
    expectedSnapshotId: id.optional(),
    action: ACTION_SCHEMA,
    settle: z.boolean().optional(),
  }).strict(),
  "browser.wait": z.object({
    ...pageParams,
    condition: WAIT_CONDITION_SCHEMA,
    timeoutMs: timeoutMs.optional(),
  }).strict(),
  "browser.capture": z.object({ ...pageParams, label: label.optional() }).strict(),
  "browser.logs": z.object({
    ...pageParams,
    since: cursor.optional(),
    levels: z.array(z.enum(LOG_LEVELS)).min(1).max(LOG_LEVELS.length).optional(),
    kinds: z.array(z.enum(LOG_KINDS)).min(1).max(LOG_KINDS.length).optional(),
    limit: z.number().int().positive().max(LIMITS.maxLogEntries).optional(),
  }).strict(),
  "browser.network": z.object({
    ...pageParams,
    since: cursor.optional(),
    urlContains: z.string().min(1).max(512).optional(),
    methods: z.array(z.string().regex(/^[A-Z]{1,16}$/)).min(1).max(16).optional(),
    resourceTypes: z.array(z.string().regex(/^[a-z]{1,32}$/)).min(1).max(16).optional(),
    failedOnly: z.boolean().optional(),
    limit: z.number().int().positive().max(LIMITS.maxNetworkEntries).optional(),
  }).strict(),
  "browser.inspect": z.object({
    ...pageParams,
    // Exactly one of selector (any element, including <html>) or target (a node of the latest
    // snapshot, which needs expectedSnapshotId).
    selector: cssSelector.optional(),
    target: nodeTarget.optional(),
    expectedSnapshotId: id.optional(),
    fields: z.array(inspectField).min(1).max(LIMITS.maxInspectFields),
    maxElements: z.number().int().positive().max(LIMITS.maxInspectElements).optional(),
  }).strict(),
  "visual.compare": z.object({
    reference: evidenceRef,
    candidate: evidenceRef,
    label: label.optional(),
    policy: comparePolicy.optional(),
  }).strict(),
  "visual.compare.native": z.object({
    reference: evidenceRef,
    candidate: z.object({
      path: z.string().min(1).max(4096),
      viewport: z.object({ width: z.number().int().positive().max(LIMITS.maxViewportSize),
                           height: z.number().int().positive().max(LIMITS.maxViewportSize) }).strict(),
      deviceScaleFactor: z.number().positive().max(LIMITS.maxDeviceScaleFactor),
      theme: z.enum(["light", "dark"]),
    }).strict(),
    label: label.optional(),
    policy: comparePolicy.optional(),
  }).strict(),
});

export const METHODS = Object.freeze(Object.keys(PARAMS));

export function parseParams(method, params) {
  const schema = Object.hasOwn(PARAMS, method) ? PARAMS[method] : null;
  if (!schema) throw new BrowserApiError("METHOD_NOT_FOUND", `Unknown method: ${String(method).slice(0, 100)}`);
  const parsed = schema.safeParse(params ?? {});
  if (!parsed.success) {
    throw new BrowserApiError("INVALID_PARAMS", `Invalid parameters for ${method}`, {
      issues: parsed.error.issues.slice(0, 20).map((issue) => ({
        path: issue.path.join("."),
        message: issue.message,
      })),
    });
  }
  return parsed.data;
}

export function protocolSchema() {
  return {
    protocolVersion: PROTOCOL_VERSION,
    limits: LIMITS,
    methods: Object.fromEntries(
      Object.entries(PARAMS).map(([method, schema]) => [
        method,
        { params: z.toJSONSchema(schema, { io: "input", unrepresentable: "any" }) },
      ]),
    ),
  };
}
