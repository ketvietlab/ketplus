#!/usr/bin/env python3
"""Check systemd --user cleanup when the trusted supervisor itself crashes."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

from probe import CGROUP, emit, read_event, supervise, wait_file


def main():
    assert sys.platform == "linux" and os.getuid() != 0
    unit = "ketplus-t7-crash-" + uuid.uuid4().hex + ".service"
    with tempfile.TemporaryDirectory(prefix="ketplus-user-unit-") as directory:
        marker = Path(directory) / "ready"
        command = [
            "systemd-run", "--user", "--quiet", "--wait", "--collect", "--pipe",
            "--unit=" + unit, "-p", "Delegate=yes", "-p", "KillMode=control-group",
            "-p", "TimeoutStopSec=1s", "-p", "RuntimeMaxSec=15s", "-p", "Restart=no",
            sys.executable, str(Path(__file__).resolve()), "--supervisor", str(marker),
        ]
        process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, bufsize=0)
        try:
            record = read_event(process)
            base = Path(record["unit_cgroup"])
            assert record["uid"] == os.getuid() and record["type"] == "domain"
            assert read_event(process)["stage"] == "contained"
            assert not marker.exists()
            process.stdin.write(b"R")
            process.stdin.flush()
            worker = wait_file(marker, process)
            assert all(result != "allowed" for result in worker["attempts"].values())
            # Only systemd's recorded MainPID is targeted, never a saved PID lookup.
            subprocess.run([
                "systemctl", "--user", "kill", "--kill-who=main", "--signal=SIGKILL", unit,
            ], check=True)
            result = process.wait(timeout=8)
            assert result != 0, "supervisor was not killed"
            deadline = time.monotonic() + 5
            while base.exists() and time.monotonic() < deadline:
                time.sleep(0.02)
            assert not base.exists(), "user manager did not reclaim its unit subtree"
            emit(case="user-unit-supervisor-crash", uid=record["uid"],
                 cgroup_removed=True, double_fork=True,
                 result="user manager reclaimed the crashed supervisor's subtree")
        finally:
            subprocess.run(["systemctl", "--user", "stop", unit],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if process.poll() is None:
                process.wait(timeout=8)


def unit_supervisor(marker):
    relative = Path("/proc/self/cgroup").read_text().strip().removeprefix("0::")
    base = CGROUP / relative.lstrip("/")
    job, sibling = base / "job", base / "sibling"
    job.mkdir()
    sibling.mkdir()
    emit(unit_cgroup=str(base), uid=os.getuid(),
         type=(base / "cgroup.type").read_text().strip())
    supervise([str(job), str(sibling), marker, "yes"])


if __name__ == "__main__":
    if len(sys.argv) == 1:
        main()
    elif sys.argv[1] == "--supervisor":
        unit_supervisor(sys.argv[2])
    else:
        raise SystemExit("unknown probe mode")
