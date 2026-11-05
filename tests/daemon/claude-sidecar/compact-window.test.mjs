import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, copyFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

async function fixture(t) {
  const dir = await mkdtemp(path.join(tmpdir(), 'kp-compact-window-'));
  for (const name of ['session-usage.mjs', 'authentication.mjs', 'index.mjs', 'transcript.mjs', 'input.mjs', 'tasks.mjs', 'resume.mjs', 'session-delete.mjs'])
    await copyFile(fileURLToPath(new URL(`../../../apps/daemon/providers/claude-sidecar/${name}`, import.meta.url)), path.join(dir, name));
  const sdk = path.join(dir, 'node_modules/@anthropic-ai/claude-agent-sdk');
  await mkdir(sdk, { recursive: true });
  await writeFile(path.join(sdk, 'package.json'), JSON.stringify({ type: 'module', main: 'index.mjs' }));
  await writeFile(path.join(sdk, 'index.mjs'), `
    export function query({prompt, options}) {
      console.log(JSON.stringify({method:'test.options', params:{
        extraArgs:options.extraArgs, hasEnv:Object.hasOwn(options,'env'),
        hasSettings:Object.hasOwn(options,'settings'), marker:process.env.KETPLUS_TEST_KEEP_ENV,
      }}));
      return (async function* () {
        await prompt[Symbol.asyncIterator]().next();
        yield {type:'system', subtype:'init', session_id:'sdk-session'};
        yield {type:'result', result:'done', is_error:false};
      })();
    }
  `);
  const child = spawn(process.execPath, [path.join(dir, 'index.mjs')], {
    stdio: ['pipe', 'pipe', 'pipe'], env: { ...process.env, KETPLUS_TEST_KEEP_ENV: 'preserved' },
  });
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

for (const window of [100000, 272000, 1000000, null, undefined]) {
  test(`compactWindow ${window} uses native flags and preserves environment`, async t => {
    const f = await fixture(t);
    for (const [id, resume] of [[1, undefined], [2, 'sdk-session']]) {
      const before = f.events.filter(e => e.method === 'test.options').length;
      f.send({ id, method: 'turn/start', params: {
        sessionId: 's', cwd: f.dir, prompt: 'hello', compactWindow: window, resume,
      }});
      await f.wait(e => e.id === id);
      await f.wait(() => f.events.filter(e => e.params?.type === 'turn.completed').length === id);
      const options = f.events.filter(e => e.method === 'test.options').at(before).params;
      assert.deepEqual(options.extraArgs, window == null ? undefined : { autocompact: String(window) });
      assert.equal(options.hasEnv, false);
      assert.equal(options.hasSettings, false);
      assert.equal(options.marker, 'preserved');
    }
  });
}

test('invalid compact windows never reach the SDK', async t => {
  const f = await fixture(t);
  for (const [index, window] of [0, -1, 99999, 1000001, 272000.5, '272000', true, {}, [272000]].entries()) {
    const id = index + 1;
    f.send({ id, method: 'turn/start', params: { sessionId:'s', prompt:'hello', compactWindow:window } });
    assert.match((await f.wait(e => e.id === id)).error.message, /compactWindow/);
  }
  assert.equal(f.events.filter(e => e.method === 'test.options').length, 0);
});
