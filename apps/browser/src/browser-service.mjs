import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { createHash, randomUUID } from "node:crypto";
import { createRequire } from "node:module";
import { chromium } from "playwright-core";
import { BrowserApiError, assertApi, toApiError } from "./errors.mjs";
import { defaultBrowserCacheDir, enginePreference, selectEngine } from "./engine.mjs";
import { OriginPolicy, safeUrl } from "./origin-policy.mjs";
import { startOriginProxy } from "./origin-proxy.mjs";
import { assertValidSelector, descriptorScript, disposeHandles, observePage } from "./observation.mjs";
import { PageDiagnostics } from "./page-diagnostics.mjs";
import { inspectHandle, inspectLocator, inspectionOptions } from "./inspection.mjs";
import { compareRaster, compareSnapshots } from "./comparator.mjs";
import { LIMITS, METHODS, PROTOCOL_VERSION, parseParams, protocolSchema } from "./protocol.mjs";

const require = createRequire(import.meta.url);
const { version: playwrightVersion } = require("playwright-core/package.json");

const digest = (bytes) => createHash("sha256").update(bytes).digest("hex");
const sanitizeLabel = (label) => String(label || "capture").replace(/[^a-zA-Z0-9_-]+/g, "-").slice(0, 60);
const clip = (value, maxLength) => String(value ?? "").slice(0, maxLength);
const pngSize = (bytes) => ({ width: bytes.readUInt32BE(16), height: bytes.readUInt32BE(20) });

const FORBIDDEN_REQUEST_HEADERS = new Set([
  "connection",
  "content-length",
  "cookie",
  "cookie2",
  "expect",
  "host",
  "keep-alive",
  "te",
  "trailer",
  "transfer-encoding",
  "upgrade",
]);

async function withTimeout(promise, timeoutMs, message) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new BrowserApiError("TIMEOUT", message)), timeoutMs);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

// An agent reads every observation it asks for, so the default reply names what it can act on
// and where, without computed styles, and stops adding nodes at a byte budget. Refs of nodes
// left out stay valid; `detail: "full"` returns them all.
function summarizeSnapshot(snapshot) {
  const nodes = [];
  let bytes = Buffer.byteLength(JSON.stringify({ ...snapshot, nodes: [] }));
  for (const { styles, clipped, ...node } of snapshot.nodes) {
    if (node.text.length > LIMITS.summaryNodeTextChars) {
      node.text = `${node.text.slice(0, LIMITS.summaryNodeTextChars)}…`;
    }
    const size = Buffer.byteLength(JSON.stringify(node)) + 1;
    if (bytes + size > LIMITS.summaryMaxBytes) break;
    bytes += size;
    nodes.push(node);
  }
  return {
    ...snapshot,
    detail: "summary",
    nodes,
    truncated: snapshot.truncated || nodes.length < snapshot.nodes.length,
  };
}

export class BrowserService {
  constructor({
    artifactsDir = path.join(os.tmpdir(), `ketplus-browser-${process.pid}`),
    launchOptions = {},
    allowedOrigins = ["loopback"],
    exposeArtifactPaths = true,
    retainArtifacts = false,
    nativeCaptureRoot = path.join(os.tmpdir(), `ketplus-browser-native-${process.getuid?.() ?? "user"}`),
    // { preference, cacheDir, allowDownload, probes }; defaults come from
    // KETPLUS_BROWSER_ENGINE, KETPLUS_BROWSER_CACHE and KETPLUS_BROWSER_DOWNLOAD.
    engine = {},
  } = {}) {
    this.artifactsDir = path.resolve(artifactsDir);
    this.nativeCaptureRoot = path.resolve(nativeCaptureRoot);
    this.engineOptions = {
      preference: engine.preference ?? process.env.KETPLUS_BROWSER_ENGINE,
      cacheDir: engine.cacheDir ?? (process.env.KETPLUS_BROWSER_CACHE || defaultBrowserCacheDir()),
      allowDownload: engine.allowDownload ?? process.env.KETPLUS_BROWSER_DOWNLOAD !== "0",
      probes: engine.probes,
    };
    this.engine = null;
    this.launchOptions = {
      headless: true,
      ...launchOptions,
      // WebRTC UDP would bypass the per-session origin proxy.
      args: [...(launchOptions.args ?? []), "--force-webrtc-ip-handling-policy=disable_non_proxied_udp"],
    };
    // The host ceiling. Sessions can only narrow it.
    this.hostPolicy = new OriginPolicy(allowedOrigins);
    this.exposeArtifactPaths = exposeArtifactPaths;
    this.retainArtifacts = retainArtifacts;
    this.browser = null;
    this.browserPromise = null;
    this.sessions = new Map();
    // Sessions still starting count against LIMITS.maxSessions too.
    this.startingSessions = 0;
    this.snapshots = new Map();
    this.captures = new Map();
    this.diffs = new Map();
    this.sequence = 0;
  }

