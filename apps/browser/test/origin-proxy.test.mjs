import assert from "node:assert/strict";
import http from "node:http";
import net from "node:net";
import { once } from "node:events";
import { setTimeout as delay } from "node:timers/promises";
import test from "node:test";
import { startOriginProxy } from "../src/origin-proxy.mjs";

for (const method of ["CONNECT", "upgrade"]) {
  test(`a reset after denied ${method} leaves the proxy available`, { timeout: 5000 }, async (t) => {
    const proxy = await startOriginProxy({ allowsUrl: () => false, allowsAuthority: () => false });
    t.after(() => proxy.close());
    const url = new URL(proxy.server);
    const socket = net.connect(Number(url.port), url.hostname);
    t.after(() => socket.destroy());
    await once(socket, "connect");
    const response = once(socket, "data");
    socket.write(method === "CONNECT"
      ? "CONNECT denied.test:443 HTTP/1.1\r\nHost: denied.test:443\r\n\r\n"
      : "GET http://denied.test/ HTTP/1.1\r\nHost: denied.test\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    assert.match(String((await response)[0]), /403 Forbidden/);
    socket.resetAndDestroy();
    await delay(50); // let the peer receive the RST before verifying the worker survived
    const status = await new Promise((resolve, reject) => {
      http.get(proxy.server, { path: "http://denied.test/" }, (res) => {
        res.resume();
        res.on("end", () => resolve(res.statusCode));
      }).on("error", reject);
    });
    assert.equal(status, 403);
  });
}
