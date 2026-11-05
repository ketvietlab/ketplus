import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, copyFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

// Behaves like Claude Code in streaming input: a steer sent while the last
// request streams gets one more request after the first result.
const stubSdk = `
  export function query({prompt}) {
    const input = prompt[Symbol.asyncIterator]();
    return (async function* () {
      await input.next();
      yield {type:'system', session_id:'sdk-session'};
      yield {type:'stream_event', parent_tool_use_id:null, event:{type:'message_start', message:{id:'m1'}}};
      const steer = await input.next();
      yield {type:'result', result:'first', is_error:false};
      yield {type:'stream_event', parent_tool_use_id:null, event:{type:'message_start', message:{id:'m2'}}};
      const content = steer.value.message.content;
      yield {type:'result', result:'steered: ' + (typeof content === 'string' ? content : JSON.stringify(content)), is_error:false};
      if (!(await input.next()).done) throw new Error('input stayed open');
    })();
  }
`;

async function fixture(t, sdkSource = stubSdk) {
  const dir = await mkdtemp(path.join(tmpdir(), 'kp-claude-steer-'));
  for (const name of ['session-usage.mjs', 'authentication.mjs', 'index.mjs', 'transcript.mjs', 'input.mjs', 'tasks.mjs', 'resume.mjs', 'session-delete.mjs'])
    await copyFile(fileURLToPath(new URL(`../../../apps/daemon/providers/claude-sidecar/${name}`, import.meta.url)), path.join(dir, name));
  const sdk = path.join(dir, 'node_modules/@anthropic-ai/claude-agent-sdk');
  await mkdir(sdk, { recursive: true });
  await writeFile(path.join(sdk, 'package.json'), JSON.stringify({ type: 'module', main: 'index.mjs' }));
  await writeFile(path.join(sdk, 'index.mjs'), sdkSource);
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

test('a steer joins the running turn and the turn ends once it is answered', async t => {
  const f = await fixture(t);
  f.send({id: 1, method: 'turn/start', params: {sessionId: 's', prompt: 'start'}});
  await f.wait(e => e.params?.type === 'turn.started');
  f.send({id: 2, method: 'turn/steer', params: {sessionId: 's', prompt: 'use the other branch'}});
  assert.deepEqual((await f.wait(e => e.id === 2)).result, {});
  const done = await f.wait(e => e.params?.type === 'turn.completed');
  assert.equal(done.params.payload.status, 'completed');
  assert.equal(f.events.filter(e => e.params?.type === 'turn.completed').length, 1);
  const answers = f.events.filter(e => e.params?.type === 'item.completed' &&
    e.params.payload.item.phase === 'final_answer').map(e => e.params.payload.item.text);
  assert.equal(answers.at(-1), 'steered: use the other branch');
  assert.equal(f.events.filter(e => e.params?.type === 'runtime.error').length, 0);

  f.send({id: 3, method: 'turn/steer', params: {sessionId: 's', prompt: 'too late'}});
  assert.match((await f.wait(e => e.id === 3)).error.message, /already finished/);
});

test('a steer carries its images to the running turn', async t => {
  const f = await fixture(t);
  const image = path.join(f.dir, 'screen.png');
  await writeFile(image, Buffer.from('\x89PNG steer'));
  f.send({id: 1, method: 'turn/start', params: {sessionId: 's', prompt: 'start'}});
  await f.wait(e => e.params?.type === 'turn.started');
  f.send({id: 2, method: 'turn/steer', params: {sessionId: 's', prompt: 'match this', images: [image]}});
  assert.deepEqual((await f.wait(e => e.id === 2)).result, {});
  await f.wait(e => e.params?.type === 'turn.completed');
  const answer = f.events.filter(e => e.params?.type === 'item.completed' &&
    e.params.payload.item.phase === 'final_answer').map(e => e.params.payload.item.text).at(-1);
  assert.deepEqual(JSON.parse(answer.slice('steered: '.length)), [
    {type: 'image', source: {type: 'base64', media_type: 'image/png',
      data: Buffer.from('\x89PNG steer').toString('base64')}},
    {type: 'text', text: 'match this'},
  ]);
});

test('a steer whose image cannot be read is refused', async t => {
  const f = await fixture(t);
  f.send({id: 1, method: 'turn/start', params: {sessionId: 's', prompt: 'start'}});
  await f.wait(e => e.params?.type === 'turn.started');
  f.send({id: 2, method: 'turn/steer', params: {sessionId: 's', prompt: 'look', images: [path.join(f.dir, 'gone.png')]}});
  assert.match((await f.wait(e => e.id === 2)).error.message, /could not be read/);
});

// Closing the prompt does not guarantee that the SDK output iterator ends.
// It may stay alive for background notifications; the daemon must still finish.
test('a settled result completes the turn without draining lingering SDK output', async t => {
  const f = await fixture(t, `
    export function query({prompt}) {
      return (async function* () {
        const input = prompt[Symbol.asyncIterator]();
        await input.next();
        yield {type:'result', result:'done', is_error:false};
        await input.next();
        await new Promise(() => {});
      })();
    }
  `);
  f.send({id: 1, method: 'turn/start', params: {sessionId: 's', prompt: 'start'}});
  const done = await f.wait(e => e.params?.type === 'turn.completed');
  assert.equal(done.params.payload.status, 'completed');
  assert.equal(f.events.filter(e => e.params?.type === 'turn.completed').length, 1);
  f.send({id: 2, method: 'turn/steer', params: {sessionId: 's', prompt: 'late'}});
  assert.match((await f.wait(e => e.id === 2)).error.message, /already finished/);
});

// These are startup/resume frames, not the answer to the newly supplied prompt.
for (const [name, stale] of [
  ['queued prompt', "{type:'result', result:'old answer', queued_turn_count:1, is_error:false}"],
  ['another prompt', "{type:'result', result:'old answer', user_message_uuid:'old-prompt', is_error:false}"],
  ['task notification', "{type:'result', result:'', origin:{kind:'task-notification'}, is_error:false}"],
  ['empty startup result', "{type:'result', result:'', num_turns:0, is_error:false}"],
]) {
  test(`a ${name} result cannot consume the new user turn`, async t => {
    const f = await fixture(t, `
      export function query({prompt}) {
        return (async function* () {
          const user = (await prompt[Symbol.asyncIterator]().next()).value;
          yield {type:'assistant', message:{model:'<synthetic>', content:[{type:'text', text:'No response requested.'}]}};
          yield ${stale};
          yield {type:'stream_event', user_message_uuid:user.uuid, event:{type:'message_start', message:{id:'fresh'}}};
          yield {type:'result', result:'answer to current prompt', user_message_uuid:user.uuid, is_error:false};
        })();
      }
    `);
    f.send({id:1, method:'turn/start', params:{sessionId:'s', prompt:'continue'}});
    const done = await f.wait(e => e.params?.type === 'turn.completed');
    assert.equal(done.params.payload.status, 'completed');
    const answers = f.events.filter(e => e.params?.type === 'item.completed' &&
      e.params.payload.item.phase === 'final_answer').map(e => e.params.payload.item.text);
    assert.deepEqual(answers, ['answer to current prompt']);
    assert.equal(f.events.filter(e => e.params?.type === 'turn.completed').length, 1);
    assert.equal(f.events.filter(e => e.params?.type === 'runtime.error').length, 0);
  });
}

for (const ending of ['eof', 'timeout']) {
  test(`an unanswered prompt fails visibly on ${ending}`, async t => {
    const f = await fixture(t, `
      const timeout = globalThis.setTimeout;
      globalThis.setTimeout = (fn, ms, ...args) => timeout(fn, ms === 60000 ? 30 : ms, ...args);
      export function query({prompt}) {
        return (async function* () {
          await prompt[Symbol.asyncIterator]().next();
          yield {type:'result', result:'', queued_turn_count:1, is_error:false};
          ${ending === 'timeout' ? 'await new Promise(() => {});' : ''}
        })();
      }
    `);
    f.send({id:1, method:'turn/start', params:{sessionId:'s', prompt:'continue'}});
    const done = await f.wait(e => e.params?.type === 'turn.completed');
    assert.equal(done.params.payload.status, 'failed');
    assert.match((await f.wait(e => e.params?.type === 'runtime.error')).params.payload.message,
      /before answering the pending user message/);
    assert.equal(f.events.filter(e => e.params?.type === 'turn.completed').length, 1);
  });
}

test('an SDK startup error is terminal even without a model request', async t => {
  const f = await fixture(t, `
    export function query({prompt}) {
      return (async function* () {
        await prompt[Symbol.asyncIterator]().next();
        yield {type:'result', is_error:true, errors:['authentication failed']};
      })();
    }
  `);
  f.send({id:1, method:'turn/start', params:{sessionId:'s', prompt:'continue'}});
  assert.equal((await f.wait(e => e.params?.type === 'turn.completed')).params.payload.status, 'failed');
});


test('steering accepted during a result usage probe still gets an answer', async t => {
  const f = await fixture(t, `
    export function query({prompt}) {
      const input = prompt[Symbol.asyncIterator]();
      let steer;
      const output = (async function* () {
        const first = (await input.next()).value;
        yield {type:'result', result:'first', user_message_uuid:first.uuid, is_error:false};
        yield {type:'result', result:'steered', user_message_uuid:steer.uuid, is_error:false};
      })();
      return Object.assign(output, {getContextUsage:async () => {
        if (!steer) {
          process.stdout.write(JSON.stringify({testProbe:true})+'\\n');
          steer = (await input.next()).value;
        }
        return {};
      }});
    }
  `);
  f.send({id:1, method:'turn/start', params:{sessionId:'s', prompt:'start'}});
  await f.wait(e => e.testProbe);
  f.send({id:2, method:'turn/steer', params:{sessionId:'s', prompt:'continue'}});
  await f.wait(e => e.id === 2);
  assert.equal((await f.wait(e => e.params?.type === 'turn.completed')).params.payload.status, 'completed');
  const answers = f.events.filter(e => e.params?.type === 'item.completed' &&
    e.params.payload.item.phase === 'final_answer').map(e => e.params.payload.item.text);
  assert.deepEqual(answers, ['steered']);
});

test('starting the pending model request cancels the resume wait deadline', async t => {
  const f = await fixture(t, `
    const timeout = globalThis.setTimeout;
    globalThis.setTimeout = (fn, ms, ...args) => timeout(fn, ms === 60000 ? 30 : ms, ...args);
    export function query({prompt}) {
      return (async function* () {
        const user = (await prompt[Symbol.asyncIterator]().next()).value;
        yield {type:'result', result:'', queued_turn_count:1};
        yield {type:'stream_event', user_message_uuid:user.uuid, event:{type:'message_start'}};
        await new Promise(resolve => setTimeout(resolve, 70));
        yield {type:'result', result:'done', user_message_uuid:user.uuid, is_error:false};
      })();
    }
  `);
  f.send({id:1, method:'turn/start', params:{sessionId:'s', prompt:'continue'}});
  assert.equal((await f.wait(e => e.params?.type === 'turn.completed')).params.payload.status, 'completed');
  assert.equal(f.events.filter(e => e.params?.type === 'runtime.error').length, 0);
});
