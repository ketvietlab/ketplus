"""Run the production Sparkle delegate against disposable loopback feeds."""
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import sys
import tempfile
import threading
import uuid


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        if self.path != "/appcast.xml":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/rss+xml")
        self.send_header("Content-Length", str(len(self.server.appcast)))
        self.end_headers()
        self.wfile.write(self.server.appcast)

    def log_message(self, *_):
        pass


def probe(server, source, executable_name, appcast, allowed):
    with tempfile.TemporaryDirectory(prefix="ketplus-architecture-") as temporary:
        root = Path(temporary)
        bundle = root / source.name
        shutil.copytree(source, bundle, symlinks=True)
        metadata = bundle / "Contents/Info.plist"
        info = plistlib.loads(metadata.read_bytes())
        # Sparkle requires a public key even for an information-only check.
        info.update(CFBundleExecutable=executable_name,
                    CFBundleIdentifier="dev.ketplus.tests.updater-architecture." + uuid.uuid4().hex,
                    SUFeedURL=f"http://127.0.0.1:{server.server_port}/appcast.xml",
                    SUPublicEDKey=base64.b64encode(bytes(32)).decode("ascii"))
        metadata.write_bytes(plistlib.dumps(info))
        subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", str(bundle)],
                       check=True, capture_output=True)
        server.appcast = appcast
        server.requests = []
        executable = bundle / "Contents/MacOS" / executable_name
        result = subprocess.run([str(executable), "--expect-offer" if allowed else "--expect-no-offer"],
                                env=dict(os.environ, HOME=str(root), CFFIXED_USER_HOME=str(root),
                                         KETPLUS_QA_SETTINGS_DIR=str(root / "settings")),
                                capture_output=True, text=True, timeout=15)
        if result.returncode or server.requests != ["/appcast.xml"]:
            raise RuntimeError(result.stdout + result.stderr + f"requests: {server.requests}")
        return result.stderr


def main(binary):
    source = binary.parents[2]
    executable = binary.name
    slices = subprocess.check_output(["/usr/bin/lipo", "-archs", str(binary)],
                                     text=True).split()
    arch = slices[0] if len(slices) == 1 else platform.machine()
    wrong = {"arm64": "x86_64", "x86_64": "arm64"}[arch]
    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        def item(version, architecture):
            tag = f"<sparkle:channel>ketplus-{architecture}</sparkle:channel>" if architecture else ""
            return (f"<item><sparkle:version>{version}</sparkle:version>{tag}"
                    f'<enclosure url="http://127.0.0.1:{server.server_port}/update.zip" '
                    f'length="1" type="application/zip"/></item>')

        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            cases = ((item(2, arch), True, None),
                     (item(2, wrong), False, "different architecture"),
                     (item(2, None), False, "missing or invalid architecture metadata"),
                     (item(2, "invalid!"), False, "missing or invalid architecture metadata"),
                     (item(3, wrong) + item(2, arch), True, "different architecture"))
            for items, expect_offer, diagnostic in cases:
                appcast = (f'<rss xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">'
                           f'<channel><title>QA</title>{items}</channel></rss>').encode()
                log = probe(server, source, executable, appcast, expect_offer)
                if diagnostic and diagnostic not in log:
                    raise RuntimeError(f"Architecture rejection was not logged: {log}")
        finally:
            server.shutdown()
            thread.join()
    print("Matching architecture offered; wrong and missing architecture rejected.")


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
