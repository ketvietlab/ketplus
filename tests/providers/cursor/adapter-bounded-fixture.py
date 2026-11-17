#!/usr/bin/env python3
"""Bounded production-adapter observer; only its first authorized prompt can pass.

Epoch two intercepts the prompt AFTER the real load/model ACK. No provider
history or ACK is fabricated. Only this newly created session's marker presence
is recorded, never message contents or authentication material.
"""
import fcntl
import json
import os
import pathlib
import signal
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True
MODEL = "claude-haiku-5-5-low"
MARKER = "T3_BOUNDED_CURSOR_ALPHA"


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def main():
    config = json.loads(pathlib.Path(sys.argv[1]).read_text())
    args = sys.argv[2:]
    if "acp" not in args:
        os.execv(config["executable"], [config["executable"], *args])
    log = pathlib.Path(config["log"])
    prior = rows(log)
    epoch = "two" if any(row["event"] == "provider" for row in prior) else "one"
    lock = threading.Lock()

    def record(event, **fields):
        with lock, log.open("a") as output:
            output.write(json.dumps(dict(event=event, pid=os.getpid(), epoch=epoch, **fields)) + "\n")

    def emit(value):
        with lock:
            print(json.dumps(value), flush=True)

    prior_workers = {row["pid"] for row in prior if row["event"] == "mcp"}
    process = subprocess.Popen([config["executable"], *args], cwd=config["work"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True)
    record("provider", child_pid=process.pid,
           prior_mcp_alive=sum(alive(pid) for pid in prior_workers))
    pending = {}
    selected = False
    replaying = False

    def read():
        nonlocal selected, replaying
        for line in process.stdout:
            try:
                value = json.loads(line)
            except ValueError:
                continue
            request = pending.pop(str(value.get("id")), None) if "method" not in value else None
            if request:
                method, params = request
                if method == "session/set_config_option" and params.get("configId") == "model":
                    for option in value.get("result", {}).get("configOptions", []):
                        current = option.get("currentValue", "")
                        if option.get("id") == "model":
                            selected = current.startswith("claude-haiku-5-5") and "fast=true" not in current
                            record("cheap_model_ack", accepted=selected)
                if method == "session/load":
                    record("load_ack", success="error" not in value,
                           same_id=value.get("result", {}).get("sessionId") == params.get("sessionId"))
                    replaying = False
            if value.get("method") == "session/update" and replaying:
                update = value.get("params", {}).get("update", {})
                if MARKER in update.get("content", {}).get("text", ""):
                    record("history_marker", kind=update.get("sessionUpdate"))
            emit(value)

    thread = threading.Thread(target=read, daemon=True)
    thread.start()

    def stop(*_):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stop)
    try:
        for line in sys.stdin:
            value = json.loads(line)
            method = value.get("method")
            params = value.get("params", {})
            if method == "session/prompt":
                # The adapter adds host context, so inspect only for our exact
                # synthetic marker. The second prompt is never sent to a model.
                valid = MARKER in json.dumps(params.get("prompt", []))
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline:
                    ready = any(row["event"] == "tools_list" and row.get("epoch") == epoch
                                for row in rows(log))
                    if ready:
                        break
                    time.sleep(.05)
                record("prompt_barrier", mcp_ready=ready, cheap_model_ack=selected)
                if not ready or not selected or not valid:
                    emit(dict(jsonrpc="2.0", id=value["id"],
                              error=dict(code=-32000, message="Bounded control/MCP/model barrier failed; no model sent.")))
                    continue
                if epoch == "two" or config["mode"] == "control":
                    record("prompt_intercepted", model_request_sent=False)
                    emit(dict(jsonrpc="2.0", id=value["id"], result=dict(stopReason="cancelled")))
                    continue
                ledger = pathlib.Path(config["ledger"])
                with open(str(ledger) + ".lock", "w") as budget_lock:
                    fcntl.flock(budget_lock, fcntl.LOCK_EX)
                    budget = json.loads(ledger.read_text())
                    assert budget["decision"] == "D-3" and budget["maximum_per_provider"] == 1
                    assert budget["owner_conversation"] == config["owner"] and not budget["retry_allowed"]
                    assert config["head"]
                    assert not budget["recall_allowed"] and budget["cursor"]["sent_or_uncertain"] == 0
                    budget["cursor"].update(sent_or_uncertain=1, model=MODEL, head=config["head"],
                                             auth_route="inherited-provider-bootstrap", status="reserved_before_delivery")
                    temporary = ledger.with_suffix(".pending")
                    with temporary.open("w", opener=lambda path, flags: os.open(path, flags, 0o600)) as output:
                        json.dump(budget, output, indent=2)
                        output.flush()
                        os.fsync(output.fileno())
                    temporary.replace(ledger)
                record("model_request_forwarded")
            if "id" in value:
                pending[str(value["id"])] = (method, params)
            if method == "session/load":
                replaying = True
            process.stdin.write(json.dumps(value) + "\n")
            process.stdin.flush()
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(3)
        thread.join(1)
        record("provider_exit", child_exited=process.poll() is not None)


if __name__ == "__main__":
    main()
