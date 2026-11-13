"""Update preflight with synthetic bundles; no signatures, uploads or Apple services."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


SOURCE = Path(__file__).resolve().parents[2] / "packaging/macos/publish-update.sh"


class UpdateArchiveCliTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ketplus-update-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.app = self.root / "staged app/KetPlus.app"
        (self.app / "Contents/Frameworks/Sparkle.framework").mkdir(parents=True)
        self.updates = self.root / "updates/production/arm64"
        self.updates.parent.mkdir(parents=True)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls.jsonl"
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        UPDATE_CALLS=str(self.calls), UPDATE_VERSION="0.1.50")
        # Only the test copy redirects fixed Apple tool paths to deterministic stubs.
        self.script = self.root / "repo/packaging/macos/publish-update.sh"
        self.script.parent.mkdir(parents=True)
        source = SOURCE.read_text()
        for tool in ("/usr/libexec/PlistBuddy", "/usr/bin/codesign", "/usr/bin/ditto", "/usr/bin/xcrun", "/usr/bin/lipo"):
            source = source.replace(tool, str(self.bin / Path(tool).name))
        self.script.write_text(source)
        self.script.with_name("update-release-policy.py").write_text(SOURCE.with_name("update-release-policy.py").read_text())
        (self.app / "Contents/MacOS").mkdir()
        (self.app / "Contents/MacOS/ketplus").write_text("synthetic Mach-O")
        self.stub("PlistBuddy", '''import os,sys
key = sys.argv[2]
if "CFBundleShortVersionString" in key: print(os.environ["UPDATE_VERSION"])
elif "CFBundleExecutable" in key: print("ketplus")
else: print(os.environ.get("UPDATE_FEED", "https://updates.example.invalid/production/arm64/appcast.xml"))
''')
        self.stub("lipo", '''import os
print(os.environ.get("UPDATE_ARCH", "arm64"))
''')
        self.stub("codesign", '''import os,sys
if "--verify" in sys.argv and os.environ.get("UPDATE_INVALID_SIGNATURE"): sys.exit(1)
if "-dv" in sys.argv and os.environ.get("UPDATE_DEVELOPER_ID", "1") == "1":
    print("Authority=Developer ID Application: Synthetic fixture", file=sys.stderr)
''')
        self.stub("xcrun", '''import os,sys
if os.environ.get("UPDATE_TICKET", "1") != "1": sys.exit(1)
''')
        self.stub("ditto", '''from pathlib import Path
import json,os,sys
with open(os.environ["UPDATE_CALLS"], "a") as log: log.write(json.dumps(sys.argv) + "\\n")
Path(sys.argv[-1]).write_bytes(b"synthetic archive")
''')
        # Like Sparkle 2.10, an existing -o file is the feed to update and must not be empty;
        # rewriting it in place keeps its mode.
        self.stub("generate_appcast", '''from pathlib import Path
import json,os,sys,xml.etree.ElementTree as ET
with open(os.environ["UPDATE_CALLS"], "a") as log: log.write(json.dumps(sys.argv) + "\\n")
output = Path(sys.argv[sys.argv.index("-o") + 1])
if output.exists():
    root = ET.fromstring(output.read_text())
else:
    root = ET.Element("rss"); ET.SubElement(ET.SubElement(root,"channel"),"title").text="synthetic appcast"
channel = root.find("channel")
ET.SubElement(channel, "description").text = "synthetic item"
prefix = sys.argv[sys.argv.index("--download-url-prefix") + 1]
url = os.environ.get("UPDATE_GENERATED_URL", prefix + "KetPlus-" + os.environ["UPDATE_VERSION"] + ".zip")
ET.SubElement(ET.SubElement(channel,"item"),"enclosure",url=url)
output.write_bytes(ET.tostring(root))
if os.environ.get("UPDATE_APPCAST_FAIL"): sys.exit("synthetic signing failure")
''')

    def feed(self, channel="production", architecture="arm64"):
        return ('<rss xmlns:ketplus="urn:ketplus:release"><channel><title>published appcast</title>'
                + "<ketplus:releaseChannel>" + channel + "</ketplus:releaseChannel>"
                + "<ketplus:architecture>" + architecture + "</ketplus:architecture></channel></rss>")

    def stub(self, name, body):
        path = self.bin / name
        path.write_text("#!" + sys.executable + "\n" + body)
        path.chmod(0o755)

    def run_update(self, extra=()):
        return subprocess.run(
            ["/bin/bash", str(self.script), "--app", str(self.app),
             "--updates-dir", str(self.updates), "--download-url-prefix",
             "https://updates.example.invalid/production/arm64/", "--sparkle-bin", str(self.bin), "--channel", "production", "--arch", "arm64", *extra],
            env=self.env, capture_output=True, text=True, timeout=10)

    def assert_rejected_before_archive(self, result):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.calls.exists(), result.stderr)
        self.assertFalse((self.updates / "KetPlus-0.1.50.zip").exists())

    def test_release_requires_developer_id(self):
        self.env["UPDATE_DEVELOPER_ID"] = "0"
        result = self.run_update()
        self.assert_rejected_before_archive(result)
        self.assertIn("Developer ID", result.stderr)

    def test_release_requires_stapled_ticket(self):
        self.env["UPDATE_TICKET"] = "0"
        result = self.run_update()
        self.assert_rejected_before_archive(result)
        self.assertIn("notarization ticket", result.stderr)

    def test_local_qa_explicitly_allows_ad_hoc_without_ticket(self):
        self.env.update(UPDATE_DEVELOPER_ID="0", UPDATE_TICKET="0")
        result = self.run_update(["--mode", "local-qa"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Do not publish", result.stderr)

    def test_local_qa_still_requires_valid_signature(self):
        self.env["UPDATE_INVALID_SIGNATURE"] = "1"
        self.assert_rejected_before_archive(self.run_update(["--mode", "local-qa"]))

    def test_outside_key_symlink_into_checkout_is_rejected(self):
        inside = self.root / "repo/synthetic-key"
        inside.write_text("inert fixture, not signing material")
        alias = self.root / "outside-key"
        alias.symlink_to(inside)
        result = self.run_update(["--ed-key-file", str(alias)])
        self.assert_rejected_before_archive(result)
        self.assertIn("must not live in the repository", result.stderr)

    def test_case_alias_of_checkout_key_is_rejected(self):
        inside = self.root / "repo/synthetic-key"
        inside.write_text("inert fixture")
        alias = self.root / "REPO/synthetic-key"
        if not alias.exists():
            self.skipTest("filesystem is case-sensitive")
        result = self.run_update(["--ed-key-file", str(alias)])
        self.assert_rejected_before_archive(result)
        self.assertIn("must not live in the repository", result.stderr)

    def test_local_qa_accepts_http_loopback_feed(self):
        prefix = "http://127.0.0.1:4443/production/arm64/"
        self.env.update(UPDATE_FEED=prefix + "appcast.xml", UPDATE_DEVELOPER_ID="0", UPDATE_TICKET="0")
        result = self.run_update(["--mode", "local-qa", "--download-url-prefix", prefix])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.updates / "appcast.xml").is_file())

    def test_http_is_refused_for_release_and_nonloopback_qa(self):
        for mode, host in (("release", "127.0.0.1"), ("local-qa", "updates.example.invalid"),
                           ("local-qa", "127.0.0.1.example.invalid")):
            prefix = f"http://{host}/production/arm64/"
            self.env["UPDATE_FEED"] = prefix + "appcast.xml"
            with self.subTest(mode=mode, host=host):
                self.assert_rejected_before_archive(self.run_update(
                    ["--mode", mode, "--download-url-prefix", prefix]))

    def test_existing_sparkle_feed_seeding_preserves_every_byte(self):
        self.updates.mkdir()
        feed = self.updates / "appcast.xml"
        original = ('<?xml version="1.0"?><rss xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" '
                    'xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:ketplus="urn:ketplus:release">'
                    '<channel><title>ketPlus Pro</title><ketplus:releaseChannel>production</ketplus:releaseChannel>'
                    '<ketplus:architecture>arm64</ketplus:architecture><!-- preserve this -->'
                    '<item><sparkle:version>1</sparkle:version><sparkle:shortVersionString>0.1.1</sparkle:shortVersionString>'
                    '<enclosure url="https://updates.example.invalid/production/arm64/KetPlus-0.1.1.zip" '
                    'sparkle:edSignature="synthetic-signature" /></item></channel></rss>').encode()
        feed.write_bytes(original)
        for operation in ("seed-feed", "seed-feed", "check-feed"):
            result = subprocess.run([sys.executable, str(self.script.with_name("update-release-policy.py")),
                operation, "--channel", "production", "--arch", "arm64", "--download-prefix",
                "https://updates.example.invalid/production/arm64/", "--appcast", str(feed)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(feed.read_bytes(), original)

    def test_new_feed_declares_namespaces_used_by_the_real_generator(self):
        self.updates.mkdir()
        feed = self.updates / "appcast.xml"
        result = subprocess.run([sys.executable, str(self.script.with_name("update-release-policy.py")),
            "seed-feed", "--channel", "production", "--arch", "arm64", "--download-prefix",
            "https://updates.example.invalid/production/arm64/", "--appcast", str(feed)],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        xml = feed.read_text()
        self.assertIn('xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle"', xml)
        self.assertIn('xmlns:dc="http://purl.org/dc/elements/1.1/"', xml)
        # generate_appcast appends prefixed nodes to the existing seed rather than
        # creating a new RSS root with its own namespace declarations.
        generated = xml.replace('</channel>', '<item><sparkle:version>2</sparkle:version>'
            '<dc:creator>fixture</dc:creator><enclosure sparkle:edSignature="fixture" />'
            '</item></channel>')
        import xml.etree.ElementTree as ET
        item = ET.fromstring(generated).find('channel/item')
        self.assertEqual(item.find('{http://www.andymatuschak.org/xml-namespaces/sparkle}version').text, '2')

    def test_external_key_symlink_is_resolved_before_signer(self):
        target = self.root / "external-key"
        target.write_text("inert fixture")
        alias = self.root / "external-alias"
        alias.symlink_to(target)
        result = self.run_update(["--ed-key-file", str(alias)])
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls.read_text()
        self.assertIn(str(target), calls)
        self.assertNotIn(str(alias), calls)

    def test_mismatched_channel_or_arch_inputs_are_rejected(self):
        for extra in (["--channel", "staging"], ["--arch", "x86_64"], ["--channel", "unknown"]):
            with self.subTest(extra=extra):
                self.assert_rejected_before_archive(self.run_update(extra))

    def test_each_channel_and_architecture_has_a_separate_feed(self):
        for channel in ("staging", "production"):
            for arch in ("arm64", "x86_64"):
                with self.subTest(channel=channel, arch=arch):
                    self.updates = self.root / "matrix" / channel / arch
                    self.updates.parent.mkdir(parents=True, exist_ok=True)
                    prefix = f"https://updates.example.invalid/{channel}/{arch}/"
                    self.env["UPDATE_FEED"] = prefix + "appcast.xml"
                    self.env["UPDATE_ARCH"] = arch
                    result = self.run_update(["--channel", channel, "--arch", arch,
                                              "--download-url-prefix", prefix])
                    self.assertEqual(result.returncode, 0, result.stderr)
                    node = ET.parse(self.updates / "appcast.xml").getroot().find("channel")
                    self.assertEqual(node.find("{urn:ketplus:release}releaseChannel").text, channel)
                    self.assertEqual(node.find("{urn:ketplus:release}architecture").text, arch)
                    self.assertTrue(node.find("item/enclosure").get("url").startswith(prefix))

    def test_bundle_architecture_must_match(self):
        self.env["UPDATE_ARCH"] = "x86_64"
        self.assert_rejected_before_archive(self.run_update())

    def test_compiled_feed_must_match_identity(self):
        self.env["UPDATE_FEED"] = "https://updates.example.invalid/staging/arm64/appcast.xml"
        self.assert_rejected_before_archive(self.run_update())

    def test_output_and_download_prefix_must_match_identity(self):
        self.assert_rejected_before_archive(self.run_update(["--updates-dir", str(self.root / "production/x86_64")]))
        self.assert_rejected_before_archive(self.run_update(["--download-url-prefix", "https://updates.example.invalid/staging/arm64/"]))

    def test_existing_appcast_wrong_identity_is_preserved(self):
        self.updates.mkdir()
        feed = self.updates / "appcast.xml"
        feed.write_text(self.feed(channel="staging"))
        original = feed.read_bytes()
        self.assert_rejected_before_archive(self.run_update())
        self.assertEqual(feed.read_bytes(), original)

    def test_generated_appcast_wrong_archive_url_does_not_replace_feed(self):
        self.updates.mkdir()
        feed = self.updates / "appcast.xml"
        feed.write_text(self.feed())
        original = feed.read_bytes()
        self.env["UPDATE_GENERATED_URL"] = "https://updates.example.invalid/production/x86_64/wrong.zip"
        result = self.run_update()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(feed.read_bytes(), original)
        self.assertFalse((self.updates / "KetPlus-0.1.50.zip").exists())

    def test_appcast_records_release_identity(self):
        result = self.run_update()
        self.assertEqual(result.returncode, 0, result.stderr)
        node = ET.parse(self.updates / "appcast.xml").getroot().find("channel")
        self.assertEqual(node.find("{urn:ketplus:release}releaseChannel").text, "production")
        self.assertEqual(node.find("{urn:ketplus:release}architecture").text, "arm64")
        self.assertTrue(node.find("item/enclosure").get("url").startswith("https://updates.example.invalid/production/arm64/"))

    def test_prepares_valid_archive_without_notes(self):
        result = self.run_update()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.updates / "KetPlus-0.1.50.zip").read_bytes(), b"synthetic archive")
        self.assertIn("<title>ketPlus Pro</title>", (self.updates / "appcast.xml").read_text())
        self.assertEqual(len(self.calls.read_text().splitlines()), 2)

    def test_failed_signing_removes_the_partial_release(self):
        self.updates.mkdir()
        (self.updates / "appcast.xml").write_text(self.feed())
        notes = self.root / "notes.md"
        notes.write_text("Release notes")
        self.env["UPDATE_APPCAST_FAIL"] = "1"
        result = self.run_update(("--release-notes", str(notes)))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse((self.updates / "KetPlus-0.1.50.zip").exists())
        self.assertFalse((self.updates / "KetPlus-0.1.50.md").exists())
        self.assertEqual((self.updates / "appcast.xml").read_text(), self.feed())
        # The same version can be prepared once signing works.
        del self.env["UPDATE_APPCAST_FAIL"]
        result = self.run_update(("--release-notes", str(notes)))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.updates / "KetPlus-0.1.50.zip").is_file())
        self.assertIn("<description>synthetic item</description>", (self.updates / "appcast.xml").read_text())
        self.assertEqual(sorted(path.name for path in self.root.iterdir()
                                if path.name.startswith(".ketplus-appcast")), [])

    def test_updated_appcast_keeps_its_mode(self):
        self.updates.mkdir()
        published = self.updates / "appcast.xml"
        published.write_text(self.feed())
        published.chmod(0o644)
        result = self.run_update()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("<description>synthetic item</description>", published.read_text())
        self.assertEqual(published.stat().st_mode & 0o777, 0o644)

    def test_output_failure_after_the_appcast_keeps_the_release(self):
        # The status lines are written after appcast.xml is replaced; a closed stdout fails them.
        result = subprocess.run(
            ["/bin/bash", "-c", 'exec 1>&-; exec /bin/bash "$@"', "update", str(self.script),
             "--app", str(self.app), "--updates-dir", str(self.updates),
             "--download-url-prefix", "https://updates.example.invalid/production/arm64/",
             "--sparkle-bin", str(self.bin), "--channel", "production", "--arch", "arm64"],
            env=self.env, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assertIn("<title>ketPlus Pro</title>", (self.updates / "appcast.xml").read_text())
        self.assertTrue((self.updates / "KetPlus-0.1.50.zip").is_file())

    def test_accepts_filename_safe_prerelease_version(self):
        self.env["UPDATE_VERSION"] = "1.2.3-beta.1+build_2"
        result = self.run_update()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.updates / "KetPlus-1.2.3-beta.1+build_2.zip").is_file())

    def test_rejects_unsafe_or_empty_versions_before_writing(self):
        for version in ("", "../outside", "1/../../outside", "1.2\n3", "1.2 3"):
            with self.subTest(version=version):
                self.env["UPDATE_VERSION"] = version
                self.assert_rejected_before_archive(self.run_update())
                self.assertFalse(self.updates.exists())

    def test_prepares_html_and_markdown_notes(self):
        for suffix in ("html", "md"):
            with self.subTest(suffix=suffix):
                self.updates = self.root / ("updates-" + suffix) / "production/arm64"
                notes = self.root / ("release notes." + suffix)
                notes.write_text("Notes content")
                result = self.run_update(("--release-notes", str(notes)))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual((self.updates / ("KetPlus-0.1.50." + suffix)).read_text(),
                                 "Notes content")

    def test_invalid_note_extension_does_not_leave_archive(self):
        notes = self.root / "notes.txt"
        notes.write_text("invalid extension")
        self.assert_rejected_before_archive(self.run_update(("--release-notes", str(notes))))
        self.assertFalse(self.updates.exists())

    def test_missing_notes_do_not_leave_archive(self):
        self.assert_rejected_before_archive(
            self.run_update(("--release-notes", str(self.root / "missing.md"))))
        self.assertFalse(self.updates.exists())

    def test_directory_notes_do_not_leave_archive(self):
        notes = self.root / "notes.md"
        notes.mkdir()
        self.assert_rejected_before_archive(self.run_update(("--release-notes", str(notes))))

    def test_existing_archive_is_preserved(self):
        self.updates.mkdir()
        archive = self.updates / "KetPlus-0.1.50.zip"
        archive.write_bytes(b"existing release")
        result = self.run_update()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(archive.read_bytes(), b"existing release")
        self.assertFalse(self.calls.exists())

    def test_dangling_archive_symlink_does_not_write_outside_directory(self):
        self.updates.mkdir()
        outside = self.root / "outside.zip"
        archive = self.updates / "KetPlus-0.1.50.zip"
        archive.symlink_to(outside)
        self.assert_rejected_before_archive(self.run_update())
        self.assertTrue(archive.is_symlink())
        self.assertFalse(outside.exists())

    def test_existing_notes_are_preserved_before_archiving(self):
        self.updates.mkdir()
        destination = self.updates / "KetPlus-0.1.50.md"
        destination.write_text("existing notes")
        source = self.root / "new.md"
        source.write_text("new notes")
        self.assert_rejected_before_archive(self.run_update(("--release-notes", str(source))))
        self.assertEqual(destination.read_text(), "existing notes")

    def test_dangling_note_symlink_does_not_write_outside_directory(self):
        self.updates.mkdir()
        outside = self.root / "outside.md"
        destination = self.updates / "KetPlus-0.1.50.md"
        destination.symlink_to(outside)
        source = self.root / "new.md"
        source.write_text("new notes")
        self.assert_rejected_before_archive(self.run_update(("--release-notes", str(source))))
        self.assertTrue(destination.is_symlink())
        self.assertFalse(outside.exists())

    def test_repair_invalid_notes_and_retry_same_version(self):
        self.assert_rejected_before_archive(
            self.run_update(("--release-notes", str(self.root / "missing.md"))))
        notes = self.root / "missing.md"
        notes.write_text("repaired")
        result = self.run_update(("--release-notes", str(notes)))
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
