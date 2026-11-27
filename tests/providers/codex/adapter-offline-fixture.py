#!/usr/bin/env python3
"""Fixtures for the opt-in production CodexAdapter control test; never run a model."""
import argparse
import json
import os
import pathlib
import signal
import sys
import threading
import time


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def append(log, event, **fields):
    with open(log, "a") as output:
        output.write(json.dumps(dict(event=event, pid=os.getpid(), at=time.monotonic_ns(), **fields)) + "\n")


def launch(config_file, arguments):
    config = json.loads(pathlib.Path(config_file).read_text())
    # Child-only isolation; never inherit credentials or the user's provider profile.
    environment = {key: value for key, value in os.environ.items()
                   if key in ("PATH", "TMPDIR", "LANG", "LC_ALL", "SYSTEMROOT")}
    environment["CODEX_HOME"] = config["profile"]
    os.chdir(config["work"])
    if "app-server" in arguments:
        log = pathlib.Path(config["log"])
        prior = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        # Observe closure at replacement launch; do not wait here or hide an adapter race.
        workers = {entry["pid"] for entry in prior if entry["event"] == "mcp"}
        append(log, "provider", prior_mcp_alive=sum(alive(pid) for pid in workers))
    os.execve(config["executable"], [config["executable"], *arguments], environment)


def worker(log):
    epoch = os.environ["KETPLUS_AGENT_TRANSPORT_EPOCH"]
    append(log, "mcp", epoch=epoch)
    timer = threading.Timer(45, lambda: os._exit(0))
    timer.daemon = True
    timer.start()

    def terminate(*_):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, terminate)
    try:
        for line in sys.stdin:
            message = json.loads(line)
            request = message.get("id")
            if request is None:
                continue
            method = message.get("method")
            if method == "initialize":
                result = dict(protocolVersion="2024-11-05", capabilities=dict(tools={}),
                              serverInfo=dict(name="t3-synthetic-control", version=epoch))
            elif method == "tools/list":
                names = ["agents_whoami", "background_task_list", "background_task_get",
                         "background_task_output", "background_task_start", "background_task_stop",
                         "t3_epoch_" + epoch]
                result = dict(tools=[dict(name=name, description="Synthetic discovery only",
                                         inputSchema=dict(type="object", properties={})) for name in names])
            elif method == "tools/call":
                append(log, "unexpected_tool", epoch=epoch)
                print(json.dumps(dict(jsonrpc="2.0", id=request,
                                      error=dict(code=-32601, message="No tool calls allowed"))), flush=True)
                continue
            else:
                result = {}
            print(json.dumps(dict(jsonrpc="2.0", id=request, result=result)), flush=True)
    finally:
        append(log, "mcp_exit", epoch=epoch)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--launch-config")
    parser.add_argument("--worker-log")
    options, extra = parser.parse_known_args()
    if options.launch_config:
        launch(options.launch_config, extra)
    elif options.worker_log:
        worker(options.worker_log)
    else:
        parser.error("a fixture role is required")
