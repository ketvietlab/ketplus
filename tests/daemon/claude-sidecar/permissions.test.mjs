import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, copyFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

async function fixture(t) {
  const dir = await mkdtemp(path.join(tmpdir(), 'kp-claude-test-'));
  for (const name of ['session-usage.mjs', 'authentication.mjs', 'index.mjs', 'transcript.mjs', 'input.mjs', 'tasks.mjs', 'resume.mjs', 'session-delete.mjs', 'review-guard.mjs'])
    await copyFile(fileURLToPath(new URL(`../../../apps/daemon/providers/claude-sidecar/${name}`, import.meta.url)), path.join(dir, name));
  const sdk = path.join(dir, 'node_modules/@anthropic-ai/claude-agent-sdk');
  await mkdir(sdk, { recursive: true });
  await writeFile(path.join(sdk, 'package.json'), JSON.stringify({ type: 'module', main: 'index.mjs' }));
  await writeFile(path.join(sdk, 'index.mjs'), `
    export function query({prompt, options}) {
      return (async function* () {
        yield {type:'system',session_id:'sdk-session'};
        const first = (await prompt[Symbol.asyncIterator]().next()).value;
        const question = first.message.content === 'question';
        const input = question ? {questions:[{question:'Choose target',options:[{label:'Desktop'}]}]} : {command:'test'};
        const answer = await options.canUseTool(question ? 'AskUserQuestion' : 'Bash', input);
        if (options.abortController.signal.aborted) throw new Error('aborted');
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
  return { send, wait, events };
}

test('Stop releases a tool permission promise and reports interruption', async t => {
  const f = await fixture(t);
  f.send({id:1,method:'turn/start',params:{sessionId:'s',prompt:'permission'}});
  await f.wait(e => e.method === 'permission.request');
  f.send({id:2,method:'turn/interrupt',params:{sessionId:'s'}});
  await f.wait(e => e.id === 2 && e.result);
  const done = await f.wait(e => e.params?.type === 'turn.completed');
  assert.equal(done.params.payload.status, 'interrupted');
  assert.equal(f.events.filter(e => e.params?.type === 'runtime.error').length, 0);
});

test('AskUserQuestion returns chosen answers through updatedInput', async t => {
  const f = await fixture(t);
  f.send({id:1,method:'turn/start',params:{sessionId:'s',prompt:'question'}});
  const request = await f.wait(e => e.method === 'question.request');
  f.send({id:request.id,result:{answers:{'question-0':{answers:['Desktop']}}}});
  const done = await f.wait(e => e.params?.type === 'item.completed' && e.params.payload.item.phase === 'final_answer');
  const answer = JSON.parse(done.params.payload.item.text);
  assert.equal(answer.updatedInput.answers['Choose target'], 'Desktop');
});
