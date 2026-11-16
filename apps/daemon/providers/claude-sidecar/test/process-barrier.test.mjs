import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { ProcessBarrier, refreshWithin } from '../process-barrier.mjs';

test('owned-process barrier preserves bootstrap env/cwd and waits for actual exit', async () => {
  const barrier = new ProcessBarrier();
  const child = barrier.spawn({ command: process.execPath,
    args: ['-e', `process.stdout.write(process.env.FIXTURE_VALUE); process.stdin.resume();
      process.stdin.on('end', () => setTimeout(() => process.exit(0), 50));`],
    cwd: process.cwd(), env: { ...process.env, FIXTURE_VALUE: 'synthetic-bootstrap' },
  });
  try {
    const [value] = await once(child.stdout, 'data');
    assert.equal(value.toString(), 'synthetic-bootstrap');
    assert.equal(barrier.processes[0].exited, false);
    child.stdin.end();
    assert.equal(await barrier.drain(), true);
    assert.equal(barrier.processes[0].exited, true);
    assert.throws(() => barrier.spawn({}), /transport already closed/);
  } finally { child.kill('SIGKILL'); await barrier.drain(); }
});

test('a child ignoring input closure is killed and its exit confirmed before reuse', async () => {
  const barrier = new ProcessBarrier();
  const child = barrier.spawn({ command: process.execPath,
    args: ['-e', `setInterval(() => {}, 1000); process.stdout.write('ready');`],
    cwd: process.cwd(), env: process.env,
  });
  try {
    await once(child.stdout, 'data');
    child.stdin.end();
    assert.equal(await barrier.drain(10), true);
    assert.equal(barrier.processes[0].exited, true);
    assert.equal(child.signalCode, 'SIGKILL');
  } finally { child.kill('SIGKILL'); await barrier.drain(); }
});

test('context refresh timeout refuses an unacknowledged operation', async () => {
  await assert.rejects(refreshWithin(new Promise(() => {}), 10), /mutation_refresh_unavailable/);
});

test('unconfirmed close stays fenced after timeout and kill request', async () => {
  const barrier = new ProcessBarrier();
  let finish;
  let kills=0;
  const record={exited:false,child:{kill(){kills++;}},
    done:new Promise(resolve => {finish=resolve;})};
  barrier.processes.push(record); // Synthetic unresponsive close, no live workload.
  try {
    assert.equal(await barrier.drain(1),false);
    assert.equal(kills,1);
    assert.throws(() => barrier.spawn({}),/transport already closed/);
  } finally {record.exited=true;finish();await barrier.drain();}
});
