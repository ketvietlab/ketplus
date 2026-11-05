import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, copyFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

// ADR 0027: the daemon's ketplus_agents server runs without a per-call prompt, only when injected.
async function fixture(t) {
  const dir = await mkdtemp(path.join(tmpdir(), 'kp-agent-tools-'));
  for (const name of ['session-usage.mjs', 'authentication.mjs', 'index.mjs', 'transcript.mjs', 'input.mjs', 'tasks.mjs', 'resume.mjs', 'session-delete.mjs'])
    await copyFile(fileURLToPath(new URL(`../../../apps/daemon/providers/claude-sidecar/${name}`, import.meta.url)), path.join(dir, name));
  const sdk = path.join(dir, 'node_modules/@anthropic-ai/claude-agent-sdk');
  await mkdir(sdk, { recursive: true });
  await writeFile(path.join(sdk, 'package.json'), JSON.stringify({ type: 'module', main: 'index.mjs' }));
  await writeFile(path.join(sdk, 'index.mjs'), `
    export function query({prompt, options}) {
      console.log(JSON.stringify({method:'test.options', params:{
        allowedTools:options.allowedTools, servers:Object.keys(options.mcpServers || {}),
      }}));
      return (async function* () {
        await prompt[Symbol.asyncIterator]().next();
        yield {type:'system', subtype:'init', session_id:'sdk-session'};
        const answer = await options.canUseTool('mcp__ketplus_agents__agents_send', {to:'4821', text:'hi'});
        yield {type:'result', result:JSON.stringify(answer), is_error:false};
      })();
    }
  `);
  const child = spawn(process.execPath, [path.join(dir, 'index.mjs')], { stdio: ['pipe', 'pipe', 'pipe'] });
  const events = []; let stderr = '';
  child.stderr.on('data', b => stderr += b);
  createInterface({ input: child.stdout }).on('line', l => events.push(JSON.parse(l)));
  t.after(async () => { child.kill(); await rm(dir, { recursive: true, force: true }); });
  const send = value => child.stdin.write(JSON.stringify(value) + '\n');
  const wait = async predicate => {
    for (let i = 0; i < 300; i++) {
      const found = events.find(predicate);
      if (found) return found;
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert.fail('Missing event: ' + stderr + JSON.stringify(events));
  };
  return { send, wait, events, dir };
}

test('an injected ketplus_agents server is allowed without a permission prompt', async t => {
  const f = await fixture(t);
  f.send({ id: 1, method: 'turn/start', params: {
    sessionId: 's', cwd: f.dir, prompt: 'hello', permission: 'workspace-write',
    mcpServers: { ketplus_agents: { type: 'stdio', command: '/bin/true', args: ['--mcp-agents'], env: {} } },
  }});
  const options = (await f.wait(e => e.method === 'test.options')).params;
  assert.deepEqual(options.servers, ['ketplus_agents']);
  assert.ok(options.allowedTools.includes('mcp__ketplus_agents__*'));
  await f.wait(e => e.params?.type === 'turn.completed');
  assert.equal(f.events.filter(e => e.method === 'permission.request').length, 0);
});

test('without the injected server the same tool name still asks', async t => {
  const f = await fixture(t);
  f.send({ id: 1, method: 'turn/start', params: {
    sessionId: 's', cwd: f.dir, prompt: 'hello', permission: 'workspace-write', mcpServers: {},
  }});
  const options = (await f.wait(e => e.method === 'test.options')).params;
  assert.ok(!options.allowedTools.includes('mcp__ketplus_agents__*'));
  await f.wait(e => e.method === 'permission.request');
});
