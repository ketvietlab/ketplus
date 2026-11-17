#!/usr/bin/env python3
"""Bounded production-adapter observer; only its first authorized prompt can pass.

Epoch two intercepts the prompt AFTER the real load/model ACK. No provider
history or ACK is fabricated. Only this newly created session's marker presence
is recorded, never message contents or authentication material.
"""
import fcntl
import hashlib
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import threading
import time

from bounded_diagnostics import Diagnostics, numeric
import cursor_extra_budget

sys.dont_write_bytecode = True
MODEL = "claude-haiku-5-5-low"
MARKER = "T3_BOUNDED_CURSOR_ALPHA"


def cursor_model(config):
    model = config.get("model", MODEL)
    assert model in [MODEL, "composer-2.5"]
    if model == "composer-2.5":
        assert config["mode"] in ["control", "cursor-extra-10"]
        assert config["purpose"] == "completed_history_refresh"
    return model


def cheap_model_ack(model, options):
    if model not in [MODEL, "composer-2.5"]:
        return False, "unmatched"
    current = next((option.get("currentValue") for option in options if option.get("id") == "model"), None)
    allowed = ["claude-haiku-5-5", MODEL] if model == MODEL else ["composer-2.5", "composer-2.5[fast=false]"]
    accepted = current in allowed
    # Composer's catalog base ID is the non-fast model. Refuse a conflicting
    # explicit fast option even if the model field still names that base ID.
    if model == "composer-2.5":
        accepted &= all(option.get("currentValue") is False or option.get("currentValue") == "false"
                        for option in options if option.get("id") == "fast")
    return bool(accepted), current if accepted else "unmatched"


def load_echo(result, requested):
    # ACP load may return an empty object; identity belongs to the request.
    present = "sessionId" in result
    return dict(echo_present=present,
                echo_matches_requested=result.get("sessionId") == requested if present else None)


