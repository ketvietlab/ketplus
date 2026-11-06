import assert from "node:assert/strict";
import http from "node:http";
import net from "node:net";
import { once } from "node:events";
import { setTimeout as delay } from "node:timers/promises";
import test from "node:test";
import { addressKind } from "../src/network-address.mjs";
import { OriginPolicy } from "../src/origin-policy.mjs";
import { startOriginProxy } from "../src/origin-proxy.mjs";

function proxyGet(proxy, target) {
  return new Promise((resolve, reject) => {
    http
      .get(
        proxy.server,
        { path: target },
        (res) => {
          let body = "";
          res.setEncoding("utf8");
          res.on("data", (chunk) => (body += chunk));
          res.on("end", () => resolve({ status: res.statusCode, headers: res.headers, body }));
        },
      )
      .on("error", reject);
  });
}

async function rawProxyRequest(t, proxy, text) {
  const url = new URL(proxy.server);
  const socket = net.connect(Number(url.port), url.hostname);
  t.after(() => socket.destroy());
  await once(socket, "connect");
  const response = once(socket, "data");
  socket.write(text);
  return { socket, text: String((await response)[0]) };
}

function policyProxy(policy, options = {}) {
  return startOriginProxy({
    allowsUrl: (url) => policy.allows(url),
    allowsAuthority: (authority) => policy.allowsAuthority(authority),
    allowsAddress: (url, address) => policy.allowsAddress(url, address),
    allowsAuthorityAddress: (authority, address) => policy.allowsAuthorityAddress(authority, address),
    ...options,
  });
}

for (const method of ["CONNECT", "upgrade"]) {
  test(`a reset after denied ${method} leaves the proxy available`, { timeout: 5000 }, async (t) => {
    const proxy = await startOriginProxy({ allowsUrl: () => false, allowsAuthority: () => false });
    t.after(() => proxy.close());
    const { socket, text } = await rawProxyRequest(
      t,
      proxy,
      method === "CONNECT"
        ? `CONNECT denied.test:443 HTTP/1.1\r\nHost: denied.test:443\r\n\r\n`
        : `GET http://denied.test/ HTTP/1.1\r\nHost: denied.test\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n`,
    );
    assert.match(text, /403 Forbidden/);
    socket.resetAndDestroy();
    await delay(50); // let the peer receive the RST before verifying the worker survived
    assert.equal((await proxyGet(proxy, "http://denied.test/")).status, 403);
  });
}

test("all websites never reach the local network, metadata or a rebound loopback", { timeout: 5000 }, async (t) => {
  const blocked = [];
  const answers = {
    "metadata.test": ["169.254.169.254"],
    "intranet.test": ["10.1.2.3"],
    "rebind.test": ["127.0.0.1"],
  };
  const proxy = await policyProxy(new OriginPolicy(["*"]), {
    onBlocked: (url) => blocked.push(url),
    lookup: async (host) => (answers[host] ?? []).map((address) => ({ address, family: 4 })),
  });
  t.after(() => proxy.close());
  for (const target of [
    "http://metadata.test/latest/meta-data/",
    "http://intranet.test/",
    "http://rebind.test/",
    "http://169.254.169.254/",
    "http://192.168.1.1/",
    "http://[::ffff:10.0.0.1]/",
  ])
    assert.equal((await proxyGet(proxy, target)).status, 403, target);
  assert.equal(blocked.length, 6);
  const { text } = await rawProxyRequest(
    t,
    proxy,
    `CONNECT metadata.test:443 HTTP/1.1\r\nHost: metadata.test:443\r\n\r\n`,
  );
  assert.match(text, /403 Forbidden/);
});

test("a hop connects only to an address the policy permits", { timeout: 5000 }, async (t) => {
  const server = http.createServer((request, response) => response.end(`host=${request.headers.host}`));
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  t.after(() => server.close());
  const { port } = server.address();
  const proxy = await policyProxy(new OriginPolicy(["loopback"]), {
    // The private answer comes first; the proxy must skip it, not try it.
    lookup: async () => [
      { address: "10.255.255.1", family: 4 },
      { address: "127.0.0.1", family: 4 },
    ],
  });
  t.after(() => proxy.close());
  const reply = await proxyGet(proxy, `http://localhost:${port}/`);
  assert.equal(reply.status, 200);
  assert.equal(reply.body, `host=localhost:${port}`);

  // The checked address is the one dialled: a name the system resolver does not
  // know still reaches it, so the hop never resolves the name a second time.
  const pinned = await startOriginProxy({
    allowsUrl: () => true,
    allowsAuthority: () => true,
    allowsAddress: (_url, address) => address === "127.0.0.1",
    allowsAuthorityAddress: (_authority, address) => address === "127.0.0.1",
    lookup: async () => [{ address: "127.0.0.1", family: 4 }],
  });
  t.after(() => pinned.close());
  const viaPin = await proxyGet(pinned, `http://pinned.invalid:${port}/`);
  assert.equal(viaPin.status, 200);
  assert.equal(viaPin.body, `host=pinned.invalid:${port}`);
});

