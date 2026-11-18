import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, copyFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { backgroundDecision, backgroundOptions } from '../../../apps/daemon/providers/claude-sidecar/background-policy.mjs';

const attempts = [
  ['Bash', { command: 'must-not-run', run_in_background: true }],
  ['Agent', { prompt: 'implicit background default' }],
  ['Agent', { prompt: 'foreground fallback', run_in_background: false }],
  ['Agent', { resume: 'old-agent', isolation: 'remote' }],
  ['Task', { prompt: 'legacy alias' }],
  ['Workflow', { code: 'agent("hidden")' }],
  ['mcp__example__tool', { run_in_background: true }],
  ['Bash', { command: 'foreground', run_in_background: false }],
  ['mcp__ketplus_agents__background_task_start', { client_request_id: 'managed-child', kind: 'agent',
    title: 'Inspect code', execution_spec: { prompt: 'Inspect code', provider: 'claude',
      model: 'sonnet', effort: 'medium' }, timeout_ms: 5000 }],
  ['mcp__ketplus_agents__background_task_stop', { task_id: 'existing' }],
  ['mcp__ketplus_agents__background_task_output', { task_id: 'existing' }],
];

test('native guard denies implicit agents, resume, workflows and explicit background without granting foreground tools', async () => {
  const inherited = { SENTINEL: 'preserved', CLAUDE_CODE_DISABLE_BACKGROUND_TASKS: '0' };
  let reviewCalls = 0;
  const review = { PreToolUse: [{ hooks: [async () => { reviewCalls++; return {}; }] }], Stop: [] };
  const options = backgroundOptions(inherited, review, { apiKeyHelper: '', env: { KEEP: 'value' } });
  assert.equal(inherited.CLAUDE_CODE_DISABLE_BACKGROUND_TASKS, '0');
  assert.equal(options.env.SENTINEL, 'preserved');
  assert.equal(options.env.CLAUDE_CODE_DISABLE_BACKGROUND_TASKS, '1');
  assert.equal(options.env.CLAUDE_CODE_DISABLE_MCP_TASK_BACKGROUND, '1');
  assert.equal(options.settings.apiKeyHelper, '');
  assert.equal(options.settings.env.KEEP, 'value');
  assert.equal(options.settings.env.CLAUDE_CODE_DISABLE_BACKGROUND_TASKS, '1');
  assert.equal(options.hooks.PreToolUse[0].onFailure, 'block');
  assert.equal(options.hooks.Stop, review.Stop);
  assert.equal(options.hooks.PreToolUse.length, 2);
  for (const [tool, input] of attempts.slice(0, 7)) {
    assert.equal(backgroundDecision(tool, input).behavior, 'deny');
    assert.equal((await options.hooks.PreToolUse[0].hooks[0]({ tool_name: tool, tool_input: input }))
      .hookSpecificOutput.permissionDecision, 'deny');
  }
  for (const [tool, input] of attempts.slice(7)) {
    assert.equal(backgroundDecision(tool, input), null);
    assert.deepEqual(await options.hooks.PreToolUse[0].hooks[0]({ tool_name: tool, tool_input: input }), {});
  }
  await options.hooks.PreToolUse[1].hooks[0]({});
  assert.equal(reviewCalls, 1);
});

const fakeSdk = `
  export function query({prompt, options}) {
    console.log(JSON.stringify({method:'test.policy', params:{
      disabled:options.env?.CLAUDE_CODE_DISABLE_BACKGROUND_TASKS,
      mcpDisabled:options.env?.CLAUDE_CODE_DISABLE_MCP_TASK_BACKGROUND,
      disallowed:options.disallowedTools, resume:options.resume || '',
      hookCount:options.hooks?.PreToolUse?.length,
      settingsDisabled:options.settings?.env?.CLAUDE_CODE_DISABLE_BACKGROUND_TASKS,
      apiKeyHelper:options.settings?.apiKeyHelper,
    }}));
    return (async function* () {
      const first = await prompt[Symbol.asyncIterator]().next();
      const cases = JSON.parse(first.value.message.content);
      yield {type:'system', subtype:'init', session_id:'sdk-test'};
      const decisions = [];
      for (const [tool, input] of cases) {
        const hooked = [];
        for (const matcher of options.hooks.PreToolUse)
          for (const hook of matcher.hooks)
            hooked.push((await hook({tool_name:tool, tool_input:input})).hookSpecificOutput?.permissionDecision || 'neutral');
        // Exercise the independent callback even if a real CLI short-circuits on deny.
        decisions.push({tool, hooked, decision:await options.canUseTool(tool,input)});
      }
      console.log(JSON.stringify({method:'test.decisions', params:decisions}));
      yield {type:'result', result:'guarded', is_error:false};
    })();
  }
`;

