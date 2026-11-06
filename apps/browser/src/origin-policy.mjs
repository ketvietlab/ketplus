import net from "node:net";
import { BrowserApiError } from "./errors.mjs";
import { addressKind } from "./network-address.mjs";
import { safeUrl } from "./redaction.mjs";

export { redactText, safeUrl } from "./redaction.mjs";

const NETWORK_SCHEMES = new Map([
  ["http:", "http:"],
  ["https:", "https:"],
  ["ws:", "http:"],
  ["wss:", "https:"],
]);

export function isLoopbackHost(hostname) {
  const host = hostname.replace(/^\[|\]$/g, "");
  return host === "localhost" || host === "::1" || /^127(?:\.\d{1,3}){3}$/.test(host);
}

export function normalizeOriginEntry(entry) {
  if (entry === "*" || entry === "loopback") return entry;
  let url = null;
  try {
    url = new URL(entry);
  } catch {
    // Reported below.
  }
  if (
    !url ||
    !["http:", "https:"].includes(url.protocol) ||
    url.username ||
    url.password ||
    url.pathname !== "/" ||
    url.search ||
    url.hash
  ) {
    throw new BrowserApiError(
      "INVALID_ARGUMENT",
      `Allowed origin must be "loopback", "*", or an http(s) origin: ${safeUrl(entry)}`,
    );
  }
  return url.origin;
}

export function parseOriginList(value) {
  const entries = String(value ?? "")
    .split(",")
    .map((entry) => entry.trim())
    .filter(Boolean);
  return entries.length ? entries : undefined;
}

export class OriginPolicy {
  constructor(entries = undefined) {
    const list = entries?.length ? entries : ["loopback"];
    this.entries = [...new Set(list.map(normalizeOriginEntry))];
  }

  // Page URLs and network hops. WebSocket schemes are checked against the
  // matching HTTP origin.
  allows(value) {
    let url;
    try {
      url = new URL(value);
    } catch {
      return false;
    }
    const scheme = NETWORK_SCHEMES.get(url.protocol);
    if (!scheme || url.username || url.password) return false;
    const origin = new URL(`${scheme}//${url.host}`).origin;
    return this.entries.some((entry) => {
      if (entry === "*") return true;
      if (entry === "loopback") return isLoopbackHost(url.hostname);
      return entry === origin;
    });
  }

  // A CONNECT tunnel reveals only host:port, and Playwright tunnels plain HTTP
  // API requests too, so either scheme for that authority is acceptable.
  allowsAuthority(authority) {
    return this.allows(`https://${authority}`) || this.allows(`http://${authority}`);
  }

  // Whether an allowed URL may connect to `address`, one of its resolved addresses.
  // "*" means public websites and this computer: never the local network,
  // link-local (cloud metadata) or reserved ranges. A loopback address needs a
  // loopback name, so a public name cannot rebind to local services. A named
  // origin may reach the private network; an IP-literal origin reaches exactly itself.
  allowsAddress(value, address) {
    if (!this.allows(value)) return false;
    const url = new URL(value);
    const scheme = NETWORK_SCHEMES.get(url.protocol);
    const origin = new URL(`${scheme}//${url.host}`).origin;
    const host = url.hostname.replace(/^\[|\]$/g, "");
    const kind = addressKind(address);
    const loopbackName = isLoopbackHost(url.hostname);
    return this.entries.some((entry) => {
      if (entry === "*") return kind === "public" || (kind === "loopback" && loopbackName);
      if (entry === "loopback") return kind === "loopback" && loopbackName;
      if (entry !== origin) return false;
      if (net.isIP(host)) return true; // resolves to exactly itself
      if (kind === "loopback") return loopbackName;
      return kind === "public" || kind === "private";
    });
  }

  allowsAuthorityAddress(authority, address) {
    return (
      this.allowsAddress(`https://${authority}`, address) ||
      this.allowsAddress(`http://${authority}`, address)
    );
  }

  permits(entry) {
    if (this.entries.includes("*")) return true;
    if (entry === "*") return false;
    if (entry === "loopback") return this.entries.includes("loopback");
    return this.allows(entry);
  }

  // Sessions may only narrow the host policy. A model-controlled caller must
  // not be able to grant itself new origins.
  narrow(requested) {
    if (!requested?.length) return new OriginPolicy(this.entries);
    const normalized = requested.map(normalizeOriginEntry);
    const denied = normalized.filter((entry) => !this.permits(entry));
    if (denied.length) {
      throw new BrowserApiError(
        "ORIGIN_NOT_PERMITTED",
        "Requested origins exceed the host browser policy",
        { denied },
      );
    }
    return new OriginPolicy(normalized);
  }

  require(value, code = "NAVIGATION_DENIED") {
    if (!this.allows(value)) {
      throw new BrowserApiError(code, `URL is outside the session origin policy: ${safeUrl(value)}`);
    }
  }
}
