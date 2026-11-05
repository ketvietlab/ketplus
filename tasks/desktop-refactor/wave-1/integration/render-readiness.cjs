#!/usr/bin/env node
const fs = require("node:fs");
const { validate } = require("./evidence-validator.cjs");
const { markdown } = require("./gate-policy.cjs");
const result = validate({ checkMatrix: false });
if (!result.ok) {
  console.error(JSON.stringify(result.errors, null, 2));
  process.exit(1);
}
fs.writeFileSync("tasks/desktop-refactor/wave-1/integration/task-readiness.md", markdown(result.taskReadiness));
