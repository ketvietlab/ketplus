import assert from "node:assert/strict";
import { test } from "node:test";
import { deleteSessionCopies } from "../session-delete.mjs";

test("removes every SDK copy and treats an absent id as idempotent", async () => {
  let count = 0;
  await deleteSessionCopies(async (id) => {
    assert.equal(id, "session-1");
    if (++count === 4) throw new Error("Session session-1 not found");
  }, "session-1");
  assert.equal(count, 4);
  await deleteSessionCopies(async () => { throw Object.assign(new Error("gone"), { code: "ENOENT" }); }, "session-1");
});
test("SDK and permission failures stay failures so the daemon can retry", async () => {
  await assert.rejects(deleteSessionCopies(null, "session-1"), /cannot delete/);
  await assert.rejects(deleteSessionCopies(async () => { throw new Error("permission denied"); }, "session-1"), /permission denied/);
  await assert.rejects(deleteSessionCopies(async () => {}, "session-1"), /more copies/);
});
