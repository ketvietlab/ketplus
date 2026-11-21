import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import { mkdtemp, mkdir, readFile, writeFile, copyFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const source = fileURLToPath(new URL('../../../apps/daemon/providers/claude-sidecar/', import.meta.url));
const sdk = String.raw`
import { appendFileSync } from 'node:fs';
import { once } from 'node:events';
const note = entry => appendFileSync(process.env.FIXTURE_LOG, JSON.stringify(entry)+'\n');
export function query({prompt,options}) {
  const epoch = options.mcpServers.ketplus_agents.env.KETPLUS_AGENT_TRANSPORT_EPOCH;
  note({event:'query',epoch,resume:options.resume || '',helper:options.settings.apiKeyHelper,
    disabled:options.env.CLAUDE_CODE_DISABLE_BACKGROUND_TASKS});
  const code = "const fs=require('node:fs');const note=event=>fs.appendFileSync(process.env.FIXTURE_LOG,JSON.stringify({event,epoch:process.env.FIXTURE_EPOCH})+'\\n');note('spawned');process.stdout.write('ready');process.stdin.resume();process.stdin.on('end',()=>setTimeout(()=>{note('exit');process.exit(0);},100));";
  const child = options.spawnClaudeCodeProcess({command:process.execPath,args:['-e',code],
    cwd:options.cwd,env:{...options.env,FIXTURE_LOG:process.env.FIXTURE_LOG,FIXTURE_EPOCH:epoch},
    signal:new AbortController().signal});
  const ready=once(child.stdout,'data');
  let closed=false;
  return {
    async initializationResult() { await ready; return {hook_on_failure_supported:true}; },
    async setMcpServers() {
      note({event:'mcp_ack',epoch});
      return {added:['ketplus_agents'],removed:[],errors:epoch==='mcp-fail'?{ketplus_agents:'fixture'}:{}};
    },
    async mcpServerStatus() { return [{name:'ketplus_agents',status:'connected'}]; },
    close() { if (!closed) { closed=true;child.stdin.end(); }
      if (epoch==='close-fail') throw new Error('Fixture close failed'); },
    async *[Symbol.asyncIterator]() {
      for await (const message of prompt) {
        note({event:'prompt',epoch});
        yield {type:'system',subtype:'init',session_id:'fixture-history'};
        yield {type:'stream_event',event:{type:'message_start',message:{model:'fixture-model'}}};
        yield {type:'assistant',session_id:'fixture-history',message:{id:'answer',content:[{type:'text',text:'Fixture answer'}]}};
        yield {type:'result',subtype:'success',session_id:'fixture-history',user_message_uuid:message.uuid,result:'Fixture answer'};
      }
    },
  };
}
`;

async function fixture() {
  const directory = await mkdtemp(path.join(tmpdir(), 'kp-claude-origin-'));
  for (const name of ['index.mjs', 'authentication.mjs', 'transcript.mjs', 'input.mjs', 'tasks.mjs',
    'resume.mjs', 'session-delete.mjs', 'session-usage.mjs', 'review-guard.mjs',
    'background-policy.mjs', 'process-barrier.mjs'])
    await copyFile(path.join(source, name), path.join(directory, name));
  const pkg = path.join(directory, 'node_modules/@anthropic-ai/claude-agent-sdk');
  await mkdir(pkg, { recursive: true });
  await writeFile(path.join(pkg, 'package.json'), JSON.stringify({type:'module',exports:'./sdk.mjs'}));
  await writeFile(path.join(pkg, 'sdk.mjs'), sdk);
  const log = path.join(directory, 'events.jsonl');
  const process = spawn(globalThis.process.execPath, [path.join(directory, 'index.mjs')], {
    env: {...globalThis.process.env, FIXTURE_LOG:log}, stdio:['pipe','pipe','pipe'],
  });
  const messages=[];
  createInterface({input:process.stdout}).on('line', line => messages.push(JSON.parse(line)));
  let errors='';process.stderr.on('data', value => { errors+=value; });
  const until = async predicate => {
    const deadline=Date.now()+5000;
    while (!messages.some(predicate) && Date.now()<deadline)
      await new Promise(resolve => setTimeout(resolve,10));
    assert.ok(messages.some(predicate), 'sidecar response missing: '+errors);
    return messages.find(predicate);
  };
  return {directory,log,process,messages,until,
    send(id,epoch,resume='') {
      process.stdin.write(JSON.stringify({id,method:'turn/start',params:{sessionId:'owner',
        cwd:directory,prompt:'fixture',resume,managedTransportEpoch:epoch,
        authentication:{enabled:true,key:'sk-ant-test-fixture'},
        mcpServers:{ketplus_agents:{type:'stdio',command:'fixture',
          env:{KETPLUS_AGENT_TRANSPORT_EPOCH:epoch}}}}})+'\n');
    },
    async close() {
      process.stdin.end();
      if (process.exitCode===null) {
        process.kill('SIGTERM');
        await once(process,'exit');
      }
      await rm(directory,{recursive:true,force:true});
    },
  };
}

test('new Claude epoch waits for old process exit, refresh ACK precedes prompt, and history/auth survive', async () => {
  const f=await fixture();
  try {
    f.send(1,'first');
    const first=await f.until(message => message.id===1);
    assert.ok(first.result, JSON.stringify(f.messages));
    assert.equal(first.result.managedTransportEpoch,'first');
    await f.until(message => message.params?.type==='turn.completed');
    f.send(2,'second','fixture-history');
    const second=await f.until(message => message.id===2);
    assert.equal(second.result.managedTransportEpoch,'second');
    const deadline=Date.now()+5000;
    let log=[];
    while (Date.now()<deadline) {
      log=(await readFile(f.log,'utf8')).trim().split('\n').map(JSON.parse);
      if (log.some(e => e.event==='exit' && e.epoch==='second')) break;
      await new Promise(resolve => setTimeout(resolve,10));
    }
    const index=(event,epoch)=>log.findIndex(e => e.event===event && e.epoch===epoch);
    assert.ok(index('exit','first')>=0 && index('exit','first')<index('query','second'));
    for (const epoch of ['first','second'])
      assert.ok(index('mcp_ack',epoch)>=0 && index('mcp_ack',epoch)<index('prompt',epoch));
    assert.equal(log.find(e => e.event==='query' && e.epoch==='second').resume,'fixture-history');
    for (const query of log.filter(e => e.event==='query')) {
      assert.equal(query.helper,'');assert.equal(query.disabled,'1');
    }
    assert.ok(index('exit','second')>=0);
  } finally { await f.close(); }
});

test('failed Claude MCP refresh never acknowledges mutation or releases the model prompt', async () => {
  const f=await fixture();
  try {
    f.send(1,'mcp-fail');
    const response=await f.until(message => message.id===1);
    assert.match(response.error.message,/mutation_refresh_unavailable/);
    await new Promise(resolve => setTimeout(resolve,200));
    const log=(await readFile(f.log,'utf8')).trim().split('\n').map(JSON.parse);
    assert.equal(log.some(e => e.event==='prompt'),false);
    assert.ok(log.some(e => e.event==='exit'));
  } finally { await f.close(); }
});

test('throwing SDK close cannot bypass the actual process exit barrier', async () => {
  const f=await fixture();
  try {
    f.send(1,'close-fail');
    assert.ok((await f.until(message => message.id===1)).result);
    await f.until(message => message.params?.type==='turn.completed');
    f.send(2,'replacement','fixture-history');
    assert.ok((await f.until(message => message.id===2)).result);
    const log=(await readFile(f.log,'utf8')).trim().split('\n').map(JSON.parse);
    const exited=log.findIndex(e => e.event==='exit' && e.epoch==='close-fail');
    const replaced=log.findIndex(e => e.event==='query' && e.epoch==='replacement');
    assert.ok(exited>=0 && replaced>exited);
  } finally { await f.close(); }
});
