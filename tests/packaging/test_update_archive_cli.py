"""Update preflight with synthetic bundles; no signatures, uploads or Apple services."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[2] / "packaging/macos/publish-update.sh"


class UpdateArchiveCliTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ketplus-update-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.app = self.root / "staged app/KetPlus.app"
        (self.app / "Contents/Frameworks/Sparkle.framework").mkdir(parents=True)
        self.updates = self.root / "updates"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls.jsonl"
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        UPDATE_CALLS=str(self.calls), UPDATE_VERSION="0.1.50")
        # Only the test copy redirects fixed Apple tool paths to deterministic stubs.
        self.script = self.root / "repo/packaging/macos/publish-update.sh"
        self.script.parent.mkdir(parents=True)
        source = SOURCE.read_text()
        for tool in ("/usr/libexec/PlistBuddy", "/usr/bin/codesign", "/usr/bin/ditto"):
            source = source.replace(tool, str(self.bin / Path(tool).name))
        self.script.write_text(source)
        self.stub("PlistBuddy", '''import os,sys
print(os.environ["UPDATE_VERSION"] if "CFBundleShortVersionString" in sys.argv[2]
      else "https://updates.example.invalid/appcast.xml")
''')
        self.stub("codesign", '''import sys
if "-dv" in sys.argv:
    print("Authority=Developer ID Application: Synthetic fixture", file=sys.stderr)
''')
        self.stub("ditto", '''from pathlib import Path
import json,os,sys
with open(os.environ["UPDATE_CALLS"], "a") as log: log.write(json.dumps(sys.argv) + "\\n")
Path(sys.argv[-1]).write_bytes(b"synthetic archive")
''')
        self.stub("generate_appcast", '''from pathlib import Path
import json,os,sys
with open(os.environ["UPDATE_CALLS"], "a") as log: log.write(json.dumps(sys.argv) + "\\n")
Path(sys.argv[sys.argv.index("-o") + 1]).write_text("synthetic appcast")
''')

    def stub(self, name, body):
        path = self.bin / name
        path.write_text("#!" + sys.executable + "\n" + body)
        path.chmod(0o755)

    def run_update(self, extra=()):
        return subprocess.run(
            ["/bin/bash", str(self.script), "--app", str(self.app),
             "--updates-dir", str(self.updates), "--download-url-prefix",
             "https://updates.example.invalid/", "--sparkle-bin", str(self.bin), *extra],
            env=self.env, capture_output=True, text=True, timeout=10)

    def assert_rejected_before_archive(self, result):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.calls.exists(), result.stderr)
        self.assertFalse((self.updates / "KetPlus-0.1.50.zip").exists())

    def test_prepares_valid_archive_without_notes(self):
        result = self.run_update()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.updates / "KetPlus-0.1.50.zip").read_bytes(), b"synthetic archive")
        self.assertEqual((self.updates / "appcast.xml").read_text(), "synthetic appcast")
        self.assertEqual(len(self.calls.read_text().splitlines()), 2)

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
                self.updates = self.root / ("updates-" + suffix)
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
