// Test-only passive handshake observer, loaded only into owned Cursor processes.
// Approval tuple/source stay unchanged. It never emits message bodies or secrets.
'use strict';
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const cp = require('node:child_process');
const crypto = require('node:crypto');
const {isMainThread, threadId} = require('node:worker_threads');
const config = JSON.parse(fs.readFileSync(process.env.T3_ATTEST_CONFIG, 'utf8'));
const original = cp.ChildProcess.prototype.spawn;
const canonical = value => fs.realpathSync(value);
const role = path.basename(process.argv[1] || '');
let sequence = 0;
const site = config.injectedSite;
if (site && crypto.createHash('sha256').update(fs.readFileSync(site.file)).digest('hex') !== site.sha256)
  throw new Error('Pinned injected-server callsite changed; no attestation');

function send(fields) {
  const socket = net.createConnection(config.socket);
  socket.on('error', () => {}); // Missing observer can never produce an ACK.
  socket.on('connect', () => socket.end(JSON.stringify({
    nonce: config.nonce, transportEpoch: config.transportEpoch,
    observerPid: process.pid, role, mainThread: isMainThread, threadId, sequence: ++sequence, ...fields
  }) + '\n'));
}

cp.ChildProcess.prototype.spawn = function(options) {
  const oldLimit = Error.stackTraceLimit;
  Error.stackTraceLimit = 24;
  const stack = new Error().stack;
  Error.stackTraceLimit = oldLimit;
  const result = original.call(this, options);
  let args = options.args || [];
  if (args.length) args = args.slice(1); // Node's spawn options include argv[0].
  let matches = false;
  try {
    matches = canonical(options.file) === canonical(config.command) &&
      JSON.stringify(args) === JSON.stringify(config.args) &&
      canonical(options.cwd || process.cwd()) === config.cwd;
  } catch (_) {}
  if (!matches || !this.stdin || !this.stdout) return result;
  const child = this;
  // Only private diagnostics receive the callsite. No argument/environment dump.
  const injected = Boolean(site && stack.includes(`${site.file}:${site.line}:${site.column}`));
  const base = {childPid: child.pid, launchDigest: config.launchDigest, injected};
  send({...base, event: 'spawn', callsite: stack.split('\n').slice(1, 24).join('\n')});
  let initId, listId, outbound = '', inbound = '';
  const write = child.stdin.write.bind(child.stdin);
  child.stdin.write = function(chunk, encoding, callback) {
    if (typeof encoding === 'function') { callback = encoding; encoding = undefined; }
    outbound += Buffer.isBuffer(chunk) ? chunk.toString('utf8') : String(chunk);
    if (outbound.length > 1024 * 1024) {
      send({...base, event: 'observer_overflow'}); child.kill('SIGTERM'); return false;
    }
    let newline;
    while ((newline = outbound.indexOf('\n')) >= 0) {
      const line = outbound.slice(0, newline); outbound = outbound.slice(newline + 1);
      let message;
      try { message = JSON.parse(line); }
      catch (_) { send({...base, event: 'invalid_frame'}); child.kill('SIGTERM'); return false; }
      if (message.method === 'tools/call') {
        // A text-only probe cannot execute even this static read-only tool.
        send({...base, event: 'forbidden_tool_call'}); child.kill('SIGTERM');
        if (callback) callback(new Error('Text-only fixture rejects tool calls'));
        return false;
      }
      if (message.method === 'initialize') {
        initId = message.id; send({...base, event: 'initialize_sent'});
      } else if (message.method === 'notifications/initialized') {
        send({...base, event: 'initialized_sent'});
      } else if (message.method === 'tools/list') {
        listId = message.id; send({...base, event: 'tools_list_sent'});
      }
      write(line + '\n', encoding);
    }
    if (callback) callback();
    return true;
  };
  child.stdout.on('data', chunk => {
    inbound += chunk.toString('utf8');
    if (inbound.length > 1024 * 1024) {
      send({...base, event: 'observer_overflow'}); child.kill('SIGTERM'); return;
    }
    let newline;
    while ((newline = inbound.indexOf('\n')) >= 0) {
      const line = inbound.slice(0, newline); inbound = inbound.slice(newline + 1);
      let message;
      try { message = JSON.parse(line); } catch (_) { continue; }
      if (initId !== undefined && message.id === initId && message.result && !message.error)
        send({...base, event: 'initialize_result'});
      if (listId !== undefined && message.id === listId && message.result && !message.error) {
        const tools = message.result.tools;
        const valid = Array.isArray(tools) && tools.length === 1 &&
          tools[0].name === 't3_probe_status' && tools[0].annotations?.readOnlyHint === true &&
          tools[0].annotations?.destructiveHint === false;
        send({...base, event: 'inventory', valid});
      }
    }
  });
  child.on('exit', () => send({...base, event: 'exit'}));
  return result;
};