  capabilities() {
    return {
      protocolVersion: PROTOCOL_VERSION,
      engine: {
        adapter: "playwright",
        browser: "chromium",
        playwrightVersion,
        // "auto" | "chrome" | "msedge" | "bundled" | "playwright" as requested by the host.
        preference: this.safePreference(),
        // Resolved at the first session: chrome/msedge (system install), bundled
        // (KetPlus-managed Chrome for Testing) or playwright (development cache).
        name: this.engine?.name ?? null,
        source: this.engine?.source ?? null,
        browserVersion: this.browser?.version() ?? null,
        isolatedProfile: true,
      },
      methods: METHODS,
      actions: ["click", "fill", "press", "hover", "check", "select", "scroll", "navigate", "history", "reload"],
      waits: ["load", "text", "url", "settled"],
      limits: LIMITS,
      hostPolicy: { allowedOrigins: this.hostPolicy.entries },
      observation: {
        semanticNodes: true,
        ariaSnapshot: true,
        geometry: true,
        computedStyles: true,
        screenshot: true,
        subframes: false,
        visibleOnly: true,
      },
      // Read-only page diagnostics; none of them runs caller-supplied script.
      diagnostics: {
        consoleMessages: true,
        pageErrors: true,
        unhandledRejections: true,
        network: true,
        networkBodies: false,
        inspect: true,
        navigationKind: true,
      },
      limitations: [
        "Chromium-family engines only (system Chrome or Edge, or KetPlus-managed Chrome for Testing)",
        "Node references are valid only for the latest snapshot of a page and are re-verified by tag, role and name before acting",
        "Subframe content is not included in semantic nodes",
        "Persistent authenticated profiles, downloads and service workers are disabled",
        "Session requests do not follow redirects or expose response bodies and headers",
        "Page network entries carry method, URL, status, encoded body size and timing, never headers or bodies",
        "Diagnostics cover the main frame's console and errors; worker and subframe consoles are not included",
        "Arbitrary JavaScript execution is not exposed",
      ],
    };
  }

  safePreference() {
    try {
      return enginePreference({ KETPLUS_BROWSER_ENGINE: this.engineOptions.preference });
    } catch {
      return "invalid";
    }
  }

  async launchEngine() {
    // Explicit launch options from an embedder win over engine selection.
    if (this.launchOptions.executablePath || this.launchOptions.channel) {
      this.engine = { name: this.launchOptions.channel ?? "custom", source: "launch-options", executablePath: null };
      return chromium.launch(this.launchOptions);
    }
    const engine = await selectEngine({
      ...this.engineOptions,
      preference: enginePreference({ KETPLUS_BROWSER_ENGINE: this.engineOptions.preference }),
      probes: this.engineOptions.probes ?? undefined,
    });
    // Playwright gives every launch a temporary user data dir; never a personal profile.
    const options = engine.executablePath ? { ...this.launchOptions, executablePath: engine.executablePath } : this.launchOptions;
    try {
      const browser = await chromium.launch(options);
      this.engine = engine;
      return browser;
    } catch (error) {
      if (engine.name === "playwright" && /Executable doesn't exist/.test(String(error?.message))) {
        throw new BrowserApiError("BROWSER_UNAVAILABLE", "Playwright Chromium is not installed; run `npm run install:browser` or use KETPLUS_BROWSER_ENGINE=auto");
      }
      throw error;
    }
  }

  async ensureBrowser() {
    if (!this.browserPromise) {
      this.browserPromise = this.launchEngine().then(
        (browser) => {
          this.browser = browser;
          browser.on("disconnected", () => {
            if (this.browser === browser) {
              this.browser = null;
              this.browserPromise = null;
            }
            for (const session of this.sessions.values()) {
              if (session.browser === browser) session.disconnected = true;
            }
          });
          return browser;
        },
        (error) => {
          this.browserPromise = null;
          throw toApiError(error, "BROWSER_LAUNCH_FAILED");
        },
      );
    }
    return this.browserPromise;
  }

  async createSession(options = {}) {
    assertApi(
      this.sessions.size + this.startingSessions < LIMITS.maxSessions,
      "LIMIT_REACHED",
      `At most ${LIMITS.maxSessions} browser sessions can be open; close one first`,
      { limit: LIMITS.maxSessions },
    );
    this.startingSessions += 1;
    try {
      return await this.startSession(options);
    } finally {
      this.startingSessions -= 1;
    }
  }

