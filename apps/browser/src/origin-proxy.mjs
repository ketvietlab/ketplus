import http from "node:http";
import net from "node:net";
import { resolveAddresses } from "./network-address.mjs";

// Every browser-context network hop, including redirects, subframes, workers,
// WebSockets and the session request context, is forced through this proxy.
// Playwright route handlers do not see redirect hops, so they cannot enforce
// an origin boundary on their own. Each hop resolves its host once, keeps only
// the addresses the policy permits and connects to one of them, so a name that
// later resolves elsewhere (DNS rebinding) cannot move the connection. The proxy needs
// no credentials: it grants no network access a local process lacks, and
// Playwright disables the HTTP cache for contexts with proxy credentials.

const HOP_BY_HOP = new Set([
  "connection",
  "keep-alive",
  "proxy-authenticate",
  "proxy-authorization",
  "proxy-connection",
  "te",
  "trailer",
  "transfer-encoding",
  "upgrade",
]);

function forwardHeaders(rawHeaders, host) {
  const headers = [];
  for (let index = 0; index < rawHeaders.length; index += 2) {
    const name = rawHeaders[index];
    if (HOP_BY_HOP.has(name.toLowerCase()) || name.toLowerCase() === "host") continue;
    headers.push(name, rawHeaders[index + 1]);
  }
  headers.push("Host", host);
  return headers;
}

function parseAbsoluteHttpUrl(value) {
  try {
    const url = new URL(value);
    return url.protocol === "http:" ? url : null;
  } catch {
    return null;
  }
}

function parseAuthority(authority) {
  try {
    const url = new URL(`https://${authority}`);
    return url.host === authority || `${url.host}:443` === authority ? url : null;
  } catch {
    return null;
  }
}

async function permittedAddresses(hostname, permits, lookup) {
  let addresses;
  try {
    addresses = await resolveAddresses(hostname, lookup);
  } catch {
    return { unresolved: true, addresses: [] };
  }
  return { addresses: addresses.filter(permits) };
}

// Dials only the checked addresses, in resolver order, falling back to the next on
// failure (localhost answers ::1 before 127.0.0.1 for an IPv4-only server). The name is
// never resolved again.
function pinnedConnection(addresses) {
  const answers = addresses.map((address) => ({ address, family: net.isIP(address) }));
  return {
    autoSelectFamily: true,
    lookup(_hostname, options, callback) {
      if (options?.all) callback(null, answers);
      else callback(null, answers[0].address, answers[0].family);
    },
  };
}

