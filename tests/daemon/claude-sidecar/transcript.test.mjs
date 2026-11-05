import test from 'node:test';
import assert from 'node:assert/strict';
import { ClaudeTranscript } from '../../../apps/daemon/providers/claude-sidecar/transcript.mjs';

test('stream snapshots repair text and preserve separate assistant steps', () => {
  const events = [];
  const t = new ClaudeTranscript('turn', (type, payload) => events.push({ type, ...structuredClone(payload) }));
  t.accept({ type: 'stream_event', event: { type: 'message_start', message: { id: 'm1' } } });
  t.accept({ type: 'stream_event', event: { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } } });
  t.accept({ type: 'stream_event', event: { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'Partial' } } });
  t.accept({ type: 'assistant', message: { id: 'm1', content: [{ type: 'text', text: 'Full progress' }] } });
  t.accept({ type: 'assistant', message: { id: 'm2', content: [{ type: 'text', text: 'Answer' }] } });
  t.accept({ type: 'result', result: 'Answer', is_error: false });
  assert.equal(t.items.size, 2);
  assert.equal(t.items.get('m1-0').text, 'Full progress');
  assert.equal(t.items.get('m1-0').phase, 'commentary');
  assert.equal(t.items.get('m2-0').phase, 'final_answer');
  assert.ok(events.some(e => e.type === 'assistant.delta' && e.delta === 'Partial'));
});

test('tool inputs, error results, compaction and cancellation are retained', () => {
  const events = [];
  const t = new ClaudeTranscript('turn', (type, payload) => events.push({ type, ...structuredClone(payload) }));
  t.accept({ type: 'assistant', message: { id: 'm', content: [
    { type: 'tool_use', id: 'bash', name: 'Bash', input: { command: 'false' } },
    { type: 'tool_use', id: 'read', name: 'Read', input: { file_path: 'a.txt' } },
    { type: 'thinking', thinking: 'private' },
  ] } });
  t.accept({ type: 'user', message: { content: [{ type: 'tool_result', tool_use_id: 'bash', is_error: true, content: 'failure' }] } });
  t.accept({ type: 'system', subtype: 'compact_boundary' });
  t.finish(true);
  assert.equal(t.items.get('bash').aggregatedOutput, 'failure');
  assert.equal(t.items.get('bash').status, 'failed');
  assert.equal(t.items.get('read').arguments.file_path, 'a.txt');
  assert.equal(t.items.get('read').status, 'interrupted');
  assert.ok(events.some(e => e.item?.type === 'contextCompaction'));
  assert.ok(!JSON.stringify(events).includes('private'));
});
