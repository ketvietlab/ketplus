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


def make_feed(path, base_url, version, archive, signature):
    rss = ET.Element("rss", {"version": "2.0"})
    channel = ET.SubElement(rss, "channel")
    ET.SubElement(channel, "title").text = "Desktop Sparkle QA"
    item = ET.SubElement(channel, "item")
    ET.SubElement(item, "title").text = version
    ET.SubElement(item, f"{{{SPARKLE}}}version").text = version
    ET.SubElement(item, f"{{{SPARKLE}}}shortVersionString").text = version
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
                                         env=daemon_env, text=True, timeout=20)
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
    expected_url = f"http://127.0.0.1:{args.port}"
    if (not re.fullmatch(r"dev\.ketplus\.audit\.[a-z0-9-]+", identifier)
            or info.get("SUFeedURL") != expected_url + "/appcast.xml"):
        raise ValueError("Use a unique QA bundle ID and the requested HTTP loopback feed")
    feed = root / "feed"
    feed.mkdir(exist_ok=False)
    (feed / "update.zip").symlink_to(archive)
    signature = args.signature.read_text().strip()
    make_feed(feed, expected_url, args.version, archive, signature)
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
    executable = bundle / "Contents" / "MacOS" / info["CFBundleExecutable"]
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
                    stop(app)
            observations = [line for line in log_path.read_text(errors="replace").splitlines()
                            if line.startswith("sparkle-qa ")]
            for line in observations:
                print(line)
            install_ready = "sparkle-qa install on quit ready" in observations
            if install_ready:
                deadline = time.monotonic() + 20
                plist = bundle / "Contents" / "Info.plist"
                while time.monotonic() < deadline:
                    if plist.exists() and plistlib.loads(plist.read_bytes())["CFBundleVersion"] != version:
                        break
                    time.sleep(0.25)
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
    parser.add_argument("--interrupt", action="store_true")
    parser.add_argument("--retry", action="store_true")
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--platform", choices=("cocoa", "offscreen"), default="cocoa")
    parser.add_argument("--ctl", type=Path)
    parser.add_argument("--fake-codex", type=Path)
    parser.add_argument("--sign-update", type=Path)
    parser.add_argument("--signing-key", type=Path)
    with tempfile.TemporaryDirectory(prefix="kp4s-", dir="/tmp") as socket_directory:
        run_case(parser.parse_args(), socket_directory)
