import http from "node:http";
import net from "node:net";

// Every browser-context network hop, including redirects, subframes, workers,
// WebSockets and the session request context, is forced through this proxy.
// Playwright route handlers do not see redirect hops, so they cannot enforce
// an origin boundary on their own.

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

export async function startOriginProxy({ allowsUrl, allowsAuthority, onBlocked = () => {} }) {
  const sockets = new Set();
  const server = http.createServer((request, response) => {
    const url = parseAbsoluteHttpUrl(request.url);
    if (!url || !allowsUrl(url.href)) {
      onBlocked(url?.href ?? String(request.url));
      response.writeHead(403, { "content-type": "text/plain; charset=utf-8" });
      response.end("Blocked by browser session origin policy");
      request.resume();
      return;
    }
    const upstream = http.request({
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

  server.on("connect", (request, clientSocket, head) => {
    const target = parseAuthority(request.url);
    if (!target || !allowsAuthority(request.url)) {
      onBlocked(target ? target.origin : String(request.url));
      clientSocket.end("HTTP/1.1 403 Forbidden\r\n\r\n");
      return;
    }
    const port = Number(request.url.slice(request.url.lastIndexOf(":") + 1)) || 443;
    const upstream = net.connect(port, target.hostname.replace(/^\[|\]$/g, ""), () => {
      clientSocket.write("HTTP/1.1 200 Connection Established\r\n\r\n");
      if (head.length) upstream.write(head);
      upstream.pipe(clientSocket);
      clientSocket.pipe(upstream);
    });
    upstream.on("error", () => clientSocket.destroy());
    clientSocket.on("error", () => upstream.destroy());
    clientSocket.on("close", () => upstream.destroy());
  });

  server.on("upgrade", (request, clientSocket, head) => {
    const url = parseAbsoluteHttpUrl(request.url);
    if (!url || !allowsUrl(url.href)) {
      onBlocked(url?.href ?? String(request.url));
      clientSocket.end("HTTP/1.1 403 Forbidden\r\n\r\n");
      return;
    }
    const upstream = net.connect(Number(url.port || 80), url.hostname.replace(/^\[|\]$/g, ""), () => {
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