  async startSession({
    viewport = { width: 1440, height: 900 },
    deviceScaleFactor = 1,
    theme = "light",
    locale = "en-US",
    reducedMotion = "reduce",
    allowedOrigins,
    evidenceContext = {},
  } = {}) {
    const policy = this.hostPolicy.narrow(allowedOrigins);
    const browser = await this.ensureBrowser();
    const sessionId = `session-${randomUUID()}`;
    const session = {
      sessionId,
      createdAt: Date.now(),
      browser,
      policy,
      context: null,
      proxy: null,
      pages: new Map(),
      openingPages: 0,
      pageByHandle: new WeakMap(),
      blockedRequests: [],
      blockedRequestCount: 0,
      evidenceContext: structuredClone(evidenceContext),
      environment: {
        viewport: { width: viewport.width, height: viewport.height },
        deviceScaleFactor,
        theme,
        locale,
        reducedMotion,
        browserEngine: this.engine?.name ?? null,
        browserVersion: browser.version(),
      },
      disconnected: false,
    };
    session.proxy = await startOriginProxy({
      allowsUrl: (url) => policy.allows(url),
      allowsAuthority: (authority) => policy.allowsAuthority(authority),
      allowsAddress: (url, address) => policy.allowsAddress(url, address),
      allowsAuthorityAddress: (authority, address) =>
        policy.allowsAuthorityAddress(authority, address),
      onBlocked: (url) => {
        session.blockedRequestCount += 1;
        if (session.blockedRequests.length < 50) session.blockedRequests.push(safeUrl(url));
      },
    });
    try {
      session.context = await browser.newContext({
        viewport: session.environment.viewport,
        deviceScaleFactor,
        colorScheme: theme,
        locale,
        reducedMotion,
        // Loopback is proxied too, so the policy also covers local services.
        proxy: { server: session.proxy.server, bypass: "<-loopback>" },
        acceptDownloads: false,
        serviceWorkers: "block",
      });
    } catch (error) {
      await session.proxy.close();
      throw toApiError(error, "SESSION_CREATE_FAILED");
    }
    session.context.on("page", (page) => this.admitPage(session, page));
    this.sessions.set(sessionId, session);
    return {
      sessionId,
      environment: structuredClone(session.environment),
      allowedOrigins: policy.entries,
    };
  }

  getSession(sessionId) {
    const session = this.sessions.get(sessionId);
    assertApi(session, "SESSION_NOT_FOUND", `Unknown browser session: ${sessionId}`);
    assertApi(!session.disconnected, "BROWSER_DISCONNECTED", "The browser process for this session has exited");
    return session;
  }

  getPage(session, pageId) {
    const record = session.pages.get(pageId);
    assertApi(record && !record.page.isClosed(), "PAGE_NOT_FOUND", `Unknown browser page: ${pageId}`);
    return record;
  }

  // Adopts a page the session opened or a site opened as a popup, or closes it past the cap.
  admitPage(session, page) {
    const existing = session.pageByHandle.get(page);
    if (existing) return existing;
    if (session.pages.size >= LIMITS.maxPagesPerSession) {
      page.close().catch(() => {});
      return null;
    }
    return this.adoptPage(session, page);
  }

  adoptPage(session, page) {
    const existing = session.pageByHandle.get(page);
    if (existing) return existing;
    const record = {
      pageId: `page-${randomUUID()}`,
      page,
      documentRevision: 0,
      actionSequence: 0,
      lastSnapshotId: null,
      handles: new Map(),
      fingerprints: new Map(),
      queue: Promise.resolve(),
      openerPageId: null,
      dialogsDismissed: 0,
      // Listeners attach now, before the first navigation, so its console output is kept.
      diagnostics: new PageDiagnostics(page),
    };
    session.pageByHandle.set(page, record);
    session.pages.set(record.pageId, record);
    page.opener().then(
      (opener) => {
        record.openerPageId = opener ? session.pageByHandle.get(opener)?.pageId ?? null : null;
      },
      () => {},
    );
    page.on("framenavigated", (frame) => {
      if (frame !== page.mainFrame()) return;
      record.documentRevision += 1;
      this.invalidate(record);
    });
    // An open dialog blocks every later operation on the page.
    page.on("dialog", (dialog) => {
      record.dialogsDismissed += 1;
      dialog.dismiss().catch(() => {});
    });
    page.on("close", () => {
      this.invalidate(record);
      record.diagnostics.close().catch(() => {});
      if (session.pages.get(record.pageId) === record) session.pages.delete(record.pageId);
    });
    return record;
  }

  invalidate(record) {
    const handles = record.handles;
    record.lastSnapshotId = null;
    record.handles = new Map();
    record.fingerprints = new Map();
    disposeHandles(handles).catch(() => {});
  }

  // Serializes state-changing operations per page so fencing checks and the
  // invalidation they guard cannot interleave.
  withPage(sessionId, pageId, operation) {
    const session = this.getSession(sessionId);
    const record = this.getPage(session, pageId);
    const run = record.queue.then(() => {
      this.getPage(this.getSession(sessionId), pageId);
      return operation(session, record);
    });
    record.queue = run.then(() => {}, () => {});
    return run;
  }

  isPageAllowed(session, record) {
    const url = record.page.url();
    return url === "about:blank" || session.policy.allows(url);
  }

  assertPageAllowed(session, record) {
    if (!this.isPageAllowed(session, record)) {
      throw new BrowserApiError("NAVIGATION_DENIED", "The page is outside the session origin policy", {
        url: safeUrl(record.page.url()),
      });
    }
  }

