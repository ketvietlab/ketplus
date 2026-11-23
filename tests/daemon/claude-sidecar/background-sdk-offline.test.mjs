import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
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

test('pinned SDK keeps project memory and filters skill descriptions in the prompt',
  { skip: !directory, timeout: 20000 }, async () => {
    const { query } = await import(pathToFileURL(path.join(directory, 'sdk.mjs')).href);
    const cwd = await mkdtemp(path.join(tmpdir(), 'kp-sdk-skill-prompt-'));
    const requests = [];
    // Use only local synthetic credentials and reject every request on loopback.
    const server = createServer(async (request, response) => {
      let body = '';
      for await (const chunk of request) body += chunk;
      if (request.url.startsWith('/v1/messages') && body.includes('Compare offline skills'))
        requests.push(body);
      response.writeHead(400, { 'content-type': 'application/json' });
      response.end(JSON.stringify({ type: 'error', error: {
        type: 'invalid_request_error', message: 'Offline fixture',
      } }));
    });
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 15000);
    try {
      await new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(0, '127.0.0.1', resolve);
      });
      for (const name of ['keep', 'hide']) {
        const skill = path.join(cwd, '.claude', 'skills', name);
        await mkdir(skill, { recursive: true });
        await writeFile(path.join(skill, 'SKILL.md'),
          `---\nname: ${name}\ndescription: OFFLINE_${name.toUpperCase()}\n---\nFixture\n`);
      }
      await writeFile(path.join(cwd, 'CLAUDE.md'), 'OFFLINE_PROJECT_MEMORY');
      const config = path.join(cwd, 'config');
      await mkdir(config);
      await writeFile(path.join(config, 'CLAUDE.md'), 'OFFLINE_USER_MEMORY');
      const plugin = path.join(cwd, 'plugin');
      const pluginSkill = path.join(plugin, 'skills', 'other');
      await mkdir(pluginSkill, { recursive: true });
      await mkdir(path.join(plugin, '.claude-plugin'));
      await writeFile(path.join(plugin, '.claude-plugin', 'plugin.json'),
        JSON.stringify({ name: 'offline-plugin' }));
      await writeFile(path.join(pluginSkill, 'SKILL.md'),
        '---\nname: other\ndescription: OFFLINE_PLUGIN_SKILL\n---\nFixture\n');
      for (const skills of [undefined, ['keep']]) {
        requests.length = 0;
        const q = query({ prompt: 'Compare offline skills', options: {
          cwd, skills, plugins: [{ type: 'local', path: plugin }],
          systemPrompt: { type: 'preset', preset: 'claude_code' },
          abortController: controller,
          env: {
            PATH: process.env.PATH, HOME: cwd, USERPROFILE: cwd,
            CLAUDE_CONFIG_DIR: config,
            ANTHROPIC_BASE_URL: `http://127.0.0.1:${server.address().port}`,
            ANTHROPIC_API_KEY: 'offline-fixture',
            CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC: '1',
          },
        } });
        // Ensure the rejection is the fixture's by asserting the captured request.
        try { for await (const _ of q) { /* Drain the fixture response. */ } }
        catch {} finally { q.close(); }
        assert.ok(requests.length > 0);
        assert.match(requests[0], /OFFLINE_PROJECT_MEMORY/);
        assert.match(requests[0], /OFFLINE_USER_MEMORY/);
        assert.match(requests[0], /OFFLINE_KEEP/);
        assert.equal(requests[0].includes('OFFLINE_PLUGIN_SKILL'), skills === undefined);
        assert.equal(requests[0].includes('OFFLINE_HIDE'), skills === undefined);
      }
    } finally {
      controller.abort();
      clearTimeout(timer);
      server.close();
      await rm(cwd, { recursive: true, force: true });
    }
  });
