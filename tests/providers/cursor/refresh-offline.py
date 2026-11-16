#!/usr/bin/env python3
"""Opt-in ACP control probe. Never sends authentication, prompts or tool calls."""
import argparse
import json
import pathlib
import queue
import subprocess
import tempfile
import threading


class Peer:
    def __init__(self, command):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, text=True)
        self.responses = queue.Queue()
        self.next_id = 0

        def read():
            for line in self.process.stdout:
                try:
                    self.responses.put(json.loads(line))
                except json.JSONDecodeError:
                    continue
            self.responses.put(None)

        threading.Thread(target=read, daemon=True).start()

    def call(self, method, params):
        self.next_id += 1
        request_id = self.next_id
        self.process.stdin.write(json.dumps(dict(jsonrpc="2.0", id=request_id,
                                                  method=method, params=params)) + "\n")
        self.process.stdin.flush()
        while True:
            response = self.responses.get(timeout=12)
            if response is None:
                raise RuntimeError("ACP process exited before its control response")
            if response.get("id") == request_id:
                return response

    def initialize(self):
        return self.call("initialize", dict(protocolVersion=1, clientCapabilities={},
                         clientInfo=dict(name="ketplus-capability-probe", version="offline")))

    def close(self):
        if self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=3)


def report(step, response, session_id=""):
    error = response.get("error", {})
    # Only control errors; do not print full session results or history updates.
    message = str(error.get("message", "")).replace(session_id, "[fixture-session]") if session_id else str(error.get("message", ""))
    print(json.dumps(dict(step=step, ok="result" in response, error_code=error.get("code"),
                          error_message=message[:160])), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, help="Already installed Cursor CLI")
    args = parser.parse_args()
    peers = []
    try:
        version = subprocess.run([args.executable, "--version"], capture_output=True,
                                 text=True, timeout=5, check=True).stdout.strip()
        print(json.dumps(dict(version=version[:100])), flush=True)
        with tempfile.TemporaryDirectory(prefix="kp-acp-refresh-") as folder:
            cwd = str(pathlib.Path(folder).resolve())
            first = Peer([args.executable, "acp"])
            peers.append(first)
            initialized = first.initialize()
            report("initialize", initialized)
            print(json.dumps(dict(agentCapabilities=initialized.get("result", {}).get("agentCapabilities"))), flush=True)
            created = first.call("session/new", dict(cwd=cwd, mcpServers=[]))
            report("new_canonical_cwd", created)
            session_id = created.get("result", {}).get("sessionId")
            if not session_id:
                return 1
            params = dict(sessionId=session_id, cwd=cwd, mcpServers=[])
            report("same_process_load", first.call("session/load", params), session_id)
            first.close()
            print(json.dumps(dict(step="old_process_exit", confirmed=first.process.poll() is not None)), flush=True)
            second = Peer([args.executable, "acp"])
            peers.append(second)
            report("replacement_initialize", second.initialize())
            report("cross_process_load", second.call("session/load", params), session_id)
            return 0
    finally:
        for peer in peers:
            peer.close()


if __name__ == "__main__":
    raise SystemExit(main())