  async request({ sessionId, url, method = "GET", headers = {}, json, timeoutMs = 15_000 }) {
    const session = this.getSession(sessionId);
    session.policy.require(url, "REQUEST_DENIED");
    for (const name of Object.keys(headers)) {
      const lower = name.toLowerCase();
      assertApi(
        !FORBIDDEN_REQUEST_HEADERS.has(lower) && !lower.startsWith("proxy-"),
        "INVALID_ARGUMENT",
        `Request header is not allowed: ${name}`,
      );
    }
    assertApi(
      json === undefined || !["GET", "DELETE"].includes(method),
      "INVALID_ARGUMENT",
      `${method} requests cannot include a JSON body`,
    );
    let response;
    try {
      response = await session.context.request.fetch(url, {
        method,
        headers,
        ...(json === undefined ? {} : { data: json }),
        timeout: timeoutMs,
        maxRedirects: 0,
        failOnStatusCode: false,
      });
    } catch (error) {
      throw toApiError(error, "REQUEST_FAILED");
    }
    try {
      const status = response.status();
      return {
        outcome: response.ok() ? "accepted" : "rejected",
        sessionId,
        url: safeUrl(url),
        status,
        ok: response.ok(),
        redirected: status >= 300 && status < 400,
        contentType: response.headers()["content-type"]?.slice(0, 200) ?? null,
      };
    } finally {
      await response.dispose().catch(() => {});
    }
  }

  async open({ sessionId, url, waitUntil = "domcontentloaded", timeoutMs = 15_000 }) {
    const session = this.getSession(sessionId);
    session.policy.require(url);
    const pageLimit = () =>
      new BrowserApiError(
        "LIMIT_REACHED",
        `At most ${LIMITS.maxPagesPerSession} pages can be open in a session; close one first`,
        { limit: LIMITS.maxPagesPerSession },
      );
    // Pages still being created hold a slot, so concurrent opens never ask the engine for more
    // pages than the cap allows.
    if (session.pages.size + session.openingPages >= LIMITS.maxPagesPerSession) throw pageLimit();
    session.openingPages += 1;
    let page;
    try {
      page = await session.context.newPage();
    } catch (error) {
      throw toApiError(error, "PAGE_CREATE_FAILED");
    } finally {
      session.openingPages -= 1;
    }
    // Concurrent opens or popups may have taken the last slot meanwhile.
    const record = this.admitPage(session, page);
    if (!record) throw pageLimit();
    await record.diagnostics.ready;
    return this.withPage(sessionId, record.pageId, async () => {
      try {
        await page.goto(url, { waitUntil, timeout: timeoutMs });
      } catch (error) {
        await page.close().catch(() => {});
        throw toApiError(error, "NAVIGATION_FAILED");
      }
      if (!this.isPageAllowed(session, record)) {
        const finalUrl = safeUrl(page.url());
        await page.close().catch(() => {});
        throw new BrowserApiError("NAVIGATION_DENIED", "Navigation left the session origin policy", { url: finalUrl });
      }
      return {
        sessionId,
        pageId: record.pageId,
        url: safeUrl(page.url()),
        title: clip(await page.title().catch(() => ""), 300),
        documentRevision: record.documentRevision,
      };
    });
  }

  async pages({ sessionId }) {
    const session = this.getSession(sessionId);
    return {
      sessionId,
      pages: [...session.pages.values()].map((record) => ({
        pageId: record.pageId,
        url: safeUrl(record.page.url()),
        openerPageId: record.openerPageId,
        documentRevision: record.documentRevision,
        allowed: this.isPageAllowed(session, record),
      })),
      blockedRequestCount: session.blockedRequestCount,
      blockedRequests: [...session.blockedRequests],
    };
  }

  async closePage({ sessionId, pageId }) {
    return this.withPage(sessionId, pageId, async (session, record) => {
      await record.page.close().catch(() => {});
      session.pages.delete(pageId);
      return { outcome: "closed", sessionId, pageId };
    });
  }

  async observe({
    sessionId,
    pageId,
    maxNodes,
    maxAriaChars,
    includeScreenshot = false,
    label,
    rootSelector,
    detail = "summary",
    visibleOnly = false,
  }) {
    const full = detail === "full";
    maxNodes ??= full ? LIMITS.defaultMaxNodes : LIMITS.defaultSummaryNodes;
    maxAriaChars ??= full ? LIMITS.defaultMaxAriaChars : LIMITS.defaultSummaryAriaChars;
    return this.withPage(sessionId, pageId, async (session, record) => {
      this.assertPageAllowed(session, record);
      const revision = record.documentRevision;
      // A failed observation leaves no fresh snapshot behind.
      this.invalidate(record);
      const snapshotId = `snapshot-${++this.sequence}`;
      let observation;
      try {
        observation = await observePage(record.page, snapshotId, { maxNodes, maxAriaChars, rootSelector, visibleOnly });
      } catch (error) {
        throw toApiError(error, "OBSERVATION_FAILED");
      }
      if (record.documentRevision !== revision) {
        await disposeHandles(observation.handles);
        throw new BrowserApiError("STALE_SNAPSHOT", "The page navigated during observation", {
          documentRevision: record.documentRevision,
        });
      }
      record.handles = observation.handles;
      record.fingerprints = observation.fingerprints;
      record.lastSnapshotId = snapshotId;
      const snapshot = {
        snapshotId,
        sessionId,
        pageId,
        documentRevision: revision,
        page: {
          url: safeUrl(record.page.url()),
          title: clip(await record.page.title().catch(() => ""), 300),
        },
        environment: structuredClone(session.environment),
        evidenceContext: structuredClone(session.evidenceContext),
        rootSelector: rootSelector ?? "body",
        visibleOnly,
        ariaSnapshot: observation.ariaSnapshot,
        ariaSnapshotTruncated: observation.ariaSnapshotTruncated,
        ariaSnapshotSha256:
          observation.ariaSnapshotFull === null ? null : digest(Buffer.from(observation.ariaSnapshotFull)),
        nodes: observation.nodes,
        truncated: observation.truncated,
        visibleNodeCount: observation.visibleNodeCount,
        subframeCount: observation.subframeCount,
        logs: record.diagnostics.logSummary(),
      };
      record.diagnostics.markObserved();
      this.snapshots.set(snapshotId, {
        snapshot,
        ariaSnapshotFull: observation.ariaSnapshotFull,
        sessionId,
        pageId,
        documentRevision: revision,
        actionSequence: record.actionSequence,
      });
      if (includeScreenshot) {
        const capture = await this.captureLocked(session, record, { label: label || snapshotId, snapshotId });
        snapshot.captureId = capture.captureId;
        if (this.exposeArtifactPaths) snapshot.capturePath = capture.path;
      }
      // The stored snapshot stays whole for visual_compare; only the reply is summarized.
      return full ? snapshot : summarizeSnapshot(snapshot);
    });
  }

