import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, readFile, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { backgroundOptions } from '../../../apps/daemon/providers/claude-sidecar/background-policy.mjs';

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
