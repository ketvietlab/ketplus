#!/usr/bin/env python3
"""Opt-in control-only Codex probe; no model turns or existing history reads."""
import argparse
import json
import os
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time


class Peer:
    def __init__(self, executable):
        self.process = subprocess.Popen([executable, "app-server", "-c", "mcp_servers={}"],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, text=True)
        self.messages = queue.Queue()
        self.next_id = 0

        def read():
            for line in self.process.stdout:
                try:
                    self.messages.put(json.loads(line))
                except json.JSONDecodeError:
                    pass
            self.messages.put(None)
        threading.Thread(target=read, daemon=True).start()

    def call(self, method, params):
        self.next_id += 1
        request_id = self.next_id
        self.send(dict(id=request_id, method=method, params=params))
        deadline = time.monotonic() + 12
        while True:
            message = self.messages.get(timeout=max(0.01, deadline-time.monotonic()))
            if message is None:
                raise RuntimeError("Owned app-server exited before control response")
            if message.get("id") == request_id:
                return message

    def send(self, message):
        self.process.stdin.write(json.dumps(message)+"\n")
        self.process.stdin.flush()

    def initialize(self):
        result = self.call("initialize", dict(clientInfo=dict(name="ketplus-offline-probe",
                           version="offline"), capabilities=dict(experimentalApi=True)))
        self.send(dict(method="initialized", params={}))
        return result

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)


def report(step, result, redactions=()):
    error = result.get("error", {})
    message = str(error.get("message", ""))
    for value in redactions:
        message = message.replace(value, "[fixture]")
    # Never print provider content, thread IDs, auth configuration or local paths.
    print(json.dumps(dict(step=step, ok="result" in result,
                          error_code=error.get("code"), error_message=message[:180])), flush=True)


def status(peer, thread):
    reply = peer.call("mcpServerStatus/list", dict(threadId=thread, limit=100))
    servers = reply.get("result", {}).get("data", [])
    ready = any(server.get("name") == "ketplus_agents" and
                "background_task_start" in server.get("tools", {}) for server in servers)
    return reply, ready


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, help="Already installed Codex CLI")
    args = parser.parse_args()
    peers = []
    try:
        version = subprocess.run([args.executable, "--version"], capture_output=True,
                                 text=True, timeout=5, check=True).stdout.strip()
        print(json.dumps(dict(version=version[:100])), flush=True)
        with tempfile.TemporaryDirectory(prefix="kp-codex-refresh-") as folder:
            root = pathlib.Path(folder).resolve()
            worker = root / "mcp.py"
            worker.write_text(MCP_WORKER)
            config = {"mcp_servers.ketplus_agents.command": sys.executable,
                      "mcp_servers.ketplus_agents.args": [str(worker)],
                      "mcp_servers.ketplus_agents.env": {"FIXTURE_EPOCH": "first",
                          "FIXTURE_PIDS": str(root / "worker-pids")}}
            params = dict(cwd=str(root), approvalPolicy="never", sandbox="read-only",
                          config=config)
            first = Peer(args.executable)
            peers.append(first)
            report("initialize", first.initialize())
            created = first.call("thread/start", params)
            report("empty_thread_start", created)
            thread = created.get("result", {}).get("thread", {}).get("id")
            if not thread:
                return 1
            reply, ready = status(first, thread)
            report("first_mcp_status", reply)
            print(json.dumps(dict(step="first_mcp_discovery", ready=ready)), flush=True)
            # This is a deliberately empty thread. No turn/start is sent.
            same = first.call("thread/resume", dict(params, threadId=thread))
            report("same_process_empty_resume", same, (thread, str(root)))
            first.close()
            pids = [int(pid) for pid in (root / "worker-pids").read_text().splitlines()]
            deadline = time.monotonic() + 3
            while any(alive(pid) for pid in pids) and time.monotonic() < deadline:
                time.sleep(0.05)
            print(json.dumps(dict(step="old_process_exit", confirmed=first.process.poll() is not None,
                                  mcp_workers_exited=not any(alive(pid) for pid in pids))), flush=True)
            config["mcp_servers.ketplus_agents.env"] = {"FIXTURE_EPOCH": "second", "FIXTURE_PIDS": str(root / "worker-pids")}
            second = Peer(args.executable)
            peers.append(second)
            report("replacement_initialize", second.initialize())
            loaded = second.call("thread/resume", dict(params, threadId=thread))
            report("cross_process_empty_resume", loaded, (thread, str(root)))
            if "result" in loaded:
                restored = loaded["result"].get("thread", {}).get("id") == thread
                reply, ready = status(second, thread)
                report("replacement_mcp_status", reply)
                print(json.dumps(dict(step="restored_context", same_thread=restored,
                                      mcp_ready=ready)), flush=True)
            return 0
    finally:
        for peer in peers:
            peer.close()


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


MCP_WORKER = r'''import json,os,sys,threading
with open(os.environ["FIXTURE_PIDS"],"a") as f: f.write(str(os.getpid())+"\n")
# Bound even a broken provider's synthetic worker; no persistent workload.
timer=threading.Timer(30,lambda:os._exit(0));timer.daemon=True;timer.start()
for line in sys.stdin:
    msg=json.loads(line); method=msg.get("method"); id=msg.get("id")
    if id is None: continue
    if method=="initialize":
        result=dict(protocolVersion="2024-11-05",capabilities=dict(tools={}),
                    serverInfo=dict(name="ketplus-offline-fixture",version="1"))
    elif method=="tools/list":
        result=dict(tools=[dict(name=name,description="Offline fixture",inputSchema=dict(type="object"))
                    for name in ("agents_whoami","background_task_start","background_task_stop")])
    else: result={}
    print(json.dumps(dict(jsonrpc="2.0",id=id,result=result)),flush=True)
'''

if __name__ == "__main__":
    raise SystemExit(main())
