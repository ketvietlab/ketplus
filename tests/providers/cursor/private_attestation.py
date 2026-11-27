"""Private per-transport test observer. Never writes a nonce to evidence logs."""
import hashlib
import json
import os
import pathlib
import secrets
import socket
import struct
import sys
import tempfile
import threading


class Attestation:
    def __init__(self, server, cwd, transport_epoch, injected_site=None):
        assert not server.get("env"), "Approved static fixture must carry no mutation grant"
        self.directory = tempfile.TemporaryDirectory(prefix="t3-att-", dir="/tmp")
        self.root = pathlib.Path(self.directory.name)
        self.nonce = secrets.token_hex(32)
        self.epoch = transport_epoch
        self.digest = hashlib.sha256(json.dumps(dict(server=server, cwd=cwd), sort_keys=True).encode()).hexdigest()
        self.native_pid = None
        self.events = []
        self.rejected = 0
        self.transport_closed = False
        self.lock = threading.Lock()
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.bind(str(self.root / "observer.sock"))
        os.chmod(self.root / "observer.sock", 0o600)
        self.socket.listen()
        self.socket.settimeout(.1)
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.receive, daemon=True)
        self.thread.start()
        config = dict(socket=str(self.root / "observer.sock"), nonce=self.nonce,
                      transportEpoch=self.epoch, launchDigest=self.digest,
                      command=server["command"], args=server["args"], cwd=cwd,
                      injectedSite=injected_site)
        self.config = self.root / "bootstrap.json"
        with open(self.config, "x", opener=lambda name, flags: os.open(name, flags, 0o600)) as output:
            json.dump(config, output)

    def environment(self):
        env = dict(os.environ)
        preload = pathlib.Path(__file__).with_name("attestation-preload.cjs").resolve()
        # Append to existing official environment; do not replace auth/settings.
        env["NODE_OPTIONS"] = env.get("NODE_OPTIONS", "") + " --require=" + json.dumps(str(preload))
        env["T3_ATTEST_CONFIG"] = str(self.config)
        return env

    def accept(self, row, peer_pid):
        with self.lock:
            if (row.get("nonce") != self.nonce or row.get("transportEpoch") != self.epoch or
                    row.get("launchDigest") != self.digest or
                    not peer_pid or row.get("observerPid") != peer_pid):
                self.rejected += 1
                return
            # Store process/method metadata only, never the authentication nonce.
            self.events.append({key: value for key, value in row.items()
                                if key not in ["nonce", "transportEpoch", "launchDigest"]})

    def receive(self):
        while not self.stopped.is_set():
            try:
                connection, _ = self.socket.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with connection:
                connection.settimeout(.5)
                data = b""
                try:
                    while b"\n" not in data and len(data) < 16384:
                        chunk = connection.recv(4096)
                        if not chunk:
                            break
                        data += chunk
                    if sys.platform == "darwin":
                        peer_pid = connection.getsockopt(0, 2)  # SOL_LOCAL / LOCAL_PEERPID
                    elif hasattr(socket, "SO_PEERCRED"):
                        peer_pid = struct.unpack("3i", connection.getsockopt(
                            socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i")))[0]
                    else:
                        peer_pid = None
                    self.accept(json.loads(data), peer_pid)
                except (ValueError, OSError):
                    with self.lock:
                        self.rejected += 1

    def revoke_transport(self):
        # The owned provider channel can close before Node delivers child
        # lifecycle callbacks. This is transport loss, not an invented child exit.
        with self.lock:
            self.transport_closed = True

    def snapshot(self):
        with self.lock:
            observed = list(self.events)
            rejected = self.rejected
            transport_closed = self.transport_closed
        direct = [row for row in observed if row.get("observerPid") == self.native_pid and
                  row.get("role") == "index.js" and row.get("mainThread") is True and
                  row.get("injected") is True]
        workers = {row["childPid"] for row in direct if row.get("event") == "spawn"}
        required = {"spawn", "initialize_sent", "initialize_result", "initialized_sent"}
        handshakes = set()
        for pid in workers:
            stages = {}
            for row in direct:
                if row.get("childPid") == pid and row.get("event") in required:
                    stages.setdefault(row["event"], []).append(row.get("sequence", -1))
            # Duplicate/malformed observations cannot replace an earlier stage
            # and repair an invalid handshake. Delivery order is irrelevant.
            if any(len(values) != 1 or type(values[0]) is not int for values in stages.values()):
                continue
            stages = {event: values[0] for event, values in stages.items()}
            order = [stages.get(key, -1) for key in ["spawn", "initialize_sent",
                                                    "initialize_result", "initialized_sent"]]
            if min(order) >= 0 and all(a < b for a, b in zip(order, order[1:])):
                handshakes.add(pid)
        errors = {"forbidden_tool_call", "observer_overflow", "invalid_frame"}
        valid = len(workers) == 1 and handshakes == workers and rejected == 0 and not any(
            row.get("event") in errors for row in observed)
        inventory = {row["childPid"] for row in direct
                     if row.get("event") == "inventory" and row.get("valid") is True}
        terminal = {"exit", "close", "child_error", "stdout_eof", "stdout_close",
                    "stdout_error", "stdin_close", "stdin_error"}
        # A terminal observation is sticky, including when its delivery races
        # older handshake records. Project/other-child exits do not revoke this
        # exact injected worker. Historical evidence remains available to audit.
        revoked = transport_closed or any(row.get("childPid") in workers and row.get("event") in terminal
                                         for row in direct)
        return dict(attested=valid, ready=valid and not revoked, revoked=revoked,
                    transport_closed=transport_closed,
                    inventory=bool(valid and workers <= inventory),
                    workers=workers, all_workers={row["childPid"] for row in observed
                                                 if row.get("event") == "spawn"},
                    rejected=rejected, forbidden=any(row.get("event") in errors for row in observed))

    def close(self):
        self.stopped.set()
        self.socket.close()
        self.thread.join(1)
        self.directory.cleanup()