def rows(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def approved_discovery(observed):
    spawned = {row["pid"] for row in observed if row.get("method") == "spawn"}
    listed = {row["pid"] for row in observed if row.get("method") == "tools/list"}
    return spawned, listed, bool(spawned) and spawned <= listed


def validate_allowance(config, budget, provider):
    decision = config["mode"]
    if decision == "cursor-extra-10":
        cursor_extra_budget.validate(config, budget, provider)
        return
    assert decision in ["D-3", "D-7"] and budget["decision"] == decision
    assert budget["maximum_per_provider"] == 1
    assert budget["owner_conversation"] == config["owner"] and config["head"]
    assert not budget["retry_allowed"] and not budget["recall_allowed"]
    assert budget[provider]["sent_or_uncertain"] == 0
    if decision == "D-7":
        # A separate user-approved extra Cursor allowance, never a reset/refund
        # of D-3. Bind it to the already consumed, unchanged parent ledger.
        assert provider == "cursor" and budget["allowed_providers"] == ["cursor"]
        assert budget["maximum_total"] == 1
        previous = pathlib.Path(budget["previous_ledger"]).read_bytes()
        assert hashlib.sha256(previous).hexdigest() == budget["previous_ledger_sha256"]
        parent = json.loads(previous)
        assert parent["decision"] == "D-3" and parent["owner_conversation"] == config["owner"]
        assert all(parent[name]["sent_or_uncertain"] == 1 for name in ["codex", "cursor", "copilot"])
    if provider == "copilot":
        assert decision == "D-3" and budget.get("copilot_auto_decision") == "D-6"


def make_attestation(config, epoch):
    from private_attestation import Attestation
    session = config["approved_session"]
    assert session["cwd"] == config["work"] and len(session["mcpServers"]) == 1
    # Installed 2026.11.11-043ef28's request-supplied ACP resources loop.
    # A different module/version must fail closed, not credit project discovery.
    module = pathlib.Path(config["executable"]).resolve().parent / "8512.index.js"
    digest = "d70d62e5f06260a477583ae6a174c537c6e8d08ea9be24c96c9a0e7ec4702725"
    assert hashlib.sha256(module.read_bytes()).hexdigest() == digest
    site = dict(file=str(module), line=1, column=70274, sha256=digest)
    return Attestation(session["mcpServers"][0], session["cwd"], config["observer_epochs"][epoch], site)


def main():
    config = json.loads(pathlib.Path(sys.argv[1]).read_text())
    args = sys.argv[2:]
    provider = config.get("provider", "cursor")
    copilot = provider == "copilot"
    model = "auto" if copilot else cursor_model(config)
    marker = "T3_BOUNDED_COPILOT_ALPHA" if copilot else MARKER
    if "acp" not in args and "--acp" not in args:
        os.execv(config["executable"], [config["executable"], *args])
    if copilot:
        # Test-process settings only. No default model/configuration is changed.
        args += ["--model", "auto", "--no-auto-update", "--disable-builtin-mcps",
                 "--disable-client-telemetry", "--available-tools=t3_no_model_tools_bounded_probe"]
    log = pathlib.Path(config["log"])
    approval_log = pathlib.Path(config["approval_log"]) if config.get("approval_log") else None
    attested_probe = config.get("attestation", False)
    if approval_log:
        assert not copilot and (config["mode"] == "control" or attested_probe)
    approval_offset = len(rows(approval_log)) if approval_log else 0
    prior = rows(log)
    epoch = "two" if any(row["event"] == "provider" for row in prior) else "one"
    lock = threading.Lock()

    def record(event, **fields):
        with lock, log.open("a") as output:
            output.write(json.dumps(dict(event=event, pid=os.getpid(), epoch=epoch, **fields)) + "\n")

    def emit(value):
        with lock:
            print(json.dumps(value), flush=True)

    prior_workers = {row.get("child_pid", row["pid"]) for row in prior if row["event"] == "mcp"}
    prior_native = {row["child_pid"] for row in prior if row["event"] == "provider"}
    native_alive = sum(alive(pid) for pid in prior_native)
    if native_alive:
        # Observe and fail; never wait here and hide an adapter closure race.
        record("closure_barrier_failed", prior_native_alive=native_alive)
        raise RuntimeError("Previous owned native provider has not exited")
    if any(alive(pid) for pid in prior_workers):
        record("closure_barrier_failed", prior_mcp_alive=sum(alive(pid) for pid in prior_workers))
        raise RuntimeError("Previous owned MCP worker has not exited")
    attestation = None
    native_env = None
    if attested_probe:
        attestation = make_attestation(config, epoch)
        native_env = attestation.environment()
    try:
        process = subprocess.Popen([config["executable"], *args], cwd=config["work"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, text=True, env=native_env)
    except BaseException:
        if attestation:
            attestation.close()
        raise
    if attestation:
        attestation.native_pid = process.pid
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
    diagnostics = Diagnostics(record)
    monitoring_stopped = threading.Event()

    def read():
        nonlocal selected, replaying, replay_text, auto_text, auto_success, auto_config_update
        for line in process.stdout:
            try:
                value = json.loads(line)
            except ValueError:
                continue
            diagnostics.observe(value)
            if auto_pending:
                if value.get("method") == "session/update":
                    update = value.get("params", {}).get("update", {})
                    auto_text += update.get("content", {}).get("text", "")
                    auto_config_update |= update.get("sessionUpdate") == "config_option_update"
                    continue
                if value.get("id") == "t3-auto-control":
                    auto_success = "error" not in value and (auto_config_update or "auto" in auto_text.lower()) and not any(
                        word in auto_text.lower() for word in ["can't", "unavailable", "error", "unknown"])
                    auto_control.set()
                    continue
            request = pending.pop(str(value.get("id")), None) if "method" not in value else None
            if value.get("method") and "id" in value:
                record("native_request", method=value["method"])
            if value.get("method") == "session/update":
                update = value.get("params", {}).get("update", {})
                if update.get("sessionUpdate", "").startswith("tool_call"):
                    record("provider_tool_call")
                reported = update.get("modelId", update.get("model", ""))
                if isinstance(reported, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,100}", reported):
                    record("provider_model_report", model=reported)
            if request:
                method, params = request
                if method == "session/prompt":
                    # Preserve bounded diagnostics, never raw provider errors
                    # (they may contain tokens, endpoints or prompt text).
                    code = value.get("error", {}).get("code")
                    reason = value.get("result", {}).get("stopReason")
                    record("native_prompt_response", error_code=numeric(code),
                           stop_reason=reason if reason in ["end_turn", "max_tokens", "refusal", "cancelled",
                                                            "max_turn_requests"] else None,
                           failed="error" in value)
                if method == "session/set_config_option" and params.get("configId") == "model":
                    selected, current = cheap_model_ack(model, value.get("result", {}).get("configOptions", []))
                    record("cheap_model_ack", accepted=selected, current=current)
                if method == "session/load":
                    code = value.get("error", {}).get("code")
                    record("load_ack", success="error" not in value,
                           error_code=numeric(code),
                           **load_echo(value.get("result", {}), params.get("sessionId")))
                    replaying = False
            if value.get("method") == "session/update" and replaying:
                update = value.get("params", {}).get("update", {})
                if update.get("sessionUpdate") == "agent_message_chunk":
                    replay_text = (replay_text + update.get("content", {}).get("text", ""))[-2 * len(marker):]
                if marker in replay_text:
                    record("history_marker", kind=update.get("sessionUpdate"))
            emit(value)

    def guarded_read():
        try:
            read()
        except (OSError, ValueError, TypeError, AttributeError):
            diagnostics.closed(process.poll(), read_error=True)
        finally:
            if attestation:
                attestation.revoke_transport()
            diagnostics.closed(process.poll())
            auto_control.set()

    def monitor():
        while not monitoring_stopped.wait(.05):
            diagnostics.expire()

    thread = threading.Thread(target=guarded_read, daemon=True)
    thread.start()
    monitor_thread = threading.Thread(target=monitor, daemon=True)
    monitor_thread.start()

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
            if attestation and method in ["session/new", "session/load"]:
                expected = config["approved_session"]
                if params.get("cwd") != expected["cwd"] or params.get("mcpServers") != expected["mcpServers"]:
                    emit(dict(jsonrpc="2.0", id=value["id"], error=dict(
                        code=-32000, message="Approved injected launch tuple changed; no prompt sent.")))
                    continue
            if method == "session/prompt":
                # The adapter adds host context, so inspect only for our exact
                # synthetic marker. The second prompt is never sent to a model.
                valid = marker in json.dumps(params.get("prompt", []))
                deadline = time.monotonic() + 8
                ready = False
                while time.monotonic() < deadline:
                    if attestation:
                        state = attestation.snapshot()
                        ready = state["ready"]
                        if state["revoked"]:
                            break
                    elif approval_log:
                        observed = rows(approval_log)[approval_offset:]
                        # Project and request-injected instances may both start.
                        # Discovery in one must not attest the other instance.
                        spawned, listed, ready = approved_discovery(observed)
                    else:
                        ready = any(row["event"] == "tools_list" and row.get("epoch") == epoch
                                    for row in rows(log))
                    if ready:
                        break
                    time.sleep(.05)
                if attestation:
                    state = attestation.snapshot()
                    ready = state["ready"]
                    record("injected_attestation", attested=state["attested"],
                           ready=state["ready"], revoked=state["revoked"],
                           injected_workers=len(state["workers"]), inventory=state["inventory"],
                           rejected=state["rejected"], forbidden=state["forbidden"])
                    for pid in state["all_workers"]:
                        record("mcp", child_pid=pid)
                elif approval_log:
                    record("approved_mcp_discovery", spawned=len(spawned), listed=len(listed))
                    for pid in spawned:
                        record("mcp", child_pid=pid)
                if copilot and ready:
                    # Official session-scoped configuration command; not inference.
                    # Swallow its informational output so it cannot masquerade as
                    # the model's answer in the adapter's synthetic turn.
                    auto_pending = True
                    diagnostics.sent("t3-auto-control", "session/prompt", auto_control=True)
                    process.stdin.write(json.dumps(dict(jsonrpc="2.0", id="t3-auto-control",
                        method="session/prompt", params=dict(sessionId=params["sessionId"],
                            prompt=[dict(type="text", text="/model --session auto")]))) + "\n")
                    process.stdin.flush()
                    acknowledged = auto_control.wait(10)
                    if not acknowledged:
                        diagnostics.timed_out("t3-auto-control")
                    selected = acknowledged and auto_success
                    auto_pending = False
                    record("auto_mode_ack", accepted=selected, actual_model="not_reported",
                           reason="accepted" if selected else "control_not_acknowledged")
                # Recheck after all potentially blocking controls, immediately
                # before the barrier and before any allowance can be reserved.
                if attestation:
                    ready = attestation.snapshot()["ready"]
                ready = ready and process.poll() is None
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
                    validate_allowance(config, budget, provider)
                    if config["mode"] == "cursor-extra-10":
                        cursor_extra_budget.record_reservation(config, budget, model, params["sessionId"])
                    else:
                        budget[provider]["sent_or_uncertain"] = 1
                    budget[provider].update(model=model, head=config["head"],
                                             auth_route="inherited-provider-bootstrap", status="reserved_before_delivery",
                                             session_id=params["sessionId"], synthetic_cwd=config["work"])
                    temporary = ledger.with_suffix(".pending")
                    with open(temporary, "w", opener=lambda path, flags: os.open(path, flags, 0o600)) as output:
                        json.dump(budget, output, indent=2)
                        output.flush()
                        os.fsync(output.fileno())
                    temporary.replace(ledger)
                record("model_request_forwarded", ordinal=budget[provider]["sent_or_uncertain"],
                       purpose=config["purpose"] if config["mode"] == "cursor-extra-10" else "single_authorized_probe")
            if "id" in value:
                pending[str(value["id"])] = (method, params)
                diagnostics.sent(value["id"], method)
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
        process.stdin.close()
        # Never block close() on a reader still holding the buffered stream
        # lock (for example a descendant retaining the stdout descriptor).
        if not thread.is_alive():
            process.stdout.close()
        monitoring_stopped.set()
        monitor_thread.join(1)
        diagnostics.closed(process.poll())
        diagnostics.report("transport", "process_exit", status=process.poll())
        if attestation:
            attestation.revoke_transport()
            # Allow asynchronous observer messages to drain after owned exit.
            time.sleep(.1)
            state = attestation.snapshot()
            record("injected_attestation_final", attested=state["attested"],
                   ready=state["ready"], revoked=state["revoked"],
                   transport_closed=state["transport_closed"],
                   injected_workers=len(state["workers"]), inventory=state["inventory"],
                   rejected=state["rejected"], forbidden=state["forbidden"])
            for row in attestation.events:
                record("observer_metadata", kind=row.get("event"), role=row.get("role"),
                       observer_pid=row.get("observerPid"), child_pid=row.get("childPid"),
                       main_thread=row.get("mainThread"), thread_id=row.get("threadId"),
                       injected=row.get("injected"),
                       callsites=[dict(file=pathlib.Path(file).name, line=int(line), column=int(column))
                                  for file, line, column in re.findall(
                                      r"(/[^ ()]+\.(?:js|cjs)):(\d+):(\d+)", row.get("callsite", ""))])
            attestation.close()
        record("provider_exit", child_exited=process.poll() is not None)


if __name__ == "__main__":
    main()
