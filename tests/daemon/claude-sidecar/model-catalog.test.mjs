import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, copyFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

// Lists no models until the account's plan allows them, as a lapsed subscription does.
const stubSdk = `
  let calls = 0;
  export function query() {
    calls += 1;
    const current = calls;
    return {
      async supportedModels() {
        if (current === 1) throw new Error('subscription required');
        return [{value: 'claude-opus-5-5', displayName: 'Opus 5.5'}];
      },
      close() {},
      async *[Symbol.asyncIterator]() {},
    };
  }
`;

async function fixture(t) {
  const dir = await mkdtemp(path.join(tmpdir(), 'kp-claude-catalog-'));
  for (const name of ['session-usage.mjs', 'authentication.mjs', 'index.mjs', 'transcript.mjs', 'input.mjs', 'tasks.mjs', 'resume.mjs', 'session-delete.mjs', 'review-guard.mjs', 'background-policy.mjs', 'process-barrier.mjs'])
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
  const wait = async predicate => {
    for (let i = 0; i < 300; i++) {
      const found = events.find(predicate);
      if (found) return found;
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert.fail('Missing event: ' + stderr + JSON.stringify(events));
  };
  return { send, wait };
}

const models = response => response.result.data.map(entry => entry.model);

test('a failed model listing is not kept, so a renewed plan shows its models', async t => {
  const f = await fixture(t);
  f.send({id: 1, method: 'model/list', params: {}});
  assert.deepEqual(models(await f.wait(e => e.id === 1)), ['opus', 'sonnet', 'haiku']);
  f.send({id: 2, method: 'model/list', params: {}});
  const renewed = await f.wait(e => e.id === 2);
  assert.equal(models(renewed)[0], 'claude-opus-5-5');
  assert.equal(renewed.result.live, undefined);
});
