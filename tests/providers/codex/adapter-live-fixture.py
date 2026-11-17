#!/usr/bin/env python3
"""Owned-process observer for the explicitly authorized bounded adapter probe.

Authentication stays with the provider's inherited environment and credential
store. Logs contain process identities and synthetic epochs, never protocol
contents, environment values, credentials, or existing history.
"""
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


def record(log, event, **fields):
    with open(log, "a") as output:
        output.write(json.dumps(dict(event=event, pid=os.getpid(), **fields)) + "\n")


def launch(path, arguments):
    config = json.loads(pathlib.Path(path).read_text())
    os.chdir(config["work"])
    if "app-server" in arguments:
        log = pathlib.Path(config["log"])
        rows = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
        workers = {row["pid"] for row in rows if row["event"] == "mcp"}
        record(log, "provider", prior_mcp_alive=sum(alive(pid) for pid in workers))
        # Process-local override only; do not load unrelated configured MCP servers.
        arguments += ["-c", "mcp_servers={}"]
    os.execv(config["executable"], [config["executable"], *arguments])


def worker(log):
    epoch = os.environ["KETPLUS_AGENT_TRANSPORT_EPOCH"]
    record(log, "mcp", epoch=epoch)
    timer = threading.Timer(180, lambda: os._exit(0))
    timer.daemon = True
    timer.start()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        for line in sys.stdin:
            message = json.loads(line)
            request = message.get("id")
            method = message.get("method")
            record(log, "mcp_method", method=method, epoch=epoch)
            if request is None:
                if method == "notifications/initialized" and os.environ.get("T3_MCP_LIST_CHANGED") == "1":
                    print(json.dumps(dict(jsonrpc="2.0", method="notifications/tools/list_changed")), flush=True)
                continue
            if method == "initialize":
                tools = dict(listChanged=True) if os.environ.get("T3_MCP_LIST_CHANGED") == "1" else {}
                result = dict(protocolVersion=message.get("params", {}).get("protocolVersion", "2024-11-05"), capabilities=dict(tools=tools),
                              serverInfo=dict(name="t3-bounded", version=epoch))
            elif method == "tools/list":
                record(log, "tools_list", epoch=epoch)
                names = ["agents_whoami", "background_task_start", "background_task_list", "background_task_get",
                         "background_task_stop", "background_task_output", "t3_epoch_" + epoch]
                result = dict(tools=[dict(name=name, description="Synthetic discovery only",
                                         inputSchema=dict(type="object", properties={})) for name in names])
            elif method == "resources/list":
                result = dict(resources=[])
            elif method == "resources/templates/list":
                result = dict(resourceTemplates=[])
            else:
                if method == "tools/call":
                    record(log, "unexpected_tool", epoch=epoch)
                print(json.dumps(dict(jsonrpc="2.0", id=request,
                                      error=dict(code=-32601, message="Discovery only"))), flush=True)
                continue
            print(json.dumps(dict(jsonrpc="2.0", id=request, result=result)), flush=True)
    finally:
        record(log, "mcp_exit", epoch=epoch)


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
