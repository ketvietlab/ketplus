import { BrowserApiError } from "./errors.mjs";
import { redactText, safeUrl } from "./redaction.mjs";
import { LIMITS } from "./protocol.mjs";

// Read-only diagnostics of one page: console messages, uncaught errors, failed requests and the
// page's own network traffic, plus whether a navigation replaced the document. Nothing here runs
// script in the page. Console and exception events come from Playwright and from a private CDP
// session; the CDP session is also what separates a same-document navigation (pushState,
// history traversal) from a document load, which Playwright's framenavigated does not.

const CONSOLE_LEVELS = new Map([
  ["error", "error"],
  ["assert", "error"],
  ["warning", "warning"],
  ["info", "info"],
  ["log", "log"],
  ["debug", "debug"],
  ["trace", "debug"],
]);

const location = (url, line, column) =>
  url ? { url: safeUrl(url), line: line + 1, column: column + 1 } : null;

const sameEntry = (left, right) =>
  left.kind === right.kind &&
  left.level === right.level &&
  left.text === right.text &&
  left.source?.url === right.source?.url &&
  left.source?.line === right.source?.line &&
  left.source?.column === right.source?.column;

function exceptionEntry(details) {
  const exception = details.exception ?? {};
  // "Uncaught (in promise)" is how Chromium reports an unhandled rejection.
  const kind = /in promise/i.test(details.text ?? "") ? "unhandledrejection" : "pageerror";
  const description = exception.description ?? (exception.value !== undefined ? String(exception.value) : "");
  const frames = details.stackTrace?.callFrames ?? [];
  const stack =
    exception.description && exception.className
      ? exception.description
      : frames
          .map((frame) => `    at ${frame.functionName || "<anonymous>"} (${frame.url}:${frame.lineNumber + 1}:${frame.columnNumber + 1})`)
          .join("\n");
  const firstFrame = frames[0];
  return {
    kind,
    level: "error",
    text: redactText(description.split("\n")[0] || details.text || kind, LIMITS.logTextChars),
    source: firstFrame
      ? location(firstFrame.url, firstFrame.lineNumber, firstFrame.columnNumber)
      : location(details.url, details.lineNumber ?? 0, details.columnNumber ?? 0),
    ...(stack ? { stack: redactText(stack, LIMITS.logStackChars) } : {}),
  };
}

export class PageDiagnostics {
  constructor(page) {
    this.page = page;
    this.cdp = null;
    this.logs = [];
    this.network = [];
    this.logSequence = 0;
    this.networkSequence = 0;
    this.logsDropped = 0;
    this.networkDropped = 0;
    this.documentLoads = 0;
    this.sameDocumentNavigations = 0;
    this.pendingRequests = 0;
    this.errorCount = 0;
    this.warningCount = 0;
    // Counts at the latest observation, so an action reports what appeared since then.
    this.observedCounts = { errors: 0, warnings: 0 };
    this.ready = this.attach();
  }

  async attach() {
    const page = this.page;
    page.on("console", (message) => {
      const { url, lineNumber, columnNumber } = message.location();
      this.addLog({
        kind: "console",
        level: CONSOLE_LEVELS.get(message.type()) ?? "log",
        text: redactText(message.text(), LIMITS.logTextChars),
        source: location(url, lineNumber, columnNumber),
      });
    });
    page.on("request", () => {
      this.pendingRequests += 1;
    });
    page.on("requestfinished", (request) => {
      this.finishRequest(request, null).catch(() => {});
    });
    page.on("requestfailed", (request) => {
      this.finishRequest(request, request.failure()?.errorText ?? "failed").catch(() => {});
    });
    try {
      this.cdp = await page.context().newCDPSession(page);
      this.cdp.on("Page.frameNavigated", ({ frame }) => {
        if (!frame.parentId) this.documentLoads += 1;
      });
      this.cdp.on("Page.navigatedWithinDocument", ({ frameId }) => {
        if (frameId === this.mainFrameId) this.sameDocumentNavigations += 1;
      });
      this.cdp.on("Runtime.exceptionThrown", ({ exceptionDetails }) => {
        this.addLog(exceptionEntry(exceptionDetails));
      });
      await this.cdp.send("Page.enable");
      const { frameTree } = await this.cdp.send("Page.getFrameTree");
      this.mainFrameId = frameTree.frame.id;
      await this.cdp.send("Runtime.enable");
    } catch {
      // Without CDP (or on a page closed while attaching) uncaught errors still arrive through
      // Playwright, without separating rejections from exceptions.
      await this.cdp?.detach().catch(() => {});
      this.cdp = null;
      page.on("pageerror", (error) => {
        this.addLog({
          kind: "pageerror",
          level: "error",
          text: redactText(String(error?.message ?? error), LIMITS.logTextChars),
          source: null,
          ...(error?.stack ? { stack: redactText(error.stack, LIMITS.logStackChars) } : {}),
        });
      });
    }
  }

  addLog(entry) {
    if (entry.level === "error") this.errorCount += 1;
    if (entry.level === "warning") this.warningCount += 1;
    const last = this.logs.at(-1);
    entry.time = new Date().toISOString();
    if (last && sameEntry(last, entry)) {
      // Repeats collapse into the latest entry, which moves past every cursor that saw it.
      last.count += 1;
      last.time = entry.time;
      last.seq = ++this.logSequence;
      return;
    }
    this.logs.push({ seq: ++this.logSequence, ...entry, count: 1 });
    if (this.logs.length > LIMITS.maxLogEntries) {
      this.logs.shift();
      this.logsDropped += 1;
    }
  }