test("address kinds", () => {
  assert.equal(addressKind("8.8.8.8"), "public");
  assert.equal(addressKind("2606:4700::1111"), "public");
  assert.equal(addressKind("127.0.0.2"), "loopback");
  assert.equal(addressKind("::1"), "loopback");
  assert.equal(addressKind("10.0.0.1"), "private");
  assert.equal(addressKind("172.20.0.1"), "private");
  assert.equal(addressKind("192.168.0.1"), "private");
  assert.equal(addressKind("100.97.50.8"), "private");
  assert.equal(addressKind("fd00::1"), "private");
  assert.equal(addressKind("169.254.169.254"), "link-local");
  assert.equal(addressKind("fe80::1"), "link-local");
  assert.equal(addressKind("::ffff:169.254.169.254"), "link-local");
  assert.equal(addressKind("0.0.0.0"), "reserved");
  assert.equal(addressKind("224.0.0.1"), "reserved");
  assert.equal(addressKind("::"), "reserved");
  assert.equal(addressKind("not-an-ip"), "invalid");
});

test("resolved-address policy for each origin entry", () => {
  const all = new OriginPolicy(["*"]);
  assert.ok(all.allowsAddress("https://example.com/", "93.184.215.14"));
  assert.ok(all.allowsAddress("http://localhost:5173/", "127.0.0.1"));
  assert.ok(all.allowsAddress("http://127.0.0.1:5173/", "127.0.0.1"));
  assert.ok(!all.allowsAddress("https://example.com/", "127.0.0.1"));
  assert.ok(!all.allowsAddress("https://example.com/", "10.0.0.1"));
  assert.ok(!all.allowsAddress("http://169.254.169.254/", "169.254.169.254"));
  assert.ok(!all.allowsAuthorityAddress("intranet.test:443", "192.168.1.10"));

  const loopback = new OriginPolicy(["loopback"]);
  assert.ok(loopback.allowsAddress("http://localhost:3000/", "::1"));
  assert.ok(!loopback.allowsAddress("http://localhost:3000/", "10.0.0.1"));

  const named = new OriginPolicy(["https://staging.internal", "http://10.0.0.5:3000"]);
  assert.ok(named.allowsAddress("https://staging.internal/", "10.20.0.4"));
  assert.ok(named.allowsAddress("https://staging.internal/", "93.184.215.14"));
  assert.ok(!named.allowsAddress("https://staging.internal/", "169.254.169.254"));
  assert.ok(!named.allowsAddress("https://staging.internal/", "127.0.0.1"));
  assert.ok(named.allowsAddress("http://10.0.0.5:3000/", "10.0.0.5"));
  assert.ok(!named.allowsAddress("https://other.test/", "93.184.215.14"));
});

test("a narrowed session keeps the host's address ceiling", () => {
  const all = new OriginPolicy(["*"]);
  const metadata = all.narrow(["http://169.254.169.254"]);
  assert.ok(metadata.allows("http://169.254.169.254/latest/"));
  assert.ok(!metadata.allowsAddress("http://169.254.169.254/latest/", "169.254.169.254"));
  const intranet = all.narrow(["https://intranet.test"]);
  assert.ok(!intranet.allowsAddress("https://intranet.test/", "10.0.0.1"));
  assert.ok(intranet.allowsAddress("https://intranet.test/", "93.184.215.14"));
  assert.ok(!all.narrow().allowsAddress("https://example.com/", "10.0.0.1"));

  const loopback = new OriginPolicy(["loopback"]).narrow(["http://localhost:3000"]);
  assert.ok(!loopback.allowsAddress("http://localhost:3000/", "10.0.0.1"));
  assert.ok(loopback.allowsAddress("http://localhost:3000/", "127.0.0.1"));
});

test("a hop falls back to the next permitted address", { timeout: 5000 }, async (t) => {
  const server = http.createServer((_request, response) => response.end("ipv4"));
  server.listen(0, "127.0.0.1"); // IPv4 only, while localhost answers ::1 first
  await once(server, "listening");
  t.after(() => server.close());
  const { port } = server.address();
  const proxy = await policyProxy(new OriginPolicy(["loopback"]), {
    lookup: async () => [
      { address: "::1", family: 6 },
      { address: "127.0.0.1", family: 4 },
    ],
  });
  t.after(() => proxy.close());
  const reply = await proxyGet(proxy, `http://localhost:${port}/`);
  assert.equal(reply.status, 200);
  assert.equal(reply.body, "ipv4");
  const { text } = await rawProxyRequest(
    t,
    proxy,
    `CONNECT localhost:${port} HTTP/1.1\r\nHost: localhost:${port}\r\n\r\n`,
  );
  assert.match(text, /^HTTP\/1\.1 200 Connection Established/);
});
