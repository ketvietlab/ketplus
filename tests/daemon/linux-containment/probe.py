#!/usr/bin/env python3
"""Opt-in Linux primitive probe, never the production executor or quota test.

Run only inside a disposable delegated systemd --user service. See ADR 0127's
Linux evidence document. Every workload is synthetic and has a finite lifetime.
"""

import ctypes
import errno
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time


SELF = str(Path(__file__).resolve())
CGROUP = Path("/sys/fs/cgroup")


def emit(**values):
    print(json.dumps(values, sort_keys=True), flush=True)


def populated(path):
    return "populated 1" in (path / "cgroup.events").read_text()


def empty(path):
    if populated(path):
        (path / "cgroup.kill").write_text("1")
    deadline = time.monotonic() + 5
    while populated(path) and time.monotonic() < deadline:
        time.sleep(0.02)
    assert not populated(path), f"cleanup_pending: {path.name}"


def wait_file(path, process):
    deadline = time.monotonic() + 8
    while not path.exists() and time.monotonic() < deadline:
        assert process.poll() is None, "launcher exited before ready"
        time.sleep(0.02)
    assert path.exists(), "workload readiness timeout"
    return json.loads(path.read_text())


def write_attempt(path, value, directory=None):
    try:
        fd = os.open(path, os.O_WRONLY, dir_fd=directory)
        try:
            os.write(fd, value.encode())
        finally:
            os.close(fd)
        return "allowed"
    except OSError as error:
        return errno.errorcode.get(error.errno, str(error.errno))


def payload(args):
    marker, sibling, peer, inherited, rootfd, nsfd = args
    attempts = {"path": write_attempt(sibling, str(os.getpid()))}
    attempts["proc_peer_root"] = write_attempt(
        f"/proc/{peer}/root{sibling}", str(os.getpid())
    )
    attempts["inherited_root_alias"] = write_attempt(
        sibling.lstrip("/"), str(os.getpid()), int(rootfd)
    )
    try:
        os.write(int(inherited), str(os.getpid()).encode())
        attempts["inherited_cgroup_fd"] = "allowed"
    except OSError as error:
        attempts["inherited_cgroup_fd"] = errno.errorcode[error.errno]
    libc = ctypes.CDLL(None, use_errno=True)
    result = libc.setns(int(nsfd), 0x02000000)  # CLONE_NEWCGROUP
    attempts["setns_ancestor"] = (
        "allowed" if result == 0 else errno.errorcode[ctypes.get_errno()]
    )
    mountpoint = Path(marker + ".mount")
    mountpoint.mkdir()
    mounted = libc.mount(b"none", os.fsencode(mountpoint), b"cgroup2", 0, None)
    remount = "mounted" if mounted == 0 else errno.errorcode[ctypes.get_errno()]
    if mounted == 0:
        attempts["remount_sibling"] = write_attempt(
            str(mountpoint / Path(sibling).parent.name / "cgroup.procs"),
            str(os.getpid()),
        )
    # Keep the strongest probes above: intentionally leaked handles are NOT a
    # recommended launcher policy. A real launcher must close unrelated FDs.
    for fd in (int(inherited), int(rootfd), int(nsfd)):
        if fd >= 0:
            os.close(fd)
    ordinary = os.fork()
    if ordinary == 0:
        time.sleep(25)
        os._exit(0)
    intermediate = os.fork()
    if intermediate == 0:
        os.setsid()
        detached = os.fork()
        if detached:
            os._exit(0)
        Path(marker + ".detached").write_text(json.dumps({
            "pid": os.getpid(), "pgid": os.getpgrp(),
            "session": os.getsid(0),
        }))
        time.sleep(25)
        os._exit(0)
    os.waitpid(intermediate, 0)
    deadline = time.monotonic() + 2
    while not Path(marker + ".detached").exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    Path(marker).write_text(json.dumps({
        "uid": os.getuid(), "pid": os.getpid(), "pgid": os.getpgrp(),
        "session": os.getsid(0), "ordinary": ordinary,
        "detached": json.loads(Path(marker + ".detached").read_text()),
        "membership": Path("/proc/self/cgroup").read_text().strip(),
        "attempts": attempts, "cgroup_remount": remount,
    }))
    time.sleep(25)


