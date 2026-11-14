// SPDX-License-Identifier: MPL-2.0
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

// Numeric container UIDs need not exist in /etc/passwd. Give the import probe its
// own private home, without inheriting build-host preloads, settings or credentials.
export function smokeRuntime(node, stage, work) {
  const home = path.join(work, "smoke-home");
  fs.mkdirSync(home, { mode: 0o700 });
  return execFileSync(
    node,
    ["--input-type=module", "-e", 'await import("./src/browser-service.mjs"); const { createRequire } = await import("node:module"); createRequire(process.cwd() + "/").resolve("playwright-core/lib/coreBundle"); console.log(process.version)'],
    { cwd: stage, env: { HOME: home, PATH: "/usr/bin:/bin" }, encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] },
  ).trim();
}
