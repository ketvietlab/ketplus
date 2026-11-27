// Mock Node networking only: no provider, sockets, auth or model requests.
'use strict';
const fs = require('node:fs');
const vm = require('node:vm');
const {EventEmitter} = require('node:events');
const source = fs.readFileSync(require('node:path').join(__dirname, 'attestation-preload.cjs'), 'utf8');

function observer() {
  const sockets = [], delivered = [];
  const config = {socket: 'fixture', nonce: 'fixture', transportEpoch: 'fixture',
    launchDigest: 'fixture', command: 'fixture', args: [], cwd: 'fixture'};
  class ChildProcess extends EventEmitter {
    constructor() {
      super(); this.pid = 456;
      this.stdin = new EventEmitter(); this.stdin.write = () => true;
      this.stdout = new EventEmitter();
    }
    spawn() {}
  }
  const context = {
    require: name => name === 'node:fs' ? {
      readFileSync: () => JSON.stringify(config), realpathSync: value => value
    } : name === 'node:net' ? {createConnection: () => {
      const socket = new EventEmitter();
      socket.end = payload => delivered.push(JSON.parse(payload));
      sockets.push(socket); return socket;
    }} : name === 'node:child_process' ? {ChildProcess} : require(name),
    process: {env: {T3_ATTEST_CONFIG: 'fixture'}, pid: 123, argv: ['node', 'index.js']}, Buffer, Error
  };
  vm.createContext(context);
  vm.runInContext(source, context);
  return {context, sockets, delivered, ChildProcess};
}

function reordered(events) {
  const fixture = observer();
  for (const event of events) fixture.context.send({event, childPid: 456, injected: true});
  // Deliver an invalid observation order as apparently valid handshake stages.
  for (const index of [0, 2, 1, 3]) fixture.sockets[index].emit('connect');
  return fixture.delivered;
}

const immutable = observer();
const fields = {event: 'spawn', childPid: 456, sequence: 999, observerPid: 666,
  nested: {value: 'observed'}};
immutable.context.send(fields);
fields.event = 'initialized_sent'; fields.nested.value = 'mutated';
immutable.sockets[0].emit('connect');

const lifecycle = observer();
const child = new lifecycle.ChildProcess();
child.spawn({file: 'fixture', args: ['fixture'], cwd: 'fixture'});
child.emit('exit'); child.emit('close'); child.emit('error', new Error('must not be logged'));
child.stdout.emit('end'); child.stdout.emit('close'); child.stdout.emit('error', new Error('private'));
child.stdin.emit('close'); child.stdin.emit('error', new Error('private'));
for (const socket of lifecycle.sockets) socket.emit('connect');

console.log(JSON.stringify({
  invalid: reordered(['spawn', 'initialize_result', 'initialize_sent', 'initialized_sent']),
  valid: reordered(['spawn', 'initialize_sent', 'initialize_result', 'initialized_sent']),
  immutable: immutable.delivered[0], lifecycle: lifecycle.delivered.map(row => row.event)
}));
