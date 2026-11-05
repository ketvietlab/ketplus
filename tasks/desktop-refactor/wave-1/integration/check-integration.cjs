#!/usr/bin/env node
const fs = require("node:fs");
const path = require("node:path");
const { validate, resolveRepoPath } = require("./evidence-validator.cjs");

const result = validate();
if (!result.ok) {
  console.error("integration handoff check failed:");
  for (const error of result.errors) console.error(`- ${error.code} ${error.path}: ${error.message}`);
  process.exit(1);
}

for (const warning of result.warnings) console.warn(`warning: ${warning.code} ${warning.path}: ${warning.message}`);

const root = process.cwd();
const dir = path.join(root, "tasks/desktop-refactor/wave-1/integration");
const linkPattern = /\[[^\]]+\]\(([^)]+)\)/g;
const errors = [];
for (const name of fs.readdirSync(dir).filter((entry) => entry.endsWith(".md"))) {
  const file = path.join(dir, name);
  const text = fs.readFileSync(file, "utf8");
  let match;
  while ((match = linkPattern.exec(text)) !== null) {
    const target = match[1];
    if (/^[a-z]+:\/\//i.test(target) || target.startsWith("#")) continue;
    const cleanTarget = target.split("#")[0];
    if (!cleanTarget) continue;
    const resolved = path.resolve(path.dirname(file), cleanTarget);
    const relative = path.relative(root, resolved);
    resolveRepoPath(root, relative, `link ${path.relative(root, file)} -> ${target}`, errors);
  }
}
if (errors.length) {
  console.error("integration handoff link check failed:");
  for (const error of errors) console.error(`- ${error.path}: ${error.message}`);
  process.exit(1);
}

console.log("Integration handoff PASS: manifest schema, atlas-derived inventory, asset hashes, evidence status, readiness, and links are valid.");
console.log(`Visual readiness: ${result.readiness.visualReference}; native pixel readiness: ${result.readiness.nativePixel}.`);
