import assert from "node:assert/strict";
import fs from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";
import { matchNodes } from "../src/comparator.mjs";
import { toApiError } from "../src/errors.mjs";
import { redactSecrets } from "../src/observation.mjs";
import { OriginPolicy, safeUrl } from "../src/origin-policy.mjs";
import { parseParams, protocolSchema } from "../src/protocol.mjs";

test("sources contain no control bytes that make Git treat them as binary", async () => {
  const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
  for (const directory of ["src", "test"]) {
    for (const name of await fs.readdir(path.join(appRoot, directory))) {
      if (!name.endsWith(".mjs")) continue;
      const text = await fs.readFile(path.join(appRoot, directory, name), "utf8");
      assert.equal(/[\x00-\x08\x0b\x0c\x0e-\x1f]/.test(text), false, `${directory}/${name}`);
    }
  }
});

test("repeated stable ids never pair many nodes to one candidate", () => {
  const node = (ref, name) => ({ ref, stableId: "row", role: "listitem", name });
  const { matched, missing, extra } = matchNodes(
    [node("r1", "A"), node("r2", "B")],
    [node("c1", "B"), node("c2", "A")],
  );
  assert.deepEqual(
    matched.map(({ reference, candidate, strategy }) => [reference.ref, candidate.ref, strategy]).sort(),
    [["r1", "c2", "role-name"], ["r2", "c1", "role-name"]],
  );
  assert.deepEqual([missing, extra], [[], []]);
});

test("origin policy normalizes entries, only narrows, and maps sockets to HTTP origins", () => {
  const host = new OriginPolicy(["loopback", "https://app.example/"]);
  assert.deepEqual(host.entries, ["loopback", "https://app.example"]);
  assert.ok(host.allows("http://127.0.0.2:8080/x"));
  assert.ok(host.allows("wss://app.example/socket"));
  assert.ok(host.allowsAuthority("app.example:443"));
  assert.equal(host.allows("https://user:pw@app.example/"), false);
  assert.equal(host.allows("file:///etc/passwd"), false);
  assert.equal(host.narrow(["https://app.example"]).allows("http://localhost/"), false);
  assert.throws(() => host.narrow(["*"]), (error) => error.code === "ORIGIN_NOT_PERMITTED");
  assert.throws(() => host.narrow(["https://other.example"]), (error) => error.code === "ORIGIN_NOT_PERMITTED");
  assert.throws(() => new OriginPolicy(["https://app.example/path"]), (error) => error.code === "INVALID_ARGUMENT");
});

test("redaction covers query secrets, opaque path tokens and engine call logs", () => {
  const url = safeUrl("https://a.example/reset/abcdefghijklmnopqrstuvwxyz0123456789?session=S1&sig=S2&page=2#frag");
  for (const secret of ["S1", "S2", "abcdefghijklmnop", "frag"]) assert.equal(url.includes(secret), false);
  assert.ok(url.includes("page=2"));

  const error = toApiError(
    new Error('page.goto: net::ERR_FAILED at http://x.test/?token=T0K3N\nCall log:\n  - navigating to "http://x.test/?token=T0K3N"'),
  );
  assert.equal(error.message.includes("T0K3N"), false);
  assert.equal(error.message.includes("Call log"), false);

  assert.equal(
    redactSecrets('- textbox "Password": hunter2\n- textbox "PIN": 42', ["hunter2", "42"]),
    '- textbox "Password": [redacted]\n- textbox "PIN": [redacted]',
  );
});

test("protocol rejects engine selectors, unknown fields and oversized inputs", () => {
  const page = { sessionId: "session", pageId: "page" };
  for (const rootSelector of ["text=Hello", "main >> button", "//main", "xpath=//main", "internal:role=button"]) {
    assert.throws(
      () => parseParams("browser.observe", { ...page, rootSelector }),
      (error) => error.code === "INVALID_PARAMS",
      rootSelector,
    );
  }
  assert.doesNotThrow(() => parseParams("browser.observe", { ...page, rootSelector: "main [data-testid='x']" }));
  assert.throws(() => parseParams("browser.observe", { ...page, extra: true }), (error) => error.code === "INVALID_PARAMS");
  assert.throws(
    () => parseParams("browser.session.create", { viewport: { width: 100_000, height: 10 } }),
    (error) => error.code === "INVALID_PARAMS",
  );
  assert.throws(() => parseParams("browser.unknown", {}), (error) => error.code === "METHOD_NOT_FOUND");
  assert.ok(protocolSchema().methods["browser.act"].params);
});
