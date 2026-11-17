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
import re
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
    provider = config.get("provider", "cursor")
    copilot = provider == "copilot"
    model = "auto" if copilot else MODEL
    marker = "T3_BOUNDED_COPILOT_ALPHA" if copilot else MARKER
    if "acp" not in args and "--acp" not in args:
        os.execv(config["executable"], [config["executable"], *args])
    if copilot:
        # Test-process settings only. No default model/configuration is changed.
        args += ["--model", "auto", "--no-auto-update", "--disable-builtin-mcps",
                 "--disable-client-telemetry", "--available-tools=t3_no_model_tools_bounded_probe"]
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
    prior_native = {row["child_pid"] for row in prior if row["event"] == "provider"}
    native_alive = sum(alive(pid) for pid in prior_native)
    if native_alive:
        # Observe and fail; never wait here and hide an adapter closure race.
        record("closure_barrier_failed", prior_native_alive=native_alive)
        raise RuntimeError("Previous owned native provider has not exited")
    process = subprocess.Popen([config["executable"], *args], cwd=config["work"],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True)
    record("provider", child_pid=process.pid,
           prior_mcp_alive=sum(alive(pid) for pid in prior_workers), prior_native_alive=native_alive)
    pending = {}
    selected = False
    replaying = False
    replay_text = ""
    auto_control = threading.Event()
    auto_pending = False
    auto_text = ""
    auto_success = False
    auto_config_update = False

    def read():
        nonlocal selected, replaying, replay_text, auto_text, auto_success, auto_config_update
        for line in process.stdout:
            try:
                value = json.loads(line)
            except ValueError:
                continue
            if auto_pending:
                if value.get("method") == "session/update":
                    update = value.get("params", {}).get("update", {})
                    auto_text += update.get("content", {}).get("text", "")
                    auto_config_update |= update.get("sessionUpdate") == "config_option_update"
                    record("auto_control_update", kind=update.get("sessionUpdate"),
                           keys=list(update), text_length=len(update.get("content", {}).get("text", "")))
                    continue
                if value.get("id") == "t3-auto-control":
                    error = value.get("error", {})
                    message = re.sub(r"[A-Za-z0-9_/+=-]{32,}", "[redacted]", str(error.get("message", "")))
                    if any(word in message.lower() for word in ["token", "authorization", "credential", "login"]):
                        message = "[authentication diagnostic omitted]"
                    record("auto_control_response", error_code=error.get("code"),
                           error_message=message[:500], result_keys=list(value.get("result", {})))
                    auto_success = "error" not in value and (auto_config_update or "auto" in auto_text.lower()) and not any(
                        word in auto_text.lower() for word in ["can't", "unavailable", "error", "unknown"])
                    auto_control.set()
                    continue
            request = pending.pop(str(value.get("id")), None) if "method" not in value else None
            if value.get("method") and "id" in value:
                record("native_request", method=value["method"])
            if copilot and value.get("method") == "session/update":
                update = value.get("params", {}).get("update", {})
                if update.get("sessionUpdate", "").startswith("tool_call"):
                    record("provider_tool_call")
                reported = update.get("modelId", update.get("model", ""))
                if isinstance(reported, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,100}", reported):
                    record("provider_model_report", model=reported)
            if request:
                method, params = request
                if method == "session/set_config_option" and params.get("configId") == "model":
                    for option in value.get("result", {}).get("configOptions", []):
                        current = option.get("currentValue", "")
                        if option.get("id") == "model":
                            selected = current.startswith("claude-haiku-5-5") and "fast=true" not in current
                            record("cheap_model_ack", accepted=selected, current=current)
                if method == "session/load":
                    record("load_ack", success="error" not in value,
                           same_id=value.get("result", {}).get("sessionId") == params.get("sessionId"))
                    replaying = False
            if value.get("method") == "session/update" and replaying:
                update = value.get("params", {}).get("update", {})
                if update.get("sessionUpdate") == "agent_message_chunk":
                    replay_text = (replay_text + update.get("content", {}).get("text", ""))[-2 * len(marker):]
                if marker in replay_text:
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
            if method:
                record("client_method", method=method)
            params = value.get("params", {})
            if method == "session/prompt":
                # The adapter adds host context, so inspect only for our exact
                # synthetic marker. The second prompt is never sent to a model.
                valid = marker in json.dumps(params.get("prompt", []))
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline:
                    ready = any(row["event"] == "tools_list" and row.get("epoch") == epoch
                                for row in rows(log))
                    if ready:
                        break
                    time.sleep(.05)
                if copilot and ready:
                    # Official session-scoped configuration command; not inference.
                    # Swallow its informational output so it cannot masquerade as
                    # the model's answer in the adapter's synthetic turn.
                    auto_pending = True
                    process.stdin.write(json.dumps(dict(jsonrpc="2.0", id="t3-auto-control",
                        method="session/prompt", params=dict(sessionId=params["sessionId"],
                            prompt=[dict(type="text", text="/model --session auto")]))) + "\n")
                    process.stdin.flush()
                    selected = auto_control.wait(10) and auto_success
                    auto_pending = False
                    diagnostic = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", auto_text)
                    diagnostic = re.sub(r"[A-Za-z0-9_/+=-]{32,}", "[redacted]", diagnostic)
                    if any(word in diagnostic.lower() for word in ["token", "authorization", "credential", "login"]):
                        diagnostic = "[authentication diagnostic omitted]"
                    record("auto_mode_ack", accepted=selected, actual_model="not_reported",
                           control_diagnostic=diagnostic[:700])
                record("prompt_barrier", mcp_ready=ready, cheap_model_ack=selected, synthetic_marker_valid=valid)
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
                    assert not budget["recall_allowed"] and budget[provider]["sent_or_uncertain"] == 0
                    if copilot:
                        assert budget.get("copilot_auto_decision") == "D-6"
                    budget[provider].update(sent_or_uncertain=1, model=model, head=config["head"],
                                             auth_route="inherited-provider-bootstrap", status="reserved_before_delivery")
                    if copilot:
                        budget[provider]["session_id"] = params["sessionId"]
                        budget[provider]["synthetic_cwd"] = config["work"]
                    temporary = ledger.with_suffix(".pending")
                    with open(temporary, "w", opener=lambda path, flags: os.open(path, flags, 0o600)) as output:
                        json.dump(budget, output, indent=2)
                        output.flush()
                        os.fsync(output.fileno())
                    temporary.replace(ledger)
                record("model_request_forwarded")
            if "id" in value:
                pending[str(value["id"])] = (method, params)
            if method == "session/load":
                replay_text = ""
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
