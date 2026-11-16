#!/usr/bin/env python3
"""Opt-in Copilot 1.0.112 offline ACP/MCP control; never authorizes inference.

The informational /mcp list command is documented as a non-model ACP command.
Offline mode and a rejecting loopback provider fence it independently. No user
profile, auth cache, environment credential, model prompt, or tool call is used.
"""
import argparse
import http.server
import json
import os
import pathlib
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time


def emit(**fields):
    print(json.dumps(fields), flush=True)


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def worker():
    log = pathlib.Path(os.environ["T3_CONTROL_LOG"])

    def record(event):
        with log.open("a") as output:
            output.write(json.dumps(dict(event=event, pid=os.getpid())) + "\n")

    timeout = threading.Timer(45, lambda: os._exit(0))
    timeout.daemon = True
    timeout.start()
    record("spawn")
    for line in sys.stdin:
        message = json.loads(line)
        request = message.get("id")
        if request is None:
            continue
        method = message.get("method")
        record(method)
        if method == "initialize":
            result = dict(protocolVersion="2024-11-05", capabilities=dict(tools={}),
                          serverInfo=dict(name="t3-control", version="1"))
        elif method == "tools/list":
            result = dict(tools=[dict(name="t3_control_one", description="Discovery only",
                                     inputSchema=dict(type="object", properties={}))])
        else:
            raise RuntimeError("No tool invocation is allowed")
        emit(jsonrpc="2.0", id=request, result=result)