def gate(args):
    gatefd, isolated, *payload_args = args
    # EOF/crash before release never runs the workload or its exec.
    token = os.read(int(gatefd), 1)
    os.close(int(gatefd))
    if token != b"R":
        return
    command = [sys.executable, SELF, "--payload", *payload_args]
    if isolated == "yes":
        # Close the authority-bearing descriptor before entering the namespace.
        os.close(int(payload_args[3]))
        payload_args[3] = "-1"
        command = [sys.executable, SELF, "--payload", *payload_args]
    if isolated != "no":
        command = ["unshare", "--user", "--map-root-user", "--mount",
                   "--pid", "--fork", "--cgroup", "--mount-proc", *command]
    os.execvp(command[0], command)


def supervise(args):
    job, sibling, marker, isolated = args
    gate_read, gate_write = os.pipe()
    inherited = os.open(str(Path(sibling) / "cgroup.procs"), os.O_WRONLY)
    rootfd = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
    nsfd = os.open("/proc/self/ns/cgroup", os.O_RDONLY)
    child = subprocess.Popen([
        sys.executable, SELF, "--gate", str(gate_read), isolated, marker,
        str(Path(sibling) / "cgroup.procs"), str(os.getpid()),
        str(inherited), str(rootfd), str(nsfd),
    ], pass_fds=(gate_read, inherited, rootfd, nsfd))
    for fd in (gate_read, inherited, rootfd, nsfd):
        os.close(fd)
    try:
        # Trusted gate exists before migration; arbitrary workload cannot run.
        Path(job, "cgroup.procs").write_text(str(child.pid))
        emit(stage="contained", child=child.pid)
        token = sys.stdin.buffer.read(1)
        if token == b"R":
            os.write(gate_write, token)
        os.close(gate_write)
        gate_write = -1
        if token == b"R":
            # A synthetic daemon lease: EOF triggers bounded reclamation.
            sys.stdin.buffer.read(1)
        empty(Path(job))
    finally:
        if gate_write >= 0:
            os.close(gate_write)
        empty(Path(job))
        child.wait(timeout=5)
        Path(marker + ".reclaimed").write_text("confirmed")


def read_event(process):
    assert select.select([process.stdout], [], [], 8)[0], "supervisor timeout"
    line = process.stdout.readline()
    assert line, "supervisor ended without containment acknowledgement"
    return json.loads(line)