  requireFreshTarget(record, expectedSnapshotId, nodeRef) {
    assertApi(expectedSnapshotId, "INVALID_ARGUMENT", "expectedSnapshotId is required for node actions");
    assertApi(nodeRef, "INVALID_ARGUMENT", "action.target.nodeRef is required");
    if (record.lastSnapshotId !== expectedSnapshotId) {
      throw new BrowserApiError("STALE_SNAPSHOT", "The page has changed since that observation", {
        expectedSnapshotId,
        latestSnapshotId: record.lastSnapshotId,
      });
    }
    const handle = record.handles.get(nodeRef);
    assertApi(handle, "NODE_NOT_FOUND", `Unknown node reference: ${nodeRef}`);
    return handle;
  }

  // Element handles keep identity, not meaning. A recycled node (virtual list,
  // keyed re-render) must fail instead of receiving the action.
  async verifyFingerprint(handle, expected, nodeRef) {
    const current = await handle.evaluate(descriptorScript).catch(() => null);
    const actual = current && { tag: current.tag, role: current.role, name: current.name };
    if (
      !actual ||
      !expected ||
      actual.tag !== expected.tag ||
      actual.role !== expected.role ||
      actual.name !== expected.name
    ) {
      throw new BrowserApiError("NODE_CHANGED", "The target node no longer matches its observation", {
        nodeRef,
        expected,
        actual,
      });
    }
  }

  async settlePage(page, timeoutMs) {
    const frames = () =>
      page.evaluate(async () => {
        await document.fonts?.ready;
        await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
      });
    const run = async () => {
      try {
        await frames();
      } catch (error) {
        if (!/context was destroyed|navigat/i.test(String(error?.message))) throw error;
        await page.waitForLoadState("domcontentloaded", { timeout: timeoutMs });
        await frames();
      }
    };
    await withTimeout(run(), timeoutMs, "Page did not settle");
  }

  async act({ sessionId, pageId, expectedSnapshotId, action, settle = true }) {
    return this.withPage(sessionId, pageId, async (session, record) => {
      const page = record.page;
      const timeout = action.timeoutMs ?? 10_000;
      if (action.type === "navigate") session.policy.require(action.url);
      else this.assertPageAllowed(session, record);
      if (action.type === "select") {
        assertApi(action.value !== undefined || action.values !== undefined, "INVALID_ARGUMENT", "select requires value or values");
      }
      const nodeRef = action.target?.nodeRef;
      const pageAction = ["navigate", "history", "reload"].includes(action.type);
      const needsNode = !pageAction && (action.type !== "scroll" || nodeRef !== undefined);
      const handle = needsNode ? this.requireFreshTarget(record, expectedSnapshotId, nodeRef) : null;
      const fingerprint = needsNode ? record.fingerprints.get(nodeRef) : null;

      // Invalidate before acting: a failed or timed-out action may still have
      // changed the page, so its snapshot must never be reused.
      const handles = record.handles;
      record.handles = new Map();
      record.fingerprints = new Map();
      record.lastSnapshotId = null;
      record.actionSequence += 1;
      const pagesBefore = new Set(session.pages.keys());
      const before = record.diagnostics.marker();
      try {
        if (handle) await this.verifyFingerprint(handle, fingerprint, nodeRef);
        switch (action.type) {
          case "click":
            await handle.click({ timeout });
            break;
          case "fill":
            await handle.fill(action.value, { timeout });
            break;
          case "press":
            await handle.press(action.key, { timeout });
            break;
          case "hover":
            await handle.hover({ timeout });
            break;
          case "check":
            await handle.setChecked(action.checked ?? true, { timeout });
            break;
          case "select":
            await handle.selectOption(action.values ?? action.value, { timeout });
            break;
          case "scroll":
            if (handle) {
              await handle.hover({ timeout });
            } else {
              const { width, height } = session.environment.viewport;
              await page.mouse.move(width / 2, height / 2);
            }
            await page.mouse.wheel(action.deltaX ?? 0, action.deltaY ?? 0);
            break;
          case "navigate":
            await page.goto(action.url, { waitUntil: action.waitUntil ?? "domcontentloaded", timeout });
            break;
          case "history": {
            const options = { waitUntil: action.waitUntil ?? "domcontentloaded", timeout };
            // A same-document entry (pushState) resolves without a response and fires popstate.
            if (action.direction === "back") await page.goBack(options);
            else await page.goForward(options);
            break;
          }
          case "reload":
            if (action.ignoreCache) {
              await record.diagnostics.reloadIgnoringCache({ waitUntil: action.waitUntil ?? "domcontentloaded", timeout });
            } else {
              await page.reload({ waitUntil: action.waitUntil ?? "domcontentloaded", timeout });
            }
            break;
          default:
            throw new BrowserApiError("UNSUPPORTED_ACTION", `Unsupported action: ${action.type}`);
        }
        if (!this.isPageAllowed(session, record)) {
          throw new BrowserApiError("NAVIGATION_DENIED", "Navigation left the session origin policy", {
            url: safeUrl(page.url()),
          });
        }
        if (settle) await this.settlePage(page, timeout);
        await record.diagnostics.flush();
      } catch (error) {
        throw toApiError(error, "ACTION_FAILED");
      } finally {
        await disposeHandles(handles);
      }
      return {
        outcome: "applied",
        sessionId,
        pageId,
        url: safeUrl(page.url()),
        documentRevision: record.documentRevision,
        // What the action committed: a new document, a same-document change (pushState, history
        // traversal within the document, which fires popstate), or nothing.
        navigation: record.diagnostics.navigationSince(before),
        openedPages: [...session.pages.keys()].filter((id) => !pagesBefore.has(id)),
        dialogsDismissed: record.dialogsDismissed,
        logs: record.diagnostics.logSummary(),
        requiresObservation: true,
      };
    });
  }

