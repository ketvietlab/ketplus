import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, copyFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

// Behaves like Claude Code with a background shell: the first turn starts a
// task and settles while it runs; the query answers later user messages and,
// once the task ends, the agent replies to its notification on its own.
const stubSdk = `
  export function query({prompt, options}) {
    console.log(JSON.stringify({method: 'test.query', params: {extraArgs: options.extraArgs}}));
    const input = prompt[Symbol.asyncIterator]();
    let pendingInput = null;
    const nextInput = () => (pendingInput ||= input.next().then(value => { pendingInput = null; return {input: value}; }));
    let wake = null;
    const control = () => new Promise(resolve => { wake = resolve; });
    let serial = 0;
    const start = () => ({type: 'stream_event', parent_tool_use_id: null, event: {type: 'message_start', message: {id: 'm' + ++serial}}});
    const gen = (async function* () {
      await nextInput();
      yield {type: 'system', subtype: 'init', session_id: 'sdk-session'};
      yield start();
      yield {type: 'assistant', parent_tool_use_id: null, message: {id: 'm1', content: [
        {type: 'tool_use', id: 'tu1', name: 'Bash', input: {command: 'sleep 100', run_in_background: true}}]}};
      yield {type: 'system', subtype: 'task_started', task_id: 't1', tool_use_id: 'tu1', description: 'Sleep a while',
        task_type: 'local_bash', is_backgrounded: true};
      yield {type: 'system', subtype: 'background_tasks_changed', tasks: [{task_id: 't1', task_type: 'local_bash', description: 'Sleep a while'}]};
      yield {type: 'result', result: 'started', is_error: false};
      while (true) {
        const event = await Promise.race([nextInput(), control()]);
        if (event.input) {
          if (event.input.done) return;
          const text = event.input.value.message.content;
          yield start();
          if (text === 'wait') await control();
          yield {type: 'result', result: text === 'wait' ? '' : 'reply: ' + text, is_error: false};
          continue;
        }
        if (event.stop) {
          yield {type: 'system', subtype: 'background_tasks_changed', tasks: []};
          yield {type: 'system', subtype: 'task_notification', task_id: event.stop, status: 'stopped',
            output_file: '/nonexistent/t1.output', summary: 'Sleep stopped'};
          yield start();
          yield {type: 'result', result: 'noticed the stop', is_error: false};
        }
      }
    })();
    gen.applyFlagSettings = async settings => console.log(JSON.stringify({method: 'test.settings', params: settings}));
    gen.stopTask = async id => wake?.({stop: id});
    gen.interrupt = async () => wake?.({interrupt: true});
    gen.getTaskOutput = async () => ({output: 'line 1\\nline 2', total_bytes: 14, truncated: false});
    gen.close = () => console.log(JSON.stringify({method: 'test.closed'}));
    return gen;
  }
`;