async function fixture(t) {
  const dir = await mkdtemp(path.join(tmpdir(), 'kp-managed-background-'));
  for (const file of ['session-usage.mjs', 'authentication.mjs', 'index.mjs', 'transcript.mjs',
    'input.mjs', 'tasks.mjs', 'resume.mjs', 'session-delete.mjs', 'review-guard.mjs', 'background-policy.mjs', 'process-barrier.mjs'])
    await copyFile(new URL(`../../../apps/daemon/providers/claude-sidecar/${file}`, import.meta.url), path.join(dir, file));
  const sdk = path.join(dir, 'node_modules/@anthropic-ai/claude-agent-sdk');
  await mkdir(sdk, { recursive: true });
  await writeFile(path.join(sdk, 'package.json'), JSON.stringify({ type: 'module', main: 'index.mjs' }));
  await writeFile(path.join(sdk, 'index.mjs'), fakeSdk);
  const child = spawn(process.execPath, [path.join(dir, 'index.mjs')], { stdio: ['pipe', 'pipe', 'pipe'] });
  const events = []; let stderr = '';
  child.stderr.on('data', b => stderr += b);
  createInterface({ input: child.stdout }).on('line', line => events.push(JSON.parse(line)));
  t.after(async () => { child.kill(); await rm(dir, { recursive: true, force: true }); });
  const wait = async predicate => {
    for (let i = 0; i < 400; i++) {
      const event = events.find(predicate);
      if (event) return event;
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert.fail('Missing event: ' + stderr + JSON.stringify(events));
  };
  return { dir, events, wait, send: value => child.stdin.write(JSON.stringify(value) + '\n') };
}

for (const profile of ['', 'read', 'verify']) {
  test(`sidecar enforces native blocking before full-access/review approval: ${profile || 'normal'}`, async t => {
    const f = await fixture(t);
    f.send({ id: 1, method: 'turn/start', params: {
      sessionId: 's', cwd: f.dir, prompt: JSON.stringify(attempts), permission: 'danger-full-access',
      reviewGuard: Boolean(profile), reviewProfile: profile, scratchDirectory: f.dir,
      resume: 'resumed-session', mcpServers: { ketplus_agents: { command: '/bin/true' } },
      authentication: { enabled: true, key: 'sk-ant-test-fixture', revision: 'fixture' },
    } });
    const policy = (await f.wait(e => e.method === 'test.policy')).params;
    assert.equal(policy.disabled, '1');
    assert.equal(policy.mcpDisabled, '1');
    assert.equal(policy.settingsDisabled, '1');
    assert.equal(policy.apiKeyHelper, '');
    assert.deepEqual(policy.disallowed, ['Agent', 'Task', 'Workflow']);
    assert.equal(policy.resume, 'resumed-session');
    assert.equal(policy.hookCount, profile ? 2 : 1);
    const decisions = (await f.wait(e => e.method === 'test.decisions')).params;
    for (const decision of decisions.slice(0, 7)) {
      assert.equal(decision.hooked[0], 'deny');
      assert.equal(decision.decision.behavior, 'deny');
    }
    assert.equal(decisions[8].decision.behavior, profile ? 'deny' : 'allow');
    if (!profile) assert.deepEqual(decisions[8].decision.updatedInput, attempts[8][1]);
    assert.equal(decisions[9].decision.behavior, profile ? 'deny' : 'allow');
    assert.equal(decisions[10].decision.behavior, 'allow');
    if (!profile) assert.equal(decisions[7].decision.behavior, 'allow');
    await f.wait(e => e.params?.type === 'turn.completed');
    assert.equal(f.events.filter(e => e.method === 'permission.request').length, 0);
    assert.equal(f.events.filter(e => e.params?.type === 'tasks.changed' && e.params.payload.tasks.length).length, 0);
  });
}
