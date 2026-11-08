#!/usr/bin/env python3
"""Sample the native #126 workload with macOS top; never launch a provider."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import re
import selectors
import subprocess
import sys
import time

TOP_SAMPLES = 5


def read_lines(stream, deadline):
    """Yield complete stdout lines; raise TimeoutError once the wall deadline passes."""
    selector = selectors.DefaultSelector()
    selector.register(stream, selectors.EVENT_READ)
    pending = b""
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError
        if not selector.select(remaining):
            continue
        chunk = os.read(stream.fileno(), 65536)
        if not chunk:
            if pending:
                yield pending.decode()
            return
        pending += chunk
        *lines, pending = pending.split(b"\n")
        for line in lines:
            yield line.decode() + "\n"


def top_samples(path, pid):
    """Count top's per-sample rows for the fixture process."""
    row = re.compile(rf"^\s*{pid}\s+\d+(\.\d+)?\s")
    return sum(1 for line in path.read_text().splitlines() if row.match(line))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--work", choices=("running", "idle"), default="running")
    parser.add_argument("--visibility", choices=("active", "inactive", "hidden"), default="active")
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--timeout", type=int, default=150,
                        help="Wall-clock limit for the whole run in seconds")
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("This qualification uses macOS top and the Cocoa backend.")
    cache = (args.build / "CMakeCache.txt").read_text()
    if "CMAKE_BUILD_TYPE:STRING=Release\n" not in cache:
        parser.error("Use a Release build for CPU qualification.")
    # Each attempt keeps its own evidence; never mix a new run with old files.
    if args.output.exists() and any(args.output.iterdir()):
        parser.error(f"{args.output} is not empty; use a new directory per attempt.")
    args.output.mkdir(parents=True, exist_ok=True)
    executable = args.build.resolve() / "ketplus_agent_idle_benchmark.app/Contents/MacOS/ketplus_agent_idle_benchmark"
    if not executable.is_file():
        parser.error("Build the ketplus_agent_idle_benchmark target first.")
    command = [str(executable), "--work", args.work, "--visibility", args.visibility]
    if args.capture:
        command += ["--capture", str(args.output.resolve() / "workspace.png")]
    environment = os.environ | {
        "QT_QPA_PLATFORM": "cocoa", "QSG_RHI_BACKEND": "metal",
        "KETPLUS_DISABLE_DAEMON_AUTOSTART": "1",
    }
    metadata = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "system": platform.system(), "osVersion": platform.mac_ver()[0],
        "architecture": platform.machine(),
        "chip": subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip(),
        "work": args.work, "visibility": args.visibility,
        "reducedMotion": "KETPLUS_REDUCED_MOTION" in environment,
        "topCommand": "top -l 5 -s 2 -pid <fixture-pid> -stats pid,cpu,threads,state",
    }
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    result = None
    top = None
    process = None
    with (args.output / "events.jsonl").open("w") as events, \
         (args.output / "stderr.log").open("w") as errors, \
         (args.output / "top.txt").open("w") as samples:
        try:
            deadline = time.monotonic() + args.timeout
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors,
                                       env=environment)
            for line in read_lines(process.stdout, deadline):
                events.write(line)
                events.flush()
                print(line, end="", flush=True)
                row = json.loads(line)
                if row.get("event") == "begin":
                    if top is not None:
                        raise RuntimeError("Fixture started measurement twice")
                    top = subprocess.Popen(
                        ["top", "-l", "5", "-s", "2", "-pid", str(process.pid),
                         "-stats", "pid,cpu,threads,state"], stdout=samples, stderr=errors)
                if row.get("event") == "result":
                    result = row
            code = process.wait(timeout=max(1, deadline - time.monotonic()))
            top_code = top.wait(timeout=max(1, deadline - time.monotonic())) if top is not None else 2
            samples.flush()
            collected = top_samples(args.output / "top.txt", process.pid) if top is not None else 0
            if collected < TOP_SAMPLES:
                print(f"collection failure: {collected}/{TOP_SAMPLES} top samples for "
                      f"pid {process.pid}", file=sys.stderr)
                return 2
            return code or top_code or (0 if result and result.get("valid") else 2)
        except (TimeoutError, subprocess.TimeoutExpired):
            print(f"collection failure: the run exceeded {args.timeout} s", file=sys.stderr)
            return 2
        finally:
            for child in (top, process):
                if child is not None and child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()


if __name__ == "__main__":
    sys.exit(main())
