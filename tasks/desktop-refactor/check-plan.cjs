#!/usr/bin/env node
"use strict";

// Documentation inventory only. This is not a visual, API or native test runner.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { createHash } = require("node:crypto");

const root = path.resolve(__dirname, "../..");
const readJSON = (file) => JSON.parse(fs.readFileSync(file, "utf8"));
const hash = (bytes) => createHash("sha256").update(bytes).digest("hex");
const coverage = readJSON(path.join(__dirname, "coverage.json"));
const manifestPath = path.resolve(__dirname, coverage.manifest);
const atlas = readJSON(manifestPath);
const bundle = path.dirname(manifestPath);
const progress = readJSON(path.join(bundle, "atlas.progress.json"));
const workPackages = fs.readFileSync(path.join(__dirname, "03-work-packages-and-agents.md"), "utf8");
const taskRows = [...workPackages.matchAll(/^\| (R\d{2}) \|/gm)].map((m) => m[1]);
const taskIDs = new Set(taskRows);
assert.equal(taskRows.length, 15, "Duplicate/missing work-package row");
assert.equal(taskIDs.size, 15, "R00–R14 must be defined exactly once in the task table");
assert.equal(coverage.version, 1);
assert.equal(hash(fs.readFileSync(manifestPath)), coverage.manifestSha256, "Design changed: reconcile coverage and visual reference; never auto-bless");
assert.deepEqual(coverage.screens.map((s) => s.id).sort(), atlas.screens.map((s) => s.id).sort());

let stateCount = 0;
let mappedNodes = 0;
const stateIDs = new Set();
for (const screen of atlas.screens) {
    const planned = coverage.screens.find((s) => s.id === screen.id);
    assert(planned.tasks.length > 0, `Missing owner tasks: ${screen.id}`);
    for (const id of planned.tasks) assert(taskIDs.has(id), `Unknown task: ${id}`);
    const expected = new Map([[screen.url, []]]);
    for (const flow of atlas.flows) {
        for (const node of flow.nodes.filter((n) => n.screen === screen.id)) {
            const url = node.url || screen.url;
            if (!expected.has(url)) expected.set(url, []);
            expected.get(url).push(`${flow.id}/${node.id}`);
        }
    }
    assert.equal(planned.states.length, expected.size, `Missing/duplicate states: ${screen.id}`);
    const seenURLs = new Set();
    for (const state of planned.states) {
        assert(!stateIDs.has(state.id), `Duplicate fixture ID: ${state.id}`);
        stateIDs.add(state.id);
        assert(!seenURLs.has(state.url), `Duplicate state URL: ${state.url}`);
        seenURLs.add(state.url);
        assert(expected.has(state.url), `Unexpected state: ${state.url}`);
        assert.deepEqual([...state.nodes].sort(), [...expected.get(state.url)].sort(), `Node coverage mismatch: ${state.id}`);
        const local = path.resolve(bundle, state.url.split("?")[0]);
        assert(local.startsWith(bundle + path.sep), "State escapes bundle");
        assert(fs.existsSync(local), `Missing screen: ${state.url}`);
        mappedNodes += state.nodes.length;
        stateCount += 1;
    }
    const record = progress.screens[screen.id];
    assert(record, `Missing progress: ${screen.id}`);
    for (const id of planned.tasks) assert(record.tasks?.includes(id), `Missing progress task ${screen.id}/${id}`);
    for (const checkID of ["feature-parity", "pixel-perfect", "performance"]) {
        const check = record.checks?.find((c) => c.id === checkID);
        assert(check, `Missing native acceptance check ${screen.id}/${checkID}`);
        const refs = (check.nodes || []).map((n) => `${n.flowId}/${n.nodeId}`).sort();
        assert.deepEqual(refs, [...expected.values()].flat().sort(), `Incomplete acceptance state scope ${screen.id}/${checkID}`);
    }
}

const totals = {
    screens: atlas.screens.length,
    flows: atlas.flows.length,
    states: stateCount,
    nodes: atlas.flows.reduce((n, f) => n + f.nodes.length, 0),
    edges: atlas.flows.reduce((n, f) => n + f.edges.length, 0),
};
assert.deepEqual(totals, coverage.totals);
assert.equal(mappedNodes, atlas.flows.reduce((n, f) => n + f.nodes.filter((node) => node.screen).length, 0));

const lock = readJSON(path.join(bundle, "design-system.lock.json"));
for (const asset of lock.files) assert.equal(hash(fs.readFileSync(path.join(bundle, asset.path))), asset.sha256, `DS asset drift: ${asset.path}`);
for (const file of fs.readdirSync(__dirname).filter((f) => f.endsWith(".md"))) {
    const text = fs.readFileSync(path.join(__dirname, file), "utf8");
    for (const match of text.matchAll(/\[[^\]]+\]\(([^)]+)\)/g)) {
        if (/^https?:\/\//.test(match[1])) continue;
        const target = path.resolve(__dirname, match[1].split("#")[0]);
        assert(target.startsWith(root + path.sep), `Link outside repository: ${match[1]}`);
        assert(fs.existsSync(target), `Broken link in ${file}: ${match[1]}`);
    }
}
console.log(`Plan inventory PASS: ${totals.screens} screens, ${totals.flows} flows, ${totals.states} states, ${totals.nodes} nodes, ${totals.edges} edges; 15 task definitions; DS hashes, local links and native check scope valid.`);
console.log("No native/API/pixel/performance implementation was tested by this checker.");
