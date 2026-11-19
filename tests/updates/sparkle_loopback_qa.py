#!/usr/bin/env python3
"""Run a manually staged, ad-hoc signed Desktop QA bundle against Sparkle 2.10."""

import argparse
from contextlib import contextmanager
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import plistlib
import re
import signal
import socket
import subprocess
import tempfile
import threading
import time
import xml.etree.ElementTree as ET


SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
ET.register_namespace("sparkle", SPARKLE)


class FeedHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        archive_request = self.path == "/update.zip"
        if archive_request:
            print("loopback: archive Range header present:", "Range" in self.headers, flush=True)
        if archive_request and self.server.interrupt:
            if self.server.retry:
                self.server.interrupt = False
            archive = Path(self.directory) / "update.zip"
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Length", str(archive.stat().st_size))
            self.end_headers()
            with archive.open("rb") as source:
                self.wfile.write(source.read(64 * 1024))
            self.wfile.flush()
            self.connection.shutdown(socket.SHUT_RDWR)
            self.close_connection = True
            print("loopback: closed archive response after 65536 bytes", flush=True)
        else:
            super().do_GET()

    def log_message(self, message, *args):
        print("loopback:", message % args, flush=True)


def make_feed(path, base_url, version, archive, signature, architecture):
    rss = ET.Element("rss", {"version": "2.0"})
    channel = ET.SubElement(rss, "channel")
    ET.SubElement(channel, "title").text = "Desktop Sparkle QA"
    item = ET.SubElement(channel, "item")
    ET.SubElement(item, "title").text = version
    ET.SubElement(item, f"{{{SPARKLE}}}version").text = version
    ET.SubElement(item, f"{{{SPARKLE}}}shortVersionString").text = version
    if architecture != "missing":
        ET.SubElement(item, f"{{{SPARKLE}}}channel").text = "ketplus-" + architecture
    ET.SubElement(item, "enclosure", {
        "url": base_url + "/update.zip",
        "length": str(archive.stat().st_size),
        "type": "application/octet-stream",
        f"{{{SPARKLE}}}edSignature": signature,
    })
    ET.ElementTree(rss).write(path / "appcast.xml", encoding="utf-8", xml_declaration=True)


def verify_bundle(bundle):
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(bundle)], check=True)
    return plistlib.loads((bundle / "Contents" / "Info.plist").read_bytes())


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