  async finishRequest(request, failure) {
    this.pendingRequests = Math.max(0, this.pendingRequests - 1);
    const response = failure ? null : await request.response().catch(() => null);
    const sizes = failure ? null : await request.sizes().catch(() => null);
    const timing = request.timing();
    const url = safeUrl(request.url());
    const status = response?.status() ?? null;
    this.network.push({
      seq: ++this.networkSequence,
      method: request.method(),
      url,
      resourceType: request.resourceType(),
      status,
      ...(failure ? { failure: redactText(failure, 200) } : {}),
      responseBodyBytes: sizes?.responseBodySize ?? null,
      durationMs: timing.responseEnd >= 0 ? Math.round(timing.responseEnd) : null,
      startedAt: timing.startTime > 0 ? new Date(timing.startTime).toISOString() : null,
    });
    if (this.network.length > LIMITS.maxNetworkEntries) {
      this.network.shift();
      this.networkDropped += 1;
    }
    if (failure) {
      this.addLog({ kind: "requestfailed", level: "error", text: `${request.method()} ${url} ${redactText(failure, 200)}`, source: null });
    } else if (status !== null && status >= 400) {
      this.addLog({ kind: "http", level: "error", text: `${request.method()} ${url} ${status}`, source: null });
    }
  }

  marker() {
    return {
      logSequence: this.logSequence,
      documentLoads: this.documentLoads,
      sameDocumentNavigations: this.sameDocumentNavigations,
    };
  }

  // What the page did since `marker`: "full-load" when a new document was committed,
  // "same-document" for pushState, replaceState, fragment or history traversal within the
  // document (traversal fires popstate), otherwise "none".
  navigationSince(marker) {
    if (this.documentLoads > marker.documentLoads) return "full-load";
    if (this.sameDocumentNavigations > marker.sameDocumentNavigations) return "same-document";
    return "none";
  }

  markObserved() {
    this.observedCounts = { errors: this.errorCount, warnings: this.warningCount };
  }

  // Counts for observe and act results; entries themselves come from browser.logs.
  logSummary() {
    return {
      cursor: this.logSequence,
      errors: this.errorCount,
      errorsSinceObserve: this.errorCount - this.observedCounts.errors,
      warningsSinceObserve: this.warningCount - this.observedCounts.warnings,
    };
  }

  queryLogs({ since = 0, levels, kinds, limit = LIMITS.defaultLogEntries }) {
    const matching = this.logs.filter(
      (entry) =>
        entry.seq > since &&
        (!levels || levels.includes(entry.level)) &&
        (!kinds || kinds.includes(entry.kind)),
    );
    const entries = matching.slice(0, limit);
    return {
      entries: structuredClone(entries),
      cursor: entries.length < matching.length ? entries.at(-1).seq : this.logSequence,
      hasMore: entries.length < matching.length,
      // Entries newer than `since` that no longer fit the page's buffer.
      dropped: this.droppedSince(this.logs, since, this.logsDropped),
    };
  }

  queryNetwork({ since = 0, urlContains, methods, resourceTypes, failedOnly = false, limit = LIMITS.defaultNetworkEntries }) {
    const matching = this.network.filter(
      (entry) =>
        entry.seq > since &&
        (!urlContains || entry.url.includes(urlContains)) &&
        (!methods || methods.includes(entry.method)) &&
        (!resourceTypes || resourceTypes.includes(entry.resourceType)) &&
        (!failedOnly || entry.failure !== undefined || (entry.status ?? 0) >= 400),
    );
    const entries = matching.slice(0, limit);
    return {
      entries: structuredClone(entries),
      cursor: entries.length < matching.length ? entries.at(-1).seq : this.networkSequence,
      hasMore: entries.length < matching.length,
      pendingRequests: this.pendingRequests,
      dropped: this.droppedSince(this.network, since, this.networkDropped),
    };
  }

  droppedSince(buffer, since, dropped) {
    if (dropped === 0) return 0;
    const oldest = buffer[0]?.seq ?? Infinity;
    // Sequence numbers are contiguous except where repeats moved an entry forward, so this is an
    // upper bound; it is zero whenever the cursor is still inside the buffer.
    return since >= oldest - 1 ? 0 : Math.min(dropped, oldest - 1 - since);
  }

  // A reply on the private CDP session arrives after every event it sent before, so navigation
  // events of a finished action are counted before navigationSince reads them.
  async flush() {
    await this.cdp?.send("Page.getFrameTree").catch(() => {});
  }

  // Playwright's reload has no cache control; CDP's Page.reload does.
  async reloadIgnoringCache({ waitUntil, timeout }) {
    if (!this.cdp) throw new BrowserApiError("ACTION_FAILED", "The page cannot be reloaded without its cache");
    const page = this.page;
    const committed = page.waitForEvent("framenavigated", { predicate: (frame) => frame === page.mainFrame(), timeout });
    await this.cdp.send("Page.reload", { ignoreCache: true });
    await committed;
    await page.waitForLoadState(waitUntil, { timeout });
  }

  async close() {
    await this.cdp?.detach().catch(() => {});
    this.cdp = null;
  }
}
