import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, copyFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

// forkSession only copies the transcript; the stub records what it was asked to copy.
const forkingSdk = `
  export function query() { throw new Error('forking must not start a query'); }
  export async function forkSession(sessionId, options) {
    if (sessionId === 'missing') throw new Error('Session missing not found');
    return { sessionId: 'fork-of-' + sessionId + '@' + (options.dir || '') };
  }
`;
const olderSdk = `export function query() { throw new Error('unused'); }`;

async function fixture(t, stubSdk) {
  const dir = await mkdtemp(path.join(tmpdir(), 'kp-claude-fork-'));
  for (const name of ['session-usage.mjs', 'authentication.mjs', 'index.mjs', 'transcript.mjs', 'input.mjs', 'tasks.mjs', 'resume.mjs', 'session-delete.mjs', 'review-guard.mjs'])
    await copyFile(fileURLToPath(new URL(`../../../apps/daemon/providers/claude-sidecar/${name}`, import.meta.url)), path.join(dir, name));
  const sdk = path.join(dir, 'node_modules/@anthropic-ai/claude-agent-sdk');
  await mkdir(sdk, { recursive: true });
  await writeFile(path.join(sdk, 'package.json'), JSON.stringify({ type: 'module', main: 'index.mjs' }));
  await writeFile(path.join(sdk, 'index.mjs'), stubSdk);
  const child = spawn(process.execPath, [path.join(dir, 'index.mjs')], { stdio: ['pipe', 'pipe', 'pipe'] });
  const events = []; let stderr = '';
  child.stderr.on('data', b => stderr += b);
  createInterface({ input: child.stdout }).on('line', l => events.push(JSON.parse(l)));
  t.after(async () => { child.kill(); await rm(dir, { recursive: true, force: true }); });
  const send = value => child.stdin.write(JSON.stringify(value) + '\n');
  const reply = async id => {
    for (let i = 0; i < 300; i++) {
      const found = events.find(e => e.id === id);
      if (found) return found;
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert.fail('Missing reply ' + id + ': ' + stderr + JSON.stringify(events));
  };
  return { send, reply };
}

test('thread/fork copies the SDK session into a new thread', async t => {
  const f = await fixture(t, forkingSdk);
  f.send({id: 1, method: 'thread/fork', params: {threadId: 'sdk-1', cwd: '/work/tree'}});
  assert.deepEqual((await f.reply(1)).result, {thread: {id: 'fork-of-sdk-1@/work/tree'}});
});

test('thread/fork reports a missing thread or session as an error', async t => {
  const f = await fixture(t, forkingSdk);
  f.send({id: 1, method: 'thread/fork', params: {cwd: '/work/tree'}});
  assert.match((await f.reply(1)).error.message, /requires a threadId/);
  f.send({id: 2, method: 'thread/fork', params: {threadId: 'missing'}});
  assert.match((await f.reply(2)).error.message, /Session missing not found/);
});

test('thread/fork fails clearly on an SDK without forkSession', async t => {
  const f = await fixture(t, olderSdk);
  f.send({id: 1, method: 'thread/fork', params: {threadId: 'sdk-1'}});
  assert.match((await f.reply(1)).error.message, /cannot fork sessions/);
});