class Peer:
    def __init__(self, executable, environment, work, mcp_config=None):
        arguments = [executable, "--acp", "--no-auto-update", "--disable-builtin-mcps",
                     "--disable-client-telemetry"]
        if mcp_config:
            arguments += ["--additional-mcp-config", json.dumps(mcp_config)]
        self.process = subprocess.Popen(
            arguments, env=environment, cwd=work,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.messages = queue.Queue()
        self.serial = 0
        self.updates = []

        def read():
            for line in self.process.stdout:
                try:
                    self.messages.put(json.loads(line))
                except ValueError:
                    pass
            self.messages.put(None)

        threading.Thread(target=read, daemon=True).start()

    def call(self, method, params):
        assert method in ("initialize", "session/new", "session/prompt")
        if method == "session/prompt":
            assert params["prompt"] == [dict(type="text", text="/mcp list")]
        self.serial += 1
        self.process.stdin.write(json.dumps(dict(jsonrpc="2.0", id=self.serial,
                                                method=method, params=params)) + "\n")
        self.process.stdin.flush()
        deadline = time.monotonic() + 15
        while True:
            message = self.messages.get(timeout=max(.01, deadline - time.monotonic()))
            if message is None:
                raise RuntimeError("Provider exited before the control response")
            if message.get("method") == "session/update":
                self.updates.append(message.get("params", {}).get("update", {}))
            if message.get("id") == self.serial:
                return message

    def close(self):
        if self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(3)


def probe(executable, source):
    requests = []

    class Reject(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append("GET")
            self.send_error(503)

        def do_POST(self):
            requests.append("POST")
            self.send_error(503)

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Reject)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    peer = None
    discovered = False
    cleaned = False
    with tempfile.TemporaryDirectory(prefix="ketplus-copilot-control-") as temporary:
        root = pathlib.Path(temporary).resolve()
        log = root / "workers.jsonl"
        try:
            for name in ("home", "profile", "cache", "work"):
                (root / name).mkdir()
            environment = {key: value for key, value in os.environ.items()
                           if key in ("PATH", "TMPDIR", "LANG", "LC_ALL", "SYSTEMROOT")}
            environment.update(
                HOME=str(root / "home"), COPILOT_HOME=str(root / "profile"),
                COPILOT_CACHE_HOME=str(root / "cache"), COPILOT_OFFLINE="true",
                COPILOT_PROVIDER_BASE_URL=f"http://127.0.0.1:{server.server_port}/v1",
                COPILOT_MODEL="gpt-4.1", COPILOT_AUTO_UPDATE="false",
                COPILOT_TELEMETRY_DISABLED="true")
            version = subprocess.run([executable, "--version"], env=environment, cwd=root / "work",
                                     capture_output=True, text=True, timeout=10, check=True)
            if version.stdout.splitlines()[0].strip() != "GitHub Copilot CLI 1.0.112.":
                raise RuntimeError("This control is pinned to Copilot CLI 1.0.112")
            command = sys.executable
            arguments = [str(pathlib.Path(__file__).resolve()), "--mcp-worker"]
            mcp_config = dict(mcpServers=dict(t3_control=dict(
                command=command, args=arguments, env=dict(T3_CONTROL_LOG=str(log)), tools=["*"])))
            peer = Peer(executable, environment, root / "work",
                        mcp_config if source == "launch" else None)
            initialized = peer.call("initialize", dict(
                protocolVersion=1, clientCapabilities={},
                clientInfo=dict(name="ketplus-t3-control", version="1")))
            emit(step="initialize", ok="result" in initialized, capabilities=initialized.get(
                "result", {}).get("agentCapabilities", {}))
            servers = [dict(name="t3_control", command=command, args=arguments,
                            env=[dict(name="T3_CONTROL_LOG", value=str(log))])]
            new = peer.call("session/new", dict(cwd=str(root / "work"),
                                                mcpServers=servers if source == "request" else []))
            session = new.get("result", {}).get("sessionId")
            if session:
                status = peer.call("session/prompt", dict(
                    sessionId=session, prompt=[dict(type="text", text="/mcp list")]))
                emit(step="informational_mcp_list", ok="result" in status,
                     error_code=status.get("error", {}).get("code"),
                     stop_reason=status.get("result", {}).get("stopReason"),
                     server_named_in_output="t3_control" in json.dumps(peer.updates),
                     no_servers_in_output="no mcp servers" in json.dumps(peer.updates).lower())
            deadline = time.monotonic() + 5
            while True:
                entries = ([json.loads(line) for line in log.read_text().splitlines()]
                           if log.exists() else [])
                if any(entry["event"] == "tools/list" for entry in entries):
                    break
                if time.monotonic() >= deadline:
                    break
                time.sleep(.05)
            discovered = any(entry["event"] == "tools/list" for entry in entries)
            emit(step="new", mcp_source=source, observation_seconds=5,
                 ok="result" in new, error_code=new.get("error", {}).get("code"),
                 session_created=bool(session), mcp_spawned=bool(entries),
                 mcp_initialized=any(entry["event"] == "initialize" for entry in entries),
                 mcp_tools_listed=discovered, model_turns_sent=0,
                 local_endpoint_requests=len(requests))
        finally:
            if peer:
                peer.close()
            entries = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
            workers = {entry["pid"] for entry in entries}
            deadline = time.monotonic() + 3
            while any(alive(pid) for pid in workers) and time.monotonic() < deadline:
                time.sleep(.05)
            remaining = [pid for pid in workers if alive(pid)]
            for pid in remaining:
                os.kill(pid, signal.SIGTERM)
            deadline = time.monotonic() + 2
            while any(alive(pid) for pid in remaining) and time.monotonic() < deadline:
                time.sleep(.05)
            emit(step="cleanup", provider_exited=peer is None or peer.process.poll() is not None,
                 workers_requiring_cleanup=len(remaining),
                 remaining_workers=sum(alive(pid) for pid in workers), model_turns_sent=0)
            cleaned = not any(alive(pid) for pid in workers)
            server.shutdown()
    # Missing discovery is a failed capability control, never authorization to prompt.
    return 0 if discovered and not requests and cleaned else 2


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable")
    parser.add_argument("--mcp-worker", action="store_true")
    parser.add_argument("--mcp-source", choices=("request", "launch"), default="request")
    options = parser.parse_args()
    if options.mcp_worker:
        worker()
    elif options.executable:
        sys.exit(probe(str(pathlib.Path(options.executable).resolve()), options.mcp_source))
    else:
        parser.error("an explicit pinned executable is required")
