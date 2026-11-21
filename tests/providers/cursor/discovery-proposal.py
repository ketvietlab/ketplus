#!/usr/bin/env python3
"""Prepare, inspect or serve a synthetic MCP approval proposal. Never approve.

Preparation only writes a new private test directory. Provider commands remain
reviewable text until the user authorizes the exact approval. No model, account
store, real daemon API or existing transcript is accessed by this fixture.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shlex
import signal
import sys
import threading
import uuid


def write(path, value):
    with open(path, "x", opener=lambda name, flags: os.open(name, flags, 0o600)) as output:
        output.write(value)


def prepare(root):
    root.mkdir(mode=0o700, parents=False, exist_ok=False)
    for name in ["config", "data", "work", "work/.cursor"]:
        (root / name).mkdir(mode=0o700)
    source = pathlib.Path(__file__).resolve()
    identity = "ketplus_t3_cursor_discovery_" + uuid.uuid4().hex[:12]
    server = dict(command=sys.executable, args=[str(source), "--serve", "--root", str(root)], env={})
    manifest = dict(identity=identity, tool="t3_probe_status", cwd=str(root / "work"),
                    source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), server=server,
                    auth="inherited HOME/provider integration; no credential copying",
                    approval_executed=False, model_prompts=0)
    write(root / "manifest.json", json.dumps(manifest, indent=2) + "\n")
    write(root / "work/.cursor/mcp.json", json.dumps(dict(mcpServers={identity: server}), indent=2) + "\n")
    # Deny tool execution: discovery does not need an MCP tool allowlist. The
    # server itself also exposes no execution, filesystem, URL or daemon tool.
    permissions = dict(allow=[], deny=["Shell(*)", "Read(*)", "Write(*)", "WebFetch(*)", "Mcp(*:*)"])
    write(root / "work/.cursor/cli.json", json.dumps(dict(permissions=permissions), indent=2) + "\n")
    acp = dict(name=identity, command=server["command"], args=server["args"], env=[])
    write(root / "session-new.json", json.dumps(dict(cwd=manifest["cwd"], mcpServers=[acp]), indent=2) + "\n")
    prefix = ("CURSOR_CONFIG_DIR=" + shlex.quote(str(root / "config")) +
              " CURSOR_DATA_DIR=" + shlex.quote(str(root / "data")) + " agent mcp ")
    instructions = ("PROPOSAL ONLY. Approval has NOT been executed. No model prompt is authorized here.\n"
                    "Server: " + identity + "\nTool: t3_probe_status (static, read-only; calls denied by project policy).\n"
                    "Source SHA-256: " + manifest["source_sha256"] + "\n"
                    "Run each command from: " + manifest["cwd"] + "\n\n"
                    "Inspect without approving:\n" + prefix + "list-tools " + shlex.quote(identity) + "\n\n"
                    "USER APPROVAL REQUIRED before this exact command:\n" + prefix + "enable " + shlex.quote(identity) + "\n\n"
                    "After approval, discovery only:\n" + prefix + "list-tools " + shlex.quote(identity) + "\n\n"
                    "Revoke using the official command:\n" + prefix + "disable " + shlex.quote(identity) + "\n\n"
                    "After confirming owned processes exited, remove ONLY this entire test root to discard\n"
                    "its isolated approval/disabled cache. Never edit a user approval file or copy an approval hash.\n"
                    "CLI project discovery and ACP request injection are separate controls. session-new.json\n"
                    "uses the same name/cwd/command/args/env, but transfer of approval is NOT assumed.\n")
    write(root / "REVIEW.txt", instructions)
    print(json.dumps(dict(prepared=True, identity=identity, review=str(root / "REVIEW.txt"),
                          source_sha256=manifest["source_sha256"], approval_executed=False)))


def serve(root):
    manifest = json.loads((root / "manifest.json").read_text())
    if hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest() != manifest["source_sha256"]:
        raise RuntimeError("Fixture changed after proposal review; prepare a new proposal.")
    log = root / "events.jsonl"

    def record(method):
        with open(log, "a", opener=lambda name, flags: os.open(name, flags, 0o600)) as output:
            output.write(json.dumps(dict(method=method, pid=os.getpid())) + "\n")

    def emit(value):
        print(json.dumps(value), flush=True)

    timer = threading.Timer(60, lambda: os._exit(0))
    timer.daemon = True
    timer.start()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    record("spawn")
    try:
        for line in sys.stdin:
            message = json.loads(line)
            method = message.get("method")
            record(method)
            request = message.get("id")
            if request is None:
                continue
            if method == "initialize":
                result = dict(protocolVersion=message.get("params", {}).get("protocolVersion", "2024-11-05"),
                              capabilities=dict(tools={}), serverInfo=dict(name=manifest["identity"], version="1.0.0"))
            elif method == "tools/list":
                result = dict(tools=[dict(name="t3_probe_status", description="Returns static synthetic readiness only",
                                         inputSchema=dict(type="object", properties={}, additionalProperties=False),
                                         annotations=dict(readOnlyHint=True, destructiveHint=False,
                                                          idempotentHint=True, openWorldHint=False))])
            elif method == "tools/call" and message.get("params", {}).get("name") == "t3_probe_status":
                if message["params"].get("arguments", {}) != {}:
                    raise ValueError("The static fixture accepts no input")
                result = dict(content=[dict(type="text", text="synthetic-ready")])
            elif method == "resources/list":
                result = dict(resources=[])
            elif method == "resources/templates/list":
                result = dict(resourceTemplates=[])
            elif method == "ping":
                result = {}
            else:
                emit(dict(jsonrpc="2.0", id=request, error=dict(code=-32601, message="Static fixture only")))
                continue
            emit(dict(jsonrpc="2.0", id=request, result=result))
    finally:
        record("exit")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--prepare", action="store_true")
    mode.add_argument("--serve", action="store_true")
    parser.add_argument("--root", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    if args.prepare:
        prepare(root)
    else:
        serve(root)