async function fixture(t) {
  const dir = await mkdtemp(path.join(tmpdir(), 'kp-claude-tasks-'));
  for (const name of ['session-usage.mjs', 'authentication.mjs', 'index.mjs', 'transcript.mjs', 'input.mjs', 'tasks.mjs', 'resume.mjs', 'session-delete.mjs'])
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
  const wait = async (predicate, after = 0) => {
    for (let i = 0; i < 300; i++) {
      const found = events.slice(after).find(predicate);
      if (found) return found;
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert.fail('Missing event: ' + stderr + JSON.stringify(events));
  };
  const of = type => events.filter(e => e.params?.type === type);
  return { send, wait, events, of };
}

const lastTasks = f => f.of('tasks.changed').at(-1).params.payload.tasks;

test('a background task outlives its turn and keeps the query for the next one', async t => {
  const f = await fixture(t);
  f.send({id: 1, method: 'turn/start', params: {sessionId: 's', prompt: 'start'}});
  const first = await f.wait(e => e.params?.type === 'turn.completed');
  assert.equal(first.params.payload.status, 'completed');
  const [task] = lastTasks(f);
  assert.equal(task.task_id, 't1');
  assert.equal(task.kind, 'shell');
  assert.equal(task.status, 'running');
  assert.equal(task.command, 'sleep 100');
  assert.equal(task.title, 'Sleep a while');
  // The task keeps the query, and its process, alive past the result.
  await new Promise(resolve => setTimeout(resolve, 50));
  assert.equal(f.events.filter(e => e.method === 'test.closed').length, 0);

  f.send({id: 2, method: 'task/output', params: {sessionId: 's', taskId: 't1'}});
  assert.deepEqual((await f.wait(e => e.id === 2)).result, {output: 'line 1\nline 2', total_bytes: 14, truncated: false});

  const before = f.events.length;
  f.send({id: 3, method: 'turn/start', params: {sessionId: 's', prompt: 'hello'}});
  const reply = await f.wait(e => e.id === 3, before);
  assert.equal(reply.result.thread.id, 'sdk-session');
  const second = await f.wait(e => e.params?.type === 'turn.completed', before);
  assert.equal(second.params.payload.status, 'completed');
  const answers = f.of('item.completed').map(e => e.params.payload.item)
    .filter(item => item.phase === 'final_answer').map(item => item.text);
  assert.equal(answers.at(-1), 'reply: hello');
  assert.equal(f.events.filter(e => e.method === 'test.query').length, 1);

  // Stopping the task lets the agent answer its notification, then the query closes.
  const stopAt = f.events.length;
  f.send({id: 4, method: 'task/stop', params: {sessionId: 's', taskId: 't1'}});
  assert.deepEqual((await f.wait(e => e.id === 4, stopAt)).result, {});
  await f.wait(e => e.method === 'test.closed', stopAt);
  assert.ok(f.of('tasks.changed').some(e => e.params.payload.tasks[0]?.status === 'stopping'));
  assert.equal(lastTasks(f)[0].status, 'stopped');
  assert.equal(lastTasks(f)[0].result, 'Sleep stopped');
  const turns = f.events.slice(stopAt).filter(e => e.params?.type === 'turn.started' || e.params?.type === 'turn.completed');
  assert.deepEqual(turns.map(e => e.params.type), ['turn.started', 'turn.completed']);
  assert.equal(f.of('runtime.error').length, 0);

  f.send({id: 5, method: 'task/stop', params: {sessionId: 's', taskId: 't1'}});
  assert.match((await f.wait(e => e.id === 5)).error.message, /no longer running/);
});

test('stopping a turn leaves its background tasks running', async t => {
  const f = await fixture(t);
  f.send({id: 1, method: 'turn/start', params: {sessionId: 's', prompt: 'start'}});
  await f.wait(e => e.params?.type === 'turn.completed');
  const before = f.events.length;
  f.send({id: 2, method: 'turn/start', params: {sessionId: 's', prompt: 'wait'}});
  await f.wait(e => e.params?.type === 'turn.started', before);
  f.send({id: 3, method: 'turn/interrupt', params: {sessionId: 's'}});
  const done = await f.wait(e => e.params?.type === 'turn.completed', before);
  assert.equal(done.params.payload.status, 'interrupted');
  await new Promise(resolve => setTimeout(resolve, 50));
  assert.equal(f.events.filter(e => e.method === 'test.closed').length, 0);
  assert.equal(lastTasks(f)[0].status, 'running');
  // Between turns there is nothing to interrupt; the task is untouched.
  f.send({id: 4, method: 'turn/interrupt', params: {sessionId: 's'}});
  assert.deepEqual((await f.wait(e => e.id === 4)).result, {});
  assert.equal(lastTasks(f)[0].status, 'running');
});

test('changing compactWindow preserves live background tasks by refusing the change', async t => {
  const f = await fixture(t);
  f.send({id:1, method:'turn/start', params:{sessionId:'s', prompt:'start', compactWindow:272000}});
  await f.wait(e => e.params?.type === 'turn.completed');
  assert.deepEqual(f.events.find(e => e.method === 'test.query').params.extraArgs, {autocompact:'272000'});
  const before = f.events.length;
  f.send({id:2, method:'turn/start', params:{sessionId:'s', prompt:'new policy', compactWindow:500000}});
  assert.match((await f.wait(e => e.id === 2, before)).error.message, /background tasks/);
  assert.equal(lastTasks(f)[0].status, 'running');
  assert.equal(f.events.filter(e => e.method === 'test.closed').length, 0);
  f.send({id:3, method:'turn/start', params:{sessionId:'s', prompt:'same policy', compactWindow:272000}});
  await f.wait(e => e.id === 3, before);
  await f.wait(e => e.params?.type === 'turn.completed', before);
  assert.equal(f.events.filter(e => e.method === 'test.query').length, 1);
});

test('a worktree change refuses to close a query with live background tasks', async t => {
  const f = await fixture(t);
  f.send({id: 1, method: 'turn/start', params: {sessionId: 's', prompt: 'start'}});
  await f.wait(e => e.params?.type === 'turn.completed');
  const before = f.events.length;
  f.send({id: 2, method: 'turn/start', params: {sessionId: 's', prompt: 'elsewhere', cwd: tmpdir()}});
  assert.match((await f.wait(e => e.id === 2, before)).error.message, /background tasks/);
  assert.equal(lastTasks(f)[0].status, 'running');
  assert.equal(f.events.filter(e => e.method === 'test.closed').length, 0);
  f.send({id: 3, method: 'turn/start', params: {sessionId: 's', prompt: 'same worktree'}});
  await f.wait(e => e.id === 3, before);
  await f.wait(e => e.params?.type === 'turn.completed', before);
  assert.equal(f.events.filter(e => e.method === 'test.query').length, 1);
});

test('returning to a provider starts fresh instead of continuing its kept context', async t => {
  const f = await fixture(t);
  f.send({id: 1, method: 'turn/start', params: {sessionId: 's', prompt: 'old A'}});
  await f.wait(e => e.params?.type === 'turn.completed');
  const before = f.events.length;
  f.send({id: 2, method: 'turn/start', params: {
    sessionId: 's', prompt: 'latest handoff from B', freshContext: true
  }});
  await f.wait(e => e.id === 2, before);
  assert.equal(f.events.slice(before).filter(e => e.method === 'test.closed').length, 1);
  assert.equal(f.events.filter(e => e.method === 'test.query').length, 2);
});

test('next-turn effort reaches a query kept for background tasks', async t => {
  const f = await fixture(t);
  f.send({id: 1, method: 'turn/start', params: {sessionId: 's', prompt: 'start', effort: 'low'}});
  await f.wait(e => e.params?.type === 'turn.completed');
  const before = f.events.length;
  f.send({id: 2, method: 'turn/start', params: {sessionId: 's', prompt: 'continue', model: 'opus', effort: 'max'}});
  await f.wait(e => e.id === 2, before);
  assert.deepEqual((await f.wait(e => e.method === 'test.settings', before)).params,
    {model: 'opus', effortLevel: 'max'});
  assert.equal(f.events.filter(e => e.method === 'test.query').length, 1);
  assert.equal(lastTasks(f)[0].status, 'running');
});


test('credential rotation does not terminate background work or reuse old credentials', async t => {
    const f = await fixture(t);
    f.send({
        id : 1,
        method : 'turn/start',
        params : {
            sessionId : 's',
            prompt : 'start',
            authentication : {enabled : true, key : 'sk-ant-test-a', revision : 'a'}
        }
    });
    await f.wait(e => e.params?.type === 'turn.completed');
    const before = f.events.length;
    f.send({
        id : 2,
        method : 'turn/start',
        params : {
            sessionId : 's',
            prompt : 'next',
            authentication : {enabled : true, key : 'sk-ant-test-b', revision : 'b'}
        }
    });
    const reply = await f.wait(e => e.id === 2, before);
    assert.match(reply.error.message, /background tasks/);
    assert.equal(f.events.filter(e => e.method === 'test.query').length, 1);
    assert.equal(f.events.filter(e => e.method === 'test.closed').length, 0);
    f.send({
        id : 3,
        method : 'turn/start',
        params : {
            sessionId : 's',
            prompt : 'continue',
            authentication : {enabled : true, key : 'sk-ant-test-a', revision : 'a'}
        }
    });
    assert.equal((await f.wait(e => e.id === 3, before)).error, undefined);
});
