import { spawn } from 'node:child_process';

// Uses the SDK's public spawnClaudeCodeProcess hook. An ACK/kill request is not
// proof of closure: only the owned child's actual exit settles this barrier.
export class ProcessBarrier {
  constructor() { this.processes = []; this.closing = false; }
  spawn = options => {
    if (this.closing) throw new Error('mutation_refresh_unavailable: Claude transport already closed.');
    const child = spawn(options.command, options.args, {
      cwd: options.cwd, env: options.env, signal: options.signal,
      stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true,
    });
    child.stderr.resume();
    const record = { child, exited: false };
    record.done = new Promise(resolve => {
      const exited = () => { record.exited = true; resolve(); };
      child.once('exit', exited);
      child.once('error', () => { if (!child.pid) exited(); });
    });
    this.processes.push(record);
    return child;
  };
  get spawned() { return this.processes.length > 0; }
  async drain(timeoutMs = 5000) {
    this.closing = true;
    const wait = async duration => {
      let timer;
      try {
        return await Promise.race([
          Promise.all(this.processes.map(record => record.done)).then(() => true),
          new Promise(resolve => { timer = setTimeout(() => resolve(false), duration); }),
        ]);
      } finally { clearTimeout(timer); }
    };
    if (await wait(timeoutMs)) return true;
    for (const { child, exited } of this.processes)
      if (!exited) child.kill('SIGKILL');
    return wait(1000);
  }
}

export async function refreshWithin(operation, timeoutMs = 15000) {
  let timer;
  try {
    return await Promise.race([operation,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('mutation_refresh_unavailable: Claude context refresh timed out.')), timeoutMs);
      }),
    ]);
  } finally { clearTimeout(timer); }
}