  async wait({ sessionId, pageId, condition, timeoutMs = 10_000 }) {
    const session = this.getSession(sessionId);
    const record = this.getPage(session, pageId);
    const page = record.page;
    try {
      switch (condition.type) {
        case "load":
          await page.waitForLoadState(condition.state ?? "domcontentloaded", { timeout: timeoutMs });
          break;
        case "text":
          await page.getByText(condition.text, { exact: condition.exact ?? false }).first().waitFor({
            state: condition.state ?? "visible",
            timeout: timeoutMs,
          });
          break;
        case "url": {
          const { equals, startsWith } = condition;
          assertApi(
            (equals === undefined) !== (startsWith === undefined),
            "INVALID_ARGUMENT",
            "url condition requires exactly one of equals or startsWith",
          );
          const expected = equals === undefined ? null : new URL(equals).href;
          await page.waitForURL(
            (url) => (expected === null ? url.href.startsWith(startsWith) : url.href === expected),
            { timeout: timeoutMs, waitUntil: "commit" },
          );
          break;
        }
        case "settled":
          await this.settlePage(page, timeoutMs);
          break;
        default:
          throw new BrowserApiError("UNSUPPORTED_WAIT", `Unsupported wait condition: ${condition.type}`);
      }
    } catch (error) {
      throw toApiError(error, "WAIT_FAILED");
    }
    return {
      outcome: "satisfied",
      conditionType: condition.type,
      url: safeUrl(page.url()),
      documentRevision: record.documentRevision,
    };
  }

  // Diagnostics read buffers kept since the page opened; they never touch the snapshot.
  async logs({ sessionId, pageId, since, levels, kinds, limit }) {
    const session = this.getSession(sessionId);
    const record = this.getPage(session, pageId);
    this.assertPageAllowed(session, record);
    return { sessionId, pageId, ...record.diagnostics.queryLogs({ since, levels, kinds, limit }) };
  }

  async network({ sessionId, pageId, since, urlContains, methods, resourceTypes, failedOnly, limit }) {
    const session = this.getSession(sessionId);
    const record = this.getPage(session, pageId);
    this.assertPageAllowed(session, record);
    return {
      sessionId,
      pageId,
      ...record.diagnostics.queryNetwork({ since, urlContains, methods, resourceTypes, failedOnly, limit }),
    };
  }

  async inspect({ sessionId, pageId, selector, target, expectedSnapshotId, fields, maxElements = LIMITS.defaultInspectElements }) {
    assertApi(
      (selector === undefined) !== (target === undefined),
      "INVALID_ARGUMENT",
      "inspect requires exactly one of selector or target",
    );
    return this.withPage(sessionId, pageId, async (session, record) => {
      this.assertPageAllowed(session, record);
      const options = inspectionOptions(fields, maxElements);
      try {
        if (target) {
          // Reading leaves the snapshot valid, so its refs stay usable for the next action.
          const handle = this.requireFreshTarget(record, expectedSnapshotId, target.nodeRef);
          await this.verifyFingerprint(handle, record.fingerprints.get(target.nodeRef), target.nodeRef);
          return { sessionId, pageId, nodeRef: target.nodeRef, ...(await inspectHandle(handle, options)) };
        }
        await assertValidSelector(record.page, selector, "selector");
        return { sessionId, pageId, selector, ...(await inspectLocator(record.page.locator(`css=${selector}`), options)) };
      } catch (error) {
        throw toApiError(error, "INSPECTION_FAILED");
      }
    });
  }