@contextmanager
def active_turn_probe(args, bundle, root, env):
    if not args.ctl and not args.fake_codex:
        yield lambda: None
        return
    if not args.ctl or not args.fake_codex or args.fake_codex.name != "codex":
        raise ValueError("Supply ketplusctl and the executable fake provider named codex together")
    fake_codex = args.fake_codex.resolve(strict=True)
    if not fake_codex.is_relative_to(root.parent):
        raise ValueError("Use a mock provider in the disposable QA workspace")
    daemon_executable = bundle / "Contents" / "Helpers" / "ketplus_daemon"
    daemon_env = dict(env, PATH=f"{fake_codex.parent}:/usr/bin:/bin")
    del daemon_env["DYLD_INSERT_LIBRARIES"]
    build_info = subprocess.check_output([str(daemon_executable), "--build-info"],
                                         env=daemon_env, text=True, timeout=45)
    if "http://127.0.0.1:9" not in build_info:
        raise ValueError("The fixture daemon must be built with a loopback control URL")
    worktree = root / "workspace"
    worktree.mkdir()
    subprocess.run(["git", "init", "-q", str(worktree)], env=daemon_env, check=True)

    def call(operation, payload):
        return subprocess.run([str(args.ctl.resolve()), "--timeout", "3", "call",
                               operation, json.dumps(payload)], env=daemon_env,
                              text=True, capture_output=True, timeout=5)

    with (root / "daemon.log").open("wb") as log:
        daemon = subprocess.Popen([str(daemon_executable), "--codex", str(fake_codex)],
                                  env=daemon_env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 10
            while not Path(env["KETPLUS_DAEMON_SOCKET"]).exists() and daemon.poll() is None:
                if time.monotonic() >= deadline:
                    break
                time.sleep(0.10)
            started = call("session.start_turn", {
                "worktree_path": str(worktree), "provider": "codex", "prompt": "hang",
            })
            if started.returncode:
                raise RuntimeError("Could not start a turn using the isolated fake provider")
            session_id = json.loads(started.stdout)["session_id"]

            def active_sequence():
                catalog = call("session.list", {})
                catalog.check_returncode()
                for session in json.loads(catalog.stdout)["sessions"]:
                    if session["session_id"] == session_id and session["state"] == "running":
                        return session["current_turn_sequence"]
                return None

            sequence = active_sequence()
            deadline = time.monotonic() + 10
            while sequence is None and time.monotonic() < deadline:
                time.sleep(0.10)
                sequence = active_sequence()
            if sequence is None:
                raise RuntimeError("The fake turn did not reach the running state")

            def busy():
                if daemon.poll() is not None or active_sequence() != sequence:
                    raise RuntimeError("The original fake turn is no longer running")
                result = call("daemon.prepare_upgrade", {"replacement_version": "9999.0"})
                if (result.returncode != 1 or
                        "An active turn prevents daemon replacement." not in result.stderr):
                    raise RuntimeError("The isolated daemon did not report an active turn")
                print("isolated daemon: original process reports daemon_busy", flush=True)

            busy()
            yield busy
        finally:
            stop(daemon)


class SparkleJobs:
    """Track Sparkle 2.10's one-shot jobs for this disposable bundle."""

    def __init__(self, identifier, bundle, root):
        self.identifier = identifier
        self.bundle = bundle
        self.root = root
        self.seen = set()
        self.exit_statuses = set()
        self.since = time.strftime("%Y-%m-%d %H:%M:%S")
        domains = (f"gui/{os.getuid()}", f"user/{os.getuid()}", "system")
        self.targets = [f"{domain}/{identifier}-sparkle-{role}"
                        for domain in domains for role in ("updater", "progress")]

    def observe(self):
        jobs = set()
        processes = set()
        for target in self.targets:
            result = subprocess.run(["launchctl", "print", target], text=True,
                                    capture_output=True, timeout=5)
            if result.returncode:
                if ("Could not find service" in result.stderr or
                        "Could not find domain" in result.stderr):
                    continue
                raise RuntimeError(f"Could not inspect the QA Sparkle job ({result.returncode})")
            jobs.add(target)
            pid = re.search(r"^\s*pid = (\d+)", result.stdout, re.MULTILINE)
            if pid:
                processes.add(int(pid[1]))
            status = re.search(r"^\s*last exit code = (-?\d+)\s*$", result.stdout, re.MULTILINE)
            if status:
                self.exit_statuses.add((target.rsplit("-", 1)[1], status[1]))

        listing = subprocess.check_output(["ps", "-Ao", "pid=,comm="], text=True, timeout=5)
        helpers = set()
        for line in listing.splitlines():
            fields = line.strip().split(maxsplit=1)
            if len(fields) == 2 and Path(fields[1]).name in {
                    "Autoupdate", "Installer", "InstallerLauncher", "Updater"}:
                helpers.add(fields[0])
        listing = subprocess.check_output(["ps", "-Ao", "pid=,args="], text=True, timeout=5)
        qa_id = re.compile(rf"(?:^|\s){re.escape(self.identifier)}(?:\s|$)")
        bundle_path = str(self.bundle)
        for line in listing.splitlines():
            fields = line.strip().split(maxsplit=1)
            if len(fields) != 2:
                continue
            pid, command = fields
            if pid in helpers and (qa_id.search(command) or bundle_path + "/" in command
                                   or command.endswith(" " + bundle_path)
                                   or " " + bundle_path + " " in command):
                processes.add(int(pid))
        self.seen.update(processes)
        return jobs, processes

    def installer_log(self):
        if not self.seen:
            return
        pids = " OR ".join(f"processIdentifier == {pid}" for pid in sorted(self.seen))
        with (self.root / "installer.log").open("wb") as output:
            subprocess.run(["/usr/bin/log", "show", "--start", self.since, "--style", "compact",
                            "--predicate", f'({pids}) AND category == "Sparkle"'],
                           stdout=output, stderr=subprocess.STDOUT, check=True, timeout=25)

    def drain(self, timeout):
        deadline = time.monotonic() + timeout
        quiet_since = None
        while True:
            jobs, processes = self.observe()
            now = time.monotonic()
            if not jobs and not processes:
                quiet_since = now if quiet_since is None else quiet_since
                if now - quiet_since >= 1:
                    print(f"Sparkle helpers stopped: observed {len(self.seen)}, zero remaining",
                          flush=True)
                    for role, status in sorted(self.exit_statuses):
                        print(f"Sparkle {role} last exit code: {status}", flush=True)
                    return True
            else:
                quiet_since = None
            if now >= deadline:
                break
            time.sleep(0.15)

        for target in jobs:
            subprocess.run(["launchctl", "bootout", target], capture_output=True, timeout=5)
        for pid in self.observe()[1]:
            try:
                os.kill(pid, signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                pass
        deadline = time.monotonic() + 5
        remaining = self.observe()
        while any(remaining) and time.monotonic() < deadline:
            time.sleep(0.15)
            remaining = self.observe()
        if any(remaining):
            raise RuntimeError("QA Sparkle helpers still running after cancellation; keep the volume mounted")
        return False


def clear_qa_preferences(identifier):
    result = subprocess.run(["/usr/bin/defaults", "delete", identifier], text=True,
                            capture_output=True, timeout=10)
    if result.returncode and "not found" not in result.stderr.lower():
        raise RuntimeError("Could not remove the disposable Sparkle preference domain")
    print("Disposable Sparkle preference domain removed", flush=True)


def volume_snapshot(volume):
    entries = set()
    inaccessible = set()

    def scan(directory):
        for path, directories, files in os.walk(
                directory, onerror=lambda err: inaccessible.add(err.filename)):
            entries.update(str((Path(path) / name).relative_to(volume))
                           for name in directories + files)

    scan(volume)
    # Cocoa creates .TemporaryItems without directory listing permission, but
    # the owner can traverse it to that user's replacement staging directory.
    temporary = volume / ".TemporaryItems"
    user_area = temporary / f"folders.{os.getuid()}"
    if str(temporary) in inaccessible and user_area.is_dir():
        entries.add(str(user_area.relative_to(volume)))
        scan(user_area)
    return entries, inaccessible


def compare_volume(volume, before, case_root):
    entries, inaccessible = volume_snapshot(volume)
    added = sorted(entries - before[0])
    removed = sorted(before[0] - entries)
    report = {"added": added, "removed": removed,
              "unreadable": sorted(before[1] | inaccessible)}
    (case_root / "volume-diff.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Destination volume: {len(added)} added, {len(removed)} removed, "
          f"{len(report['unreadable'])} unreadable paths", flush=True)


def run_case(args, socket_directory):
    root, bundle, trigger, archive = (path.resolve() for path in
                                      (args.root, args.bundle, args.trigger, args.archive))
    if not root.is_dir() or not bundle.is_relative_to(root) or not archive.is_file():
        raise ValueError("Use a bundle staged in a disposable root and an existing signed archive")
    if args.retry and not args.interrupt:
        raise ValueError("--retry requires --interrupt")
    info = verify_bundle(bundle)
    version = info["CFBundleVersion"]
    identifier = info["CFBundleIdentifier"]
    executable = bundle / "Contents" / "MacOS" / info["CFBundleExecutable"]
    architecture = args.item_architecture
    if architecture is None:
        architecture = subprocess.check_output(["/usr/bin/lipo", "-archs", str(executable)], text=True).strip()
        if architecture not in ("arm64", "x86_64"):
            raise ValueError("Use a thin QA bundle or specify --item-architecture")
    expected_url = f"http://127.0.0.1:{args.port}"
    if (not re.fullmatch(r"dev\.ketplus\.audit\.[a-z0-9-]+", identifier)
            or info.get("SUFeedURL") != expected_url + "/appcast.xml"):
        raise ValueError("Use a unique QA bundle ID and the requested HTTP loopback feed")
    feed = root / "feed"
    feed.mkdir(exist_ok=False)
    (feed / "update.zip").symlink_to(archive)
    signature = args.signature.read_text().strip()
    make_feed(feed, expected_url, args.version, archive, signature, architecture)
    if bool(args.sign_update) != bool(args.signing_key):
        raise ValueError("Supply the Sparkle signing tool and its throwaway key together")
    if args.signing_key:
        signing_key = args.signing_key.resolve(strict=True)
        checkout = Path(__file__).resolve().parents[2]
        if signing_key.is_relative_to(checkout):
            raise ValueError("The throwaway signing key must stay outside the checkout")
        subprocess.run([str(args.sign_update.resolve(strict=True)), "-f", str(signing_key),
                        str(feed / "appcast.xml")], check=True, capture_output=True)
    qa_home = root / "home"
    qa_home.mkdir(exist_ok=False)
    (qa_home / "settings").mkdir()
    env = dict(os.environ,
               HOME=str(qa_home), CFFIXED_USER_HOME=str(qa_home),
               KETPLUS_SPARKLE_QA_ROOT=str(root),
               KETPLUS_SPARKLE_QA_RETRY="1" if args.retry else "0",
               KETPLUS_QA_SETTINGS_DIR=str(qa_home / "settings"),
               KETPLUS_DISABLE_DAEMON_AUTOSTART="1",
               KETPLUS_DAEMON_SOCKET=str(Path(socket_directory) / "s"),
               KETPLUS_DAEMON_TOKEN_FILE=str(qa_home / "daemon.token"),
               KETPLUS_DAEMON_STATE=str(qa_home / "state"),
               KETPLUS_CONTROL_URL="http://127.0.0.1:9",
               QT_QPA_PLATFORM=args.platform,
               DYLD_INSERT_LIBRARIES=str(trigger))
    volume = bundle.parent
    if args.inspect_volume and not volume.is_mount():
        raise ValueError("--inspect-volume requires the app at the mounted volume root")
    before_volume = volume_snapshot(volume) if args.inspect_volume else None
    jobs = SparkleJobs(identifier, bundle, root)
    if any(jobs.observe()):
        raise RuntimeError("A Sparkle job is already using this QA bundle identifier")
    handler = partial(FeedHandler, directory=str(feed))
    with ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server, \
            active_turn_probe(args, bundle, root, env) as check_daemon:
        server.interrupt = args.interrupt
        server.retry = args.retry
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            log_path = root / "desktop.log"
            with log_path.open("wb") as log:
                app = subprocess.Popen([str(executable)], env=env, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + args.timeout
                    while app.poll() is None and time.monotonic() < deadline:
                        content = log_path.read_text(errors="replace")
                        if ("sparkle-qa install on quit ready" in content or
                                content.count("sparkle-qa cycle=") >= (2 if args.retry else 1)):
                            break
                        time.sleep(0.25)
                finally:
                    try:
                        # Observe the installer before it receives the app termination.
                        jobs.observe()
                    finally:
                        stop(app)
                        drained_normally = jobs.drain(args.installer_timeout)
                        clear_qa_preferences(identifier)
                        jobs.installer_log()
                        if not drained_normally:
                            raise RuntimeError("QA Sparkle helpers timed out and were stopped; installation is unverified")
            observations = [line for line in log_path.read_text(errors="replace").splitlines()
                            if line.startswith("sparkle-qa ")]
            for line in observations:
                print(line)
            install_ready = "sparkle-qa install on quit ready" in observations
            if before_volume is not None:
                compare_volume(volume, before_volume, root)
            check_daemon()
            result = verify_bundle(bundle)["CFBundleVersion"]
            print(f"Strict bundle verification passed; build {version} -> {result}")
            cycles = sum(line.startswith("sparkle-qa cycle=") for line in observations)
            if not install_ready and cycles < (2 if args.retry else 1):
                raise RuntimeError("No Sparkle update-cycle result before exit or timeout")
        finally:
            server.shutdown()
            thread.join()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "bundle", "trigger", "archive", "signature"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--item-architecture", choices=("arm64", "x86_64", "missing"))
    parser.add_argument("--interrupt", action="store_true")
    parser.add_argument("--retry", action="store_true")
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--installer-timeout", type=int, default=45)
    parser.add_argument("--inspect-volume", action="store_true")
    parser.add_argument("--platform", choices=("cocoa", "offscreen"), default="cocoa")
    parser.add_argument("--ctl", type=Path)
    parser.add_argument("--fake-codex", type=Path)
    parser.add_argument("--sign-update", type=Path)
    parser.add_argument("--signing-key", type=Path)
    with tempfile.TemporaryDirectory(prefix="kp4s-", dir="/tmp") as socket_directory:
        run_case(parser.parse_args(), socket_directory)
