import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, copyFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';

const experimentalUsage = 'usage_EXPERIMENTAL_MAY_CHANGE_DO_NOT_RELY_ON_THIS_API_YET';

// The real SDK answers both probes over the query's control channel and tears that
// channel down with the iterator, so a probe made after the loop never gets a reply.
// This stub reproduces exactly that: once drained, the probes hang forever.
// A long turn takes model steps and hears a rate-limit change before its result,
// and the window it reports grows with every step.
const longTurn = `
  yield {type: 'assistant', message: {content: [{type: 'text', text: 'step'}]}};
  yield {type: 'rate_limit_event', rate_limit_info: {status: 'allowed', rateLimitType: 'five_hour', utilization: 0.5}};
  await new Promise(resolve => setTimeout(resolve, 400));
`;

function stubSdk(usage, steps = '', threshold) {
  return `
    export function query() {
      let closed = false;
      let reads = 0;
      const alive = value => closed ? new Promise(() => {}) : Promise.resolve(value);
      const iterator = (async function* () {
        yield {type: 'system', session_id: 'sdk-session'};
        ${steps}
        yield {type: 'result', result: 'ok', is_error: false};
        closed = true;
      })();
      return Object.assign(iterator, {
        getContextUsage: () => alive({totalTokens: 85000 + 1000 * reads++, maxTokens: 200000,
                                     autoCompactThreshold: ${JSON.stringify(threshold)}}),
        ${experimentalUsage}: () => alive(${usage}),
      });
    }
  `;
}

async function fixture(t, usage, steps, threshold, authentication) {
  const dir = await mkdtemp(path.join(tmpdir(), 'kp-claude-usage-'));
  for (const name of ['session-usage.mjs', 'authentication.mjs', 'index.mjs', 'transcript.mjs', 'input.mjs', 'tasks.mjs', 'resume.mjs', 'session-delete.mjs'])
    await copyFile(fileURLToPath(new URL(`../../../apps/daemon/providers/claude-sidecar/${name}`, import.meta.url)), path.join(dir, name));
  const sdk = path.join(dir, 'node_modules/@anthropic-ai/claude-agent-sdk');
  await mkdir(sdk, { recursive: true });
  await writeFile(path.join(sdk, 'package.json'), JSON.stringify({ type: 'module', main: 'index.mjs' }));
  await writeFile(path.join(sdk, 'index.mjs'), stubSdk(usage, steps, threshold));
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
  send({id: 1, method: 'turn/start', params: {sessionId: 's', prompt: 'hello', authentication}});
  return { send, wait, events };
}

const plan = `{
  rate_limits_available: true,
  rate_limits: {
    five_hour: {utilization: 41.6, resets_at: '2026-09-20T12:00:00Z'},
    seven_day: {utilization: 12, resets_at: '2026-09-24T00:00:00Z'},
    seven_day_opus: {utilization: null, resets_at: null},
    model_scoped: [{display_name: 'Fable', utilization: 7, resets_at: '2026-09-24T00:00:00Z'}],
  },
}`;

test('resolved auto-compaction threshold stays separate from physical context capacity', async t => {
  const f = await fixture(t, plan, '', 167000);
  const usage = await f.wait(e => e.params?.type === 'usage.context');
  assert.deepEqual(usage.params.payload.tokenUsage,
                   {last:{totalTokens:85000}, modelContextWindow:200000, autoCompactThreshold:167000});
});

test('The context window is read while the query still answers', async t => {
  const f = await fixture(t, plan);
  const usage = await f.wait(e => e.params?.type === 'usage.context');
  assert.deepEqual(usage.params.payload.tokenUsage,
                   {last: {totalTokens: 85000}, modelContextWindow: 200000});
  // Reading it must not cost the turn its completion.
  const done = await f.wait(e => e.params?.type === 'turn.completed');
  assert.equal(done.params.payload.status, 'completed');
});

test('Plan windows become rows, and an unknown window is not a confident zero', async t => {
  const f = await fixture(t, plan);
  const limits = await f.wait(e => e.params?.type === 'usage.limits');
  assert.deepEqual(limits.params.payload.limits, [
    {id: 'five_hour', label: 'Session · 5 hours', usedPercent: 41.6,
     resetsAt: Date.parse('2026-09-20T12:00:00Z') / 1000, available: true},
    {id: 'seven_day', label: 'Weekly · all models', usedPercent: 12,
     resetsAt: Date.parse('2026-09-24T00:00:00Z') / 1000, available: true},
    {id: 'model_scoped_0', label: 'Weekly · Fable', usedPercent: 7,
     resetsAt: Date.parse('2026-09-24T00:00:00Z') / 1000, available: true},
  ]);
});

test('A session without plan limits reports none rather than an empty card', async t => {
  const f = await fixture(t, '{rate_limits_available: false, rate_limits: null}');
  await f.wait(e => e.params?.type === 'turn.completed');
  assert.equal(f.events.filter(e => e.params?.type === 'usage.limits').length, 0);
  assert.equal(f.events.filter(e => e.params?.type === 'usage.context').length, 1);
});

test('A running turn reports its window and plan rows before it ends', async t => {
  const f = await fixture(t, plan, longTurn);
  await f.wait(e => e.params?.type === 'turn.completed');
  const types = f.events.map(e => e.params?.type).filter(Boolean);
  const done = types.indexOf('turn.completed');
  assert.ok(types.indexOf('usage.context') < done, JSON.stringify(types));
  assert.ok(types.indexOf('usage.limits') < done, JSON.stringify(types));
  // The result reads the window again; it grew, so it is reported. The plan rows
  // did not change, so the daemon is not sent the same rows twice.
  assert.deepEqual(f.events.filter(e => e.params?.type === 'usage.context')
                     .map(e => e.params.payload.tokenUsage.last.totalTokens), [85000, 86000]);
  assert.equal(types.filter(type => type === 'usage.limits').length, 1);
});

// An inherited plan cache must not leak into explicitly selected API billing.
test('API query publishes scoped budget and session accounting, suppressing subscription windows', async t => {
  const f = await fixture(t, `{session:{total_cost_usd:12.5,model_usage:{sonnet:{inputTokens:50,outputTokens:5,cacheReadInputTokens:20,cacheCreationInputTokens:0}}},...${plan}}`, '', undefined,
    {enabled:true,key:'sk-ant-test-only',budget_usd:5});
  await f.wait(e => e.params?.type === 'turn.completed');
  const snapshots=f.events.filter(e => e.params?.type === 'usage.session').map(e=>e.params.payload);
  assert.deepEqual(snapshots.at(-1), {mode:'api_key',budgetUsd:5,estimatedCostUsd:12.5,inputTokens:50,outputTokens:5,cacheReadInputTokens:20,cacheCreationInputTokens:0});
  assert.equal(f.events.filter(e=>e.params?.type === 'usage.limits').length,0);
  assert.ok(!JSON.stringify(snapshots).includes('sk-ant'));
});
