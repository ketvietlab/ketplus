"""Exercise the Sparkle uploader with a local, conditional S3 API fixture."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMMAND = Path(__file__).resolve().parents[2] / "packaging/macos/upload-update-r2.py"
SCOPE = "production/arm64/"
PREFIX = "https://updates.example.invalid/" + SCOPE

FAKE_AWS = '''import base64
import hashlib
import json
import os
from pathlib import Path
import sys

def fail(code):
    operation = "".join(part.title() for part in command.split("-"))
    sys.exit(f"An error occurred ({code}) when calling the {operation} operation: "
             + os.environ["AWS_ENDPOINT_URL_S3"] + " " + os.environ["AWS_SECRET_ACCESS_KEY"])

def option(name):
    return args[args.index(name) + 1] if name in args else None

def etag(data):
    return '"' + hashlib.md5(data).hexdigest() + '"'

api, command, *args = sys.argv[1:]
assert api == "s3api"
assert "--endpoint-url" not in args
key = option("--key")
root = Path(os.environ["FAKE_R2_ROOT"])
path = root / "objects" / key
old = path.read_bytes() if path.exists() else None
with (root / "calls.jsonl").open("a") as calls:
    calls.write(json.dumps([command, key, option("--content-type"), option("--cache-control"),
                            option("--if-none-match"), option("--if-match")]) + "\\n")
if command in ("get-object", "head-object"):
    if old is None:
        fail("NoSuchKey" if command == "get-object" else "404")
    if option("--if-match") not in (None, etag(old)):
        fail("PreconditionFailed")
    if command == "get-object":
        Path(args[-1]).write_bytes(old)
    print(json.dumps({"ContentLength": len(old), "ETag": etag(old)}))
elif command == "put-object":
    if os.environ.get("FAKE_R2_FAIL_KEY") == key:
        fail("AccessDenied")
    if os.environ.get("FAKE_R2_CONFLICT_KEY") == key:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"concurrent write")
        fail("PreconditionFailed")
    if old is None:
        assert option("--if-none-match") == "*"
    else:
        if option("--if-none-match") == "*" or option("--if-match") != etag(old):
            fail("PreconditionFailed")
    data = Path(option("--body")).read_bytes()
    assert base64.b64decode(option("--content-md5")) == hashlib.md5(data).digest()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    print(json.dumps({"ETag": etag(data)}))
else:
    raise AssertionError(command)
'''


class UpdateR2Test(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ketplus-r2-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.local = self.root / "updates" / SCOPE
        self.local.mkdir(parents=True)
        self.remote = self.root / "objects" / SCOPE
        self.calls = self.root / "calls.jsonl"
        fake = self.root / "aws"
        fake.write_text("#!" + sys.executable + "\n" + FAKE_AWS)
        fake.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.root) + os.pathsep + os.environ["PATH"],
                        AWS_ENDPOINT_URL_S3="https://fixture.r2.cloudflarestorage.com",
                        KETPLUS_R2_BUCKET="fixture-bucket", AWS_SECRET_ACCESS_KEY="fixture-secret",
                        AWS_CONFIG_FILE=str(self.root / "config"),
                        AWS_SHARED_CREDENTIALS_FILE=str(self.root / "credentials"),
                        FAKE_R2_ROOT=str(self.root))
        self.prepare()

    def prepare(self, version="1.2.3", mode="release", channel="production", arch="arm64",
                prefix=PREFIX, include_delta=False):
        archive = f"KetPlus-{version}.zip"
        (self.local / archive).write_bytes(b"synthetic zip")
        (self.local / f"KetPlus-{version}.md").write_text("embedded synthetic notes")
        delta = ""
        if include_delta:
            (self.local / "KetPlus1-0.delta").write_bytes(b"delta")
            delta = (f'<sparkle:deltas><enclosure url="{prefix}KetPlus1-0.delta" length="5" '
                     'sparkle:deltaFrom="1.0"/></sparkle:deltas>')
        mode_node = f"<ketplus:releaseMode>{mode}</ketplus:releaseMode>" if mode else ""
        feed = (f'<rss xmlns:ketplus="urn:ketplus:release" '
                'xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle"><channel>'
                f'<ketplus:releaseChannel>{channel}</ketplus:releaseChannel>'
                f'<ketplus:architecture>{arch}</ketplus:architecture>{mode_node}'
                f'<item><enclosure url="{prefix}{archive}" length="13"/>{delta}</item></channel></rss>')
        (self.local / "appcast.xml").write_text(feed)
        return feed.encode()

    def run_cli(self, *extra, snapshot=None, success=True):
        inputs = ["publish", "--updates-dir", str(self.local)]
        if snapshot is not None:
            inputs = ["rollback", "--snapshot", snapshot]
        result = subprocess.run(
            [sys.executable, "-I", str(COMMAND), *inputs, "--channel", "production", "--arch", "arm64",
             "--download-url-prefix", PREFIX, *extra],
            env=self.env, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result

    def writes(self):
        if not self.calls.exists():
            return []
        return [entry[1:] for entry in map(json.loads, self.calls.read_text().splitlines())
                if entry[0] == "put-object"]

    def test_dry_run_is_offline(self):
        self.env.pop("AWS_ENDPOINT_URL_S3")
        result = self.run_cli()
        self.assertIn(SCOPE + "KetPlus-1.2.3.zip", result.stdout)
        self.assertIn("Write last " + SCOPE + "appcast.xml", result.stdout)
        self.assertFalse(self.calls.exists())

    def test_upload_orders_assets_before_feed_and_sets_headers(self):
        self.prepare(include_delta=True)
        self.run_cli("--execute")
        self.assertEqual([(key, mime) for key, mime, *_ in self.writes()], [
            (SCOPE + "KetPlus-1.2.3.zip", "application/zip"),
            (SCOPE + "KetPlus1-0.delta", "application/octet-stream"),
            (SCOPE + "KetPlus-1.2.3.md", "text/markdown; charset=utf-8"),
            (SCOPE + "appcast.xml", "application/rss+xml"),
        ])
        for _, _, cache, condition, _ in self.writes()[:-1]:
            self.assertEqual(cache, "public, max-age=31536000, immutable")
            self.assertEqual(condition, "*")
        self.assertEqual(self.writes()[-1][2], "public, max-age=60, must-revalidate")

    def test_local_qa_and_unmarked_legacy_feed_rejected_before_s3(self):
        for mode in ("local-qa", None):
            with self.subTest(mode=mode):
                self.prepare(mode=mode)
                result = self.run_cli("--execute", success=False)
                self.assertIn("release mode", result.stderr)
                self.assertFalse(self.calls.exists())

    def test_identity_and_archive_prefix_must_match(self):
        for field in ({"channel": "staging"}, {"arch": "x86_64"},
                      {"prefix": "https://elsewhere.example.invalid/" + SCOPE},
                      {"prefix": PREFIX + "%2F"}):
            with self.subTest(field=field):
                self.prepare(**field)
                self.run_cli("--execute", success=False)
                self.assertFalse(self.calls.exists())

    def test_delta_and_external_notes_must_use_the_scoped_prefix(self):
        data = self.prepare(include_delta=True)
        feed = self.local / "appcast.xml"
        wrong_scope = b"https://updates.example.invalid/staging/arm64/"
        feed.write_bytes(data.replace(PREFIX.encode() + b"KetPlus1-0.delta", wrong_scope + b"KetPlus1-0.delta"))
        self.run_cli("--execute", success=False)
        feed.write_bytes(data.replace(b"</item>", b"<sparkle:releaseNotesLink>" + wrong_scope +
                                      b"notes.html</sparkle:releaseNotesLink></item>"))
        self.run_cli("--execute", success=False)
        self.assertFalse(self.calls.exists())

    def test_same_size_different_archive_is_refused_without_writes(self):
        self.remote.mkdir(parents=True)
        archive = self.remote / "KetPlus-1.2.3.zip"
        archive.write_bytes(b"different zip")
        result = self.run_cli("--execute", success=False)
        self.assertIn("immutable R2 object differs", result.stderr)
        self.assertEqual(archive.read_bytes(), b"different zip")
        self.assertEqual(self.writes(), [])

    def test_identical_retry_keeps_existing_objects(self):
        self.run_cli("--execute")
        self.calls.unlink()
        self.run_cli("--execute")
        self.assertEqual(self.writes(), [])

    def test_failed_asset_put_keeps_live_feed_and_does_not_print_secrets(self):
        old = self.prepare()
        self.run_cli("--execute")
        new = self.prepare("2.0")
        self.calls.unlink()
        self.env["FAKE_R2_FAIL_KEY"] = SCOPE + "KetPlus-2.0.md"
        result = self.run_cli("--execute", success=False)
        self.assertNotIn(self.env["AWS_ENDPOINT_URL_S3"], result.stderr + result.stdout)
        self.assertNotIn(self.env["AWS_SECRET_ACCESS_KEY"], result.stderr + result.stdout)
        self.assertEqual((self.remote / "appcast.xml").read_bytes(), old)
        self.calls.unlink()
        self.env.pop("FAKE_R2_FAIL_KEY")
        self.run_cli("--execute")
        self.assertEqual((self.remote / "appcast.xml").read_bytes(), new)
        self.assertNotIn(SCOPE + "KetPlus-2.0.zip", [write[0] for write in self.writes()])

    def test_concurrent_objects_cannot_be_overwritten(self):
        for name in ("KetPlus-1.2.3.zip", "appcast.xml"):
            with self.subTest(name=name):
                self.env["FAKE_R2_CONFLICT_KEY"] = SCOPE + name
                result = self.run_cli("--execute", success=False)
                self.assertIn("conditional R2 request conflicted", result.stderr)
                self.assertEqual((self.remote / name).read_bytes(), b"concurrent write")
                (self.remote / name).unlink()
                self.assertFalse((self.remote / "appcast.xml").exists())

    def test_concurrent_feed_replacement_keeps_the_snapshot(self):
        old = self.prepare()
        self.run_cli("--execute")
        self.prepare("2.0")
        self.env["FAKE_R2_CONFLICT_KEY"] = SCOPE + "appcast.xml"
        self.run_cli("--execute", success=False)
        snapshot = self.remote / "snapshots/1.2.3" / (hashlib.sha256(old).hexdigest() + ".xml")
        self.assertEqual(snapshot.read_bytes(), old)
        self.assertEqual((self.remote / "appcast.xml").read_bytes(), b"concurrent write")

    def test_rollback_keeps_archives_and_saves_the_displaced_feed(self):
        old = self.prepare()
        self.run_cli("--execute")
        new = self.prepare("2.0")
        self.calls.unlink()
        self.run_cli("--execute")
        snapshot = "1.2.3/" + hashlib.sha256(old).hexdigest() + ".xml"
        self.assertEqual([write[0] for write in self.writes()][-2:],
                         [SCOPE + "snapshots/" + snapshot, SCOPE + "appcast.xml"])
        self.assertEqual((self.remote / "snapshots" / snapshot).read_bytes(), old)
        self.assertEqual((self.remote / "appcast.xml").read_bytes(), new)
        self.assertIsNotNone(self.writes()[-1][-1])

        self.calls.unlink()
        self.run_cli(snapshot=snapshot)
        self.assertFalse(self.calls.exists())
        self.run_cli("--execute", snapshot=snapshot)
        self.assertEqual((self.remote / "appcast.xml").read_bytes(), old)
        self.assertTrue((self.remote / "KetPlus-2.0.zip").is_file())
        self.assertEqual([write[0] for write in self.writes()], [
            SCOPE + "snapshots/2.0/" + hashlib.sha256(new).hexdigest() + ".xml",
            SCOPE + "appcast.xml",
        ])

    def test_rollback_requires_snapshot_assets(self):
        old = self.prepare()
        self.run_cli("--execute")
        new = self.prepare("2.0")
        self.run_cli("--execute")
        (self.remote / "KetPlus-1.2.3.zip").unlink()
        self.calls.unlink()
        snapshot = "1.2.3/" + hashlib.sha256(old).hexdigest() + ".xml"
        self.run_cli("--execute", snapshot=snapshot, success=False)
        self.assertEqual(self.writes(), [])
        self.assertEqual((self.remote / "appcast.xml").read_bytes(), new)


if __name__ == "__main__":
    unittest.main()