  async capture({ sessionId, pageId, label = "capture" }) {
    return this.withPage(sessionId, pageId, async (session, record) => {
      this.assertPageAllowed(session, record);
      return this.captureLocked(session, record, { label, snapshotId: null });
    });
  }

  async captureLocked(session, record, { label, snapshotId }) {
    let bytes;
    try {
      bytes = await record.page.screenshot({ type: "png", animations: "disabled", caret: "hide" });
    } catch (error) {
      throw toApiError(error, "CAPTURE_FAILED");
    }
    const captureId = `capture-${++this.sequence}`;
    const directory = path.join(this.artifactsDir, session.sessionId);
    await fs.mkdir(directory, { recursive: true, mode: 0o700 });
    const filePath = path.join(directory, `${sanitizeLabel(label)}-${captureId}.png`);
    await fs.writeFile(filePath, bytes, { mode: 0o600 });
    const capture = {
      captureId,
      sessionId: session.sessionId,
      pageId: record.pageId,
      snapshotId,
      path: filePath,
      mediaType: "image/png",
      ...pngSize(bytes),
      sha256: digest(bytes),
      url: safeUrl(record.page.url()),
      documentRevision: record.documentRevision,
      actionSequence: record.actionSequence,
      environment: structuredClone(session.environment),
      evidenceContext: structuredClone(session.evidenceContext),
      recordedAt: new Date().toISOString(),
    };
    this.captures.set(captureId, capture);
    return this.publicCapture(capture);
  }

  publicCapture(capture) {
    const { path: filePath, ...rest } = capture;
    return this.exposeArtifactPaths ? { ...rest, path: filePath } : structuredClone(rest);
  }

  loadEvidence(side, reference) {
    const stored = this.snapshots.get(reference.snapshotId);
    const capture = this.captures.get(reference.captureId);
    assertApi(stored, "SNAPSHOT_NOT_FOUND", `Unknown snapshot: ${reference.snapshotId}`);
    assertApi(capture, "CAPTURE_NOT_FOUND", `Unknown capture: ${reference.captureId}`);
    assertApi(
      capture.sessionId === stored.sessionId &&
        capture.pageId === stored.pageId &&
        capture.documentRevision === stored.documentRevision &&
        capture.actionSequence === stored.actionSequence,
      "EVIDENCE_MISMATCH",
      `${side} capture was not taken from the same page state as its snapshot`,
      { snapshotId: reference.snapshotId, captureId: reference.captureId },
    );
    return {
      snapshot: { ...stored.snapshot, ariaSnapshotFull: stored.ariaSnapshotFull },
      capture,
    };
  }

  async compare({ reference, candidate, policy = {}, label = "comparison" }) {
    const referenceEvidence = this.loadEvidence("Reference", reference);
    const candidateEvidence = this.loadEvidence("Candidate", candidate);
    const diffId = `diff-${++this.sequence}`;
    const diffPath = path.join(this.artifactsDir, "comparisons", `${sanitizeLabel(label)}-${diffId}.png`);
    const result = await compareSnapshots({
      referenceSnapshot: referenceEvidence.snapshot,
      candidateSnapshot: candidateEvidence.snapshot,
      referenceCapture: referenceEvidence.capture,
      candidateCapture: candidateEvidence.capture,
      diffPath,
      policy,
    });
    if (result.raster) {
      this.diffs.set(diffId, {
        diffId,
        path: diffPath,
        sessionIds: new Set([referenceEvidence.capture.sessionId, candidateEvidence.capture.sessionId]),
      });
      result.raster.diffId = diffId;
      if (!this.exposeArtifactPaths) delete result.raster.diffPath;
    }
    return result;
  }

