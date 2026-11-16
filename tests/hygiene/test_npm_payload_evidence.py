# SPDX-License-Identifier: MPL-2.0
import base64
import hashlib
import io
import json
from pathlib import Path
import sys
import subprocess
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "packaging"))
from artifact_inventory import inventory
from npm_payload_evidence import archive_name, verify


class NpmPayloadEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.stage = self.root / "stage"
        self.archives = self.root / "archives"
        self.archives.mkdir()
        self.lock = {"packages": {}}

    def package(self, relative="node_modules/example", notices=True, extra=None):
        directory = self.stage / "runtime" / relative
        directory.mkdir(parents=True)
        entries = {"package.json": json.dumps({"name": "example", "version": "1.0.0", "license": "MIT"}).encode(),
                   "index.js": b"throw new Error('This file must never execute');"}
        if notices:
            entries["LICENSE"] = b"Synthetic fixture license, not a real permission grant"
        for name, data in entries.items():
            (directory / name).write_bytes(data)
        url = "https://registry.npmjs.org/example/-/" + relative.replace("/", "-") + ".tgz"
        archive = self.archives / archive_name(url)
        with tarfile.open(archive, "w:gz") as output:
            for name, data in {**entries, **(extra or {})}.items():
                member = tarfile.TarInfo("package/" + name)
                member.size = len(data)
                output.addfile(member, io.BytesIO(data))
        self.lock["packages"][relative] = {"version": "1.0.0", "license": "MIT", "resolved": url,
            "integrity": "sha512-" + base64.b64encode(hashlib.sha512(archive.read_bytes()).digest()).decode()}
        return directory, archive

    def report(self):
        inv = inventory(self.stage, "a" * 40, "fixture", 1)
        return verify(inv, {"runtime": self.lock}, self.archives)

    def test_nested_ownership_and_notice_absence_do_not_authorize_release(self):
        self.package(notices=False)
        self.package("node_modules/example/node_modules/example")
        report = self.report()
        self.assertTrue(report["allNpmFilesMatch"])
        self.assertFalse(report["publicationAuthorized"])
        self.assertEqual([3, 2], sorted([r["matchedFileCount"] for r in report["components"]], reverse=True))
        self.assertEqual(1, sum(not r["installedNotices"] for r in report["components"]))

    def test_changed_stage_and_corrupt_tarball_are_distinguished(self):
        directory, archive = self.package()
        (directory / "index.js").write_bytes(b"changed installed bytes")
        self.assertEqual("staged-files-differ", self.report()["components"][0]["issue"])
        archive.write_bytes(b"not the lock-pinned archive")
        self.assertEqual("archive-integrity-mismatch", self.report()["components"][0]["issue"])

    def test_credentials_and_missing_archives_remain_unverified(self):
        _, archive = self.package()
        archive.unlink()
        self.assertEqual("archive-not-supplied", self.report()["components"][0]["issue"])
        self.lock["packages"]["node_modules/example"]["resolved"] = "https://secret@registry.npmjs.org/package.tgz"
        report = self.report()
        self.assertEqual("non-public-registry-url", report["components"][0]["issue"])
        self.assertNotIn("secret", json.dumps(report))

    def test_lock_consistency_and_archive_traversal_are_required(self):
        self.package(extra={"../outside": b"never extract this"})
        self.assertEqual("archive-member-invalid", self.report()["components"][0]["issue"])
        self.lock["packages"]["node_modules/example"]["version"] = "2.0.0"
        self.assertEqual("lock-manifest-mismatch", self.report()["components"][0]["issue"])

    def test_cli_redacts_malformed_inputs_and_does_not_write_evidence(self):
        self.package()
        manifest = self.root / "inventory.json"
        manifest.write_text(json.dumps(inventory(self.stage, "a" * 40, "fixture", 1)))
        lock = self.root / "lock.json"
        lock.write_text('{"credential-marker": invalid')
        output = self.root / "evidence.json"
        result = subprocess.run([sys.executable, str(Path(__file__).resolve().parents[2] / "packaging/npm_payload_evidence.py"),
                                 "--stage", str(self.stage), "--inventory", str(manifest), "--archives", str(self.archives),
                                 "--lock", "runtime=" + str(lock), "--out", str(output)], capture_output=True)
        self.assertEqual(2, result.returncode)
        self.assertNotIn(b"credential-marker", result.stdout + result.stderr)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