def daemon(args):
    # A disposable fake daemon. Its death closes the supervisor's lease writer.
    supervisor = subprocess.Popen([
        sys.executable, SELF, "--supervise", *args,
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
    emit(**read_event(supervisor))
    token = sys.stdin.buffer.read(1)
    if token:
        supervisor.stdin.write(token)
        supervisor.stdin.flush()
    sys.stdin.buffer.read(1)
    supervisor.stdin.close()
    supervisor.wait(timeout=5)


def run_case(base, directory, case):
    job, sibling = base / (case + "-job"), base / (case + "-sibling")
    job.mkdir()
    sibling.mkdir()
    marker = directory / case
    isolated = {"delegation-only": "no", "descriptor-leak": "leaked"}.get(case, "yes")
    supervisor = subprocess.Popen([
        sys.executable, SELF,
        "--daemon" if case == "daemon-crash" else "--supervise", str(job), str(sibling),
        str(marker), isolated,
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=False, bufsize=0)
    try:
        contained = read_event(supervisor)
        assert contained["stage"] == "contained"
        assert (job / "cgroup.type").read_text().strip() == "domain"
        assert not marker.exists()
        if case == "barrier-crash":
            supervisor.kill()
            supervisor.wait(timeout=5)
            deadline = time.monotonic() + 3
            while populated(job) and time.monotonic() < deadline:
                time.sleep(0.02)
            assert not populated(job) and not marker.exists()
            emit(case=case, pre_exec_marker=False, populated=False)
            return
        supervisor.stdin.write(b"R")
        supervisor.stdin.flush()
        worker = wait_file(marker, supervisor)
        before = populated(job)
        if case == "namespace-init-death":
            launcher = contained["child"]
            children = Path(f"/proc/{launcher}/task/{launcher}/children").read_text().split()
            assert len(children) == 1 and worker["pid"] == 1
            init = int(children[0])
            identity = os.pidfd_open(init)
            try:
                assert str(init) in (job / "cgroup.procs").read_text().split()
                signal.pidfd_send_signal(identity, signal.SIGKILL)
            finally:
                os.close(identity)
            deadline = time.monotonic() + 5
            while populated(job) and time.monotonic() < deadline:
                time.sleep(0.02)
            assert not populated(job)
            emit(case=case, namespace_descendants_reclaimed=True)
        if case == "same-uid-peer":
            # An external peer remains in the original cgroup namespace.
            peer_results = [write_attempt(str(sibling / "cgroup.procs"), pid)
                            for pid in (job / "cgroup.procs").read_text().split()]
            emit(case=case, peer_migrations=peer_results)
        if case == "supervisor-crash":
            supervisor.kill()
            supervisor.wait(timeout=5)
            time.sleep(0.1)
            survives = populated(job)
            assert survives, "negative control did not exercise supervisor loss"
            emit(case=case, survives_supervisor_crash=survives)
        elif case == "daemon-crash":
            supervisor.kill()  # This handle is the synthetic daemon, not its supervisor.
            supervisor.wait(timeout=5)
            deadline = time.monotonic() + 5
            while (populated(job) or not Path(str(marker) + ".reclaimed").exists()) and time.monotonic() < deadline:
                time.sleep(0.02)
            assert not populated(job), "daemon death did not close the lease"
            assert Path(str(marker) + ".reclaimed").exists(), "supervisor did not finish cleanup"
            emit(case=case, lease_cleanup_confirmed=True)
        elif case == "lease-eof":
            supervisor.stdin.close()
            supervisor.wait(timeout=5)
            assert not populated(job)
        empty(job)
        after = populated(job)
        escaped = populated(sibling)
        emit(case=case, worker=worker, populated_before=before,
             populated_after_kill=after, sibling_populated=escaped)
        if isolated == "yes" and case != "same-uid-peer":
            assert all(result != "allowed" for result in worker["attempts"].values())
            assert worker["cgroup_remount"] == "mounted"
            assert not escaped
        if case in ("delegation-only", "descriptor-leak"):
            assert escaped, "negative control did not exercise migration"
        if case == "same-uid-peer":
            assert escaped and all(result == "allowed" for result in peer_results)
    finally:
        # Reclaim both owned cgroups, including deliberately escaped controls.
        empty(job)
        empty(sibling)
        if supervisor.poll() is None:
            supervisor.stdin.close()
            supervisor.wait(timeout=5)
        job.rmdir()
        sibling.rmdir()


def main():
    assert sys.platform == "linux" and os.getuid() != 0, "requires Linux daemon user"
    relative = Path("/proc/self/cgroup").read_text().strip().removeprefix("0::")
    base = CGROUP / relative.lstrip("/")
    assert ".service" in str(base), "run in a disposable delegated --user service"
    with tempfile.TemporaryDirectory(prefix="ketplus-containment-") as work:
        emit(uid=os.getuid(), kernel=os.uname().release,
             cgroup_type=(base / "cgroup.type").read_text().strip())
        for case in ("delegation-only", "descriptor-leak", "barrier-crash", "kill-tree",
                     "namespace-init-death", "lease-eof", "daemon-crash",
                     "supervisor-crash", "same-uid-peer"):
            run_case(base, Path(work), case)
    emit(result="primitive probes completed; not production acceptance")


if __name__ == "__main__":
    if len(sys.argv) == 1:
        main()
    elif sys.argv[1] == "--payload":
        payload(sys.argv[2:])
    elif sys.argv[1] == "--gate":
        gate(sys.argv[2:])
    elif sys.argv[1] == "--supervise":
        supervise(sys.argv[2:])
    elif sys.argv[1] == "--daemon":
        daemon(sys.argv[2:])
    else:
        raise SystemExit("unknown probe mode")