  async compareNative({ reference, candidate, policy = {}, label = "native-comparison" }) {
    const { capture } = this.loadEvidence("Reference", reference);
    const root = this.nativeCaptureRoot;
    const rootStat = await fs.lstat(root).catch(() => null);
    assertApi(rootStat?.isDirectory() && !rootStat.isSymbolicLink() &&
      (process.getuid?.() === undefined || rootStat.uid === process.getuid()) &&
      (rootStat.mode & 0o077) === 0,
    "INVALID_NATIVE_CAPTURE", "Native capture directory must be private and owned by this user");
    const rootPath = await fs.realpath(root);
    const filePath = await fs.realpath(candidate.path).catch(() => "");
    assertApi(filePath.startsWith(`${rootPath}${path.sep}`) && filePath.endsWith(".png"),
      "INVALID_NATIVE_CAPTURE", "Native capture must be a PNG inside the private capture directory");
    const fileStat = await fs.lstat(candidate.path).catch(() => null);
    assertApi(fileStat?.isFile() && !fileStat.isSymbolicLink() && fileStat.size <= 20_000_000 &&
      (process.getuid?.() === undefined || fileStat.uid === process.getuid()),
    "INVALID_NATIVE_CAPTURE", "Native capture must be a regular, user-owned PNG under 20 MB");
    const expected = capture.environment;
    if (JSON.stringify(expected.viewport) !== JSON.stringify(candidate.viewport) ||
        expected.deviceScaleFactor !== candidate.deviceScaleFactor ||
        expected.theme !== candidate.theme) {
      return { status: "blocked", mode: "raster-only", environmentMismatch: ["viewport/deviceScaleFactor/theme"],
        policy: { maxPixelMismatchRatio: policy.maxPixelMismatchRatio ?? 0.05 } };
    }
    const diffId = `diff-${++this.sequence}`;
    const diffPath = path.join(this.artifactsDir, "comparisons", `${sanitizeLabel(label)}-${diffId}.png`);
    const raster = await compareRaster(capture,
      { path: filePath, environment: { deviceScaleFactor: candidate.deviceScaleFactor } },
      diffPath, policy);
    this.diffs.set(diffId, { diffId, path: diffPath, sessionIds: new Set([capture.sessionId]) });
    raster.diffId = diffId;
    if (!this.exposeArtifactPaths) delete raster.diffPath;
    return { status: raster.mismatchRatio < (policy.maxPixelMismatchRatio ?? 0.05) ? "passed" : "failed",
      mode: "raster-only", policy: { pixelThreshold: policy.pixelThreshold ?? 0.1,
        maxPixelMismatchRatio: policy.maxPixelMismatchRatio ?? 0.05 }, raster };
  }

  async readArtifact({ captureId, diffId }) {
    const artifact = captureId ? this.captures.get(captureId) : this.diffs.get(diffId);
    assertApi(artifact, "ARTIFACT_NOT_FOUND", "Unknown artifact");
    const bytes = await fs.readFile(artifact.path);
    return { bytes, mediaType: "image/png", ...pngSize(bytes) };
  }

  async purgeSessionEvidence(sessionId) {
    for (const [snapshotId, stored] of this.snapshots) {
      if (stored.sessionId === sessionId) this.snapshots.delete(snapshotId);
    }
    for (const [captureId, capture] of this.captures) {
      if (capture.sessionId === sessionId) this.captures.delete(captureId);
    }
    for (const [diffId, diff] of this.diffs) {
      if (!diff.sessionIds.has(sessionId)) continue;
      this.diffs.delete(diffId);
      if (!this.retainArtifacts) await fs.rm(diff.path, { force: true }).catch(() => {});
    }
    if (!this.retainArtifacts) {
      await fs.rm(path.join(this.artifactsDir, sessionId), { recursive: true, force: true }).catch(() => {});
    }
  }

  async closeSession({ sessionId }) {
    const session = this.sessions.get(sessionId);
    assertApi(session, "SESSION_NOT_FOUND", `Unknown browser session: ${sessionId}`);
    this.sessions.delete(sessionId);
    for (const record of session.pages.values()) this.invalidate(record);
    await session.context?.close().catch(() => {});
    await session.proxy?.close().catch(() => {});
    await this.purgeSessionEvidence(sessionId);
    return { outcome: "closed", sessionId, blockedRequestCount: session.blockedRequestCount };
  }

  /** Closes every session this service opened, or only those created before `createdBefore` (ms). */
  async closeAllSessions({ createdBefore = Infinity } = {}) {
    const closed = [];
    for (const [sessionId, session] of [...this.sessions]) {
      if (session.createdAt >= createdBefore) continue;
      await this.closeSession({ sessionId }).catch(() => {});
      closed.push(sessionId);
    }
    return { outcome: "closed", sessionIds: closed };
  }

  async dispatch(method, params = {}) {
    const input = parseParams(method, params);
    switch (method) {
      case "browser.capabilities": return this.capabilities();
      case "browser.protocol.schema": return protocolSchema();
      case "browser.session.create": return this.createSession(input);
      case "browser.session.request": return this.request(input);
      case "browser.session.close": return this.closeSession(input);
      case "browser.pages": return this.pages(input);
      case "browser.open": return this.open(input);
      case "browser.page.close": return this.closePage(input);
      case "browser.observe": return this.observe(input);
      case "browser.act": return this.act(input);
      case "browser.wait": return this.wait(input);
      case "browser.capture": return this.capture(input);
      case "browser.logs": return this.logs(input);
      case "browser.network": return this.network(input);
      case "browser.inspect": return this.inspect(input);
      case "visual.compare": return this.compare(input);
      case "visual.compare.native": return this.compareNative(input);
      default: throw new BrowserApiError("METHOD_NOT_FOUND", `Unknown method: ${method}`);
    }
  }

  async close() {
    await this.closeAllSessions();
    const browser = this.browser ?? (this.browserPromise ? await this.browserPromise.catch(() => null) : null);
    this.browser = null;
    this.browserPromise = null;
    await browser?.close().catch(() => {});
    if (!this.retainArtifacts) {
      // Remove only directories this service created, and only when empty.
      await fs.rmdir(path.join(this.artifactsDir, "comparisons")).catch(() => {});
      await fs.rmdir(this.artifactsDir).catch(() => {});
    }
  }
}
