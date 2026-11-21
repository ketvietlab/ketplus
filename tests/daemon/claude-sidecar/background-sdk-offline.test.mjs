import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, readFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { backgroundOptions } from '../../../apps/daemon/providers/claude-sidecar/background-policy.mjs';
import { ProcessBarrier } from '../../../apps/daemon/providers/claude-sidecar/process-barrier.mjs';

// Explicit opt-in to a locally installed pinned SDK. No prompt is ever yielded,
// no inference is requested, and no SDK download/provider account is required.
const directory = process.env.KETPLUS_CLAUDE_SDK_DIRECTORY;
test('pinned SDK rejects background control even when project settings request native background',
  { skip: !directory, timeout: 20000 }, async () => {
    const metadata = JSON.parse(await readFile(path.join(directory, 'package.json'), 'utf8'));
    assert.equal(metadata.version, '0.3.322');
    const { query } = await import(pathToFileURL(path.join(directory, 'sdk.mjs')).href);
    const cwd = await mkdtemp(path.join(tmpdir(), 'kp-sdk-background-offline-'));
    await mkdir(path.join(cwd, '.claude'));
    await writeFile(path.join(cwd, '.claude/settings.json'), JSON.stringify({ env: {
      CLAUDE_CODE_DISABLE_BACKGROUND_TASKS: '0', CLAUDE_CODE_DISABLE_MCP_TASK_BACKGROUND: '0',
    } }));
    let release;
    const closed = new Promise(resolve => { release = resolve; });
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 15000);
    let q;
    try {
      q = query({ prompt: (async function* () { await closed; })(), options: {
        cwd, settingSources: ['project'], mcpServers: {}, ...backgroundOptions(),
        abortController: controller,
      } });
      const initialization = await q.initializationResult();
      assert.equal(initialization.hook_on_failure_supported, true);
      await assert.rejects(q.backgroundTasks(), /Background tasks are disabled/);
    } finally {
      release(); q?.close(); controller.abort(); clearTimeout(timer);
      await rm(cwd, { recursive: true, force: true });
    }
  });

test('pinned SDK public spawn hook confirms the query subprocess actually exits without inference',
  { skip: !directory, timeout: 20000 }, async () => {
    const metadata = JSON.parse(await readFile(path.join(directory, 'package.json'), 'utf8'));
    assert.equal(metadata.version, '0.3.322');
    const { query } = await import(pathToFileURL(path.join(directory, 'sdk.mjs')).href);
    const cwd = await mkdtemp(path.join(tmpdir(), 'kp-sdk-exit-offline-'));
    const barrier = new ProcessBarrier();
    let release;
    const closed = new Promise(resolve => { release = resolve; });
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 15000);
    let q;
    try {
      q = query({ prompt: (async function* () { await closed; })(), options: {
        cwd, settingSources: [], mcpServers: {}, ...backgroundOptions(),
        spawnClaudeCodeProcess: barrier.spawn, abortController: controller,
      } });
      await q.initializationResult();
      assert.equal(barrier.spawned, true);
      assert.ok(barrier.processes.some(record => !record.exited));
      q.close();
      assert.equal(await barrier.drain(), true);
      assert.ok(barrier.processes.every(record => record.exited));
    } finally {
      release(); q?.close(); controller.abort(); clearTimeout(timer);
      await barrier.drain();
      await rm(cwd, { recursive: true, force: true });
    }
  });

test('pinned SDK acknowledges removing and replacing a host-owned MCP transport without a prompt',
  { skip: !directory, timeout: 30000 }, async () => {
    const metadata = JSON.parse(await readFile(path.join(directory, 'package.json'), 'utf8'));
    assert.equal(metadata.version, '0.3.322');
    const { query } = await import(pathToFileURL(path.join(directory, 'sdk.mjs')).href);
    const cwd = await mkdtemp(path.join(tmpdir(), 'kp-sdk-refresh-offline-'));
    const fixture = path.join(cwd, 'mcp.mjs');
    const events = path.join(cwd, 'events.jsonl');
    await writeFile(fixture, `
import { appendFileSync } from 'node:fs';
import { createInterface } from 'node:readline';
const epoch = process.env.KETPLUS_FIXTURE_EPOCH;
const note = event => appendFileSync(process.env.KETPLUS_FIXTURE_EVENTS,
  JSON.stringify({event, epoch, pid: process.pid}) + '\\n');
note('started');
process.once('exit', () => note('exited'));
process.once('SIGTERM', () => process.exit(0));
const input = createInterface({input: process.stdin});
input.once('close', () => process.exit(0));
input.on('line', line => {
  const request = JSON.parse(line);
  if (request.id === undefined) return;
  let result = {};
  if (request.method === 'initialize') {
    note('initialized');
    result = {protocolVersion: request.params.protocolVersion, capabilities: {tools: {}},
      serverInfo: {name: 'ketplus-origin-fixture', version: epoch}};
  } else if (request.method === 'tools/list') result = {tools: []};
  process.stdout.write(JSON.stringify({jsonrpc: '2.0', id: request.id, result}) + '\\n');
});
`);
    // These are synthetic public fixture labels, never production credentials.
    const server = epoch => ({ type: 'stdio', command: process.execPath, args: [fixture],
      env: { KETPLUS_FIXTURE_EPOCH: epoch, KETPLUS_FIXTURE_EVENTS: events } });
    let release;
    const closed = new Promise(resolve => { release = resolve; });
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 25000);
    let q;
    try {
      q = query({ prompt: (async function* () { await closed; })(), options: {
        cwd, settingSources: [], mcpServers: {}, ...backgroundOptions(),
        abortController: controller,
      } });
      await q.initializationResult();
      const first = await q.setMcpServers({ ketplus_origin_fixture: server('first') });
      assert.deepEqual(first.errors, {});
      assert.ok(first.added.includes('ketplus_origin_fixture'));
      const removed = await q.setMcpServers({});
      assert.deepEqual(removed.errors, {});
      assert.ok(removed.removed.includes('ketplus_origin_fixture'));
      const beforeReplacement = (await readFile(events, 'utf8')).trim().split('\n').map(JSON.parse);
      const firstPid = beforeReplacement.find(e => e.epoch === 'first').pid;
      const alive = pid => {
        try { process.kill(pid, 0); return true; }
        catch (error) { if (error.code === 'ESRCH') return false; throw error; }
      };
      const deadline = Date.now() + 3000;
      while (alive(firstPid) && Date.now() < deadline)
        await new Promise(resolve => setTimeout(resolve, 10));
      assert.equal(alive(firstPid), false, 'removed fixture worker must actually exit');
      const second = await q.setMcpServers({ ketplus_origin_fixture: server('second') });
      assert.deepEqual(second.errors, {});
      assert.ok(second.added.includes('ketplus_origin_fixture'));
      const entries = (await readFile(events, 'utf8')).trim().split('\n').map(JSON.parse);
      assert.ok(entries.some(e => e.epoch === 'first' && e.event === 'initialized'));
      assert.ok(entries.some(e => e.epoch === 'second' && e.event === 'initialized'));
    } finally {
      release(); q?.close(); controller.abort(); clearTimeout(timer);
      await rm(cwd, { recursive: true, force: true });
    }
  });