export async function startOriginProxy({
  allowsUrl,
  allowsAuthority,
  allowsAddress = () => false,
  allowsAuthorityAddress = () => false,
  onBlocked = () => {},
  lookup = undefined,
}) {
  const refuseSocket = (socket, status) => {
    socket.end(
      status === 502
        ? "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n"
        : "HTTP/1.1 403 Forbidden\r\n\r\n",
    );
  };
  const sockets = new Set();
  const server = http.createServer(async (request, response) => {
    const url = parseAbsoluteHttpUrl(request.url);
    const deny = () => {
      onBlocked(url?.href ?? String(request.url));
      response.writeHead(403, { "content-type": "text/plain; charset=utf-8" });
      response.end("Blocked by browser session origin policy");
      request.resume();
    };
    if (!url || !allowsUrl(url.href)) {
      deny();
      return;
    }
    const target = await permittedAddresses(
      url.hostname,
      (address) => allowsAddress(url.href, address),
      lookup,
    );
    if (!target.addresses.length) {
      if (target.unresolved) {
        response.writeHead(502);
        response.end();
        request.resume();
      } else deny();
      return;
    }
    const upstream = http.request({
      ...pinnedConnection(target.addresses),
      // A pooled socket keyed by host:port could carry another session's pinned address.
      agent: false,
      host: url.hostname.replace(/^\[|\]$/g, ""),
      port: url.port || 80,
      method: request.method,
      path: `${url.pathname}${url.search}`,
      headers: forwardHeaders(request.rawHeaders, url.host),
    });
    upstream.on("response", (upstreamResponse) => {
      const headers = [];
      for (let index = 0; index < upstreamResponse.rawHeaders.length; index += 2) {
        const name = upstreamResponse.rawHeaders[index];
        if (HOP_BY_HOP.has(name.toLowerCase())) continue;
        headers.push(name, upstreamResponse.rawHeaders[index + 1]);
      }
      response.writeHead(upstreamResponse.statusCode, upstreamResponse.statusMessage, headers);
      upstreamResponse.on("error", () => response.destroy());
      upstreamResponse.pipe(response);
    });
    upstream.on("error", () => {
      if (!response.headersSent) response.writeHead(502);
      response.end();
    });
    request.on("error", () => upstream.destroy());
    response.on("close", () => upstream.destroy());
    request.pipe(upstream);
  });

  server.on("connect", async (request, clientSocket, head) => {
    const target = parseAuthority(request.url);
    if (!target || !allowsAuthority(request.url)) {
      onBlocked(target ? target.origin : String(request.url));
      refuseSocket(clientSocket, 403);
      return;
    }
    const resolved = await permittedAddresses(
      target.hostname,
      (address) => allowsAuthorityAddress(request.url, address),
      lookup,
    );
    if (!resolved.addresses.length) {
      if (!resolved.unresolved) onBlocked(target.origin);
      refuseSocket(clientSocket, resolved.unresolved ? 502 : 403);
      return;
    }
    const port = Number(request.url.slice(request.url.lastIndexOf(":") + 1)) || 443;
    const upstream = net.connect({
      ...pinnedConnection(resolved.addresses),
      host: target.hostname.replace(/^\[|\]$/g, ""),
      port,
    }, () => {
      clientSocket.write("HTTP/1.1 200 Connection Established\r\n\r\n");
      if (head.length) upstream.write(head);
      upstream.pipe(clientSocket);
      clientSocket.pipe(upstream);
    });
    upstream.on("error", () => clientSocket.destroy());
    clientSocket.on("error", () => upstream.destroy());
    clientSocket.on("close", () => upstream.destroy());
  });

  server.on("upgrade", async (request, clientSocket, head) => {
    const url = parseAbsoluteHttpUrl(request.url);
    if (!url || !allowsUrl(url.href)) {
      onBlocked(url?.href ?? String(request.url));
      refuseSocket(clientSocket, 403);
      return;
    }
    const resolved = await permittedAddresses(
      url.hostname,
      (address) => allowsAddress(url.href, address),
      lookup,
    );
    if (!resolved.addresses.length) {
      if (!resolved.unresolved) onBlocked(url.href);
      refuseSocket(clientSocket, resolved.unresolved ? 502 : 403);
      return;
    }
    const upstream = net.connect({
      ...pinnedConnection(resolved.addresses),
      host: url.hostname.replace(/^\[|\]$/g, ""),
      port: Number(url.port || 80),
    }, () => {
      const lines = [`${request.method} ${url.pathname}${url.search} HTTP/1.1`];
      for (let index = 0; index < request.rawHeaders.length; index += 2) {
        const name = request.rawHeaders[index];
        const lower = name.toLowerCase();
        if (lower === "host" || lower.startsWith("proxy-")) continue;
        lines.push(`${name}: ${request.rawHeaders[index + 1]}`);
      }
      lines.push(`Host: ${url.host}`);
      upstream.write(`${lines.join("\r\n")}\r\n\r\n`);
      if (head.length) upstream.write(head);
      upstream.pipe(clientSocket);
      clientSocket.pipe(upstream);
    });
    upstream.on("error", () => clientSocket.destroy());
    clientSocket.on("error", () => upstream.destroy());
    clientSocket.on("close", () => upstream.destroy());
  });

  server.on("connection", (socket) => {
    // Install before CONNECT/upgrade policy checks, including rejected connections.
    // A browser can reset a socket after receiving 403; unhandled errors kill the worker.
    socket.on("error", () => socket.destroy());
    sockets.add(socket);
    socket.on("close", () => sockets.delete(socket));
  });
  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  const { port } = server.address();
  return {
    server: `http://127.0.0.1:${port}`,
    async close() {
      for (const socket of sockets) socket.destroy();
      await new Promise((resolve) => server.close(() => resolve()));
    },
  };
}
