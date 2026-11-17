# SPDX-License-Identifier: MPL-2.0
import json
from pathlib import Path
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "packaging"))
sys.path.insert(0, str(REPO / "packaging/linux"))
from artifact_inventory import digest
from notice_supplements import install_supplements


class NoticeSupplementsTest(unittest.TestCase):
    def test_reviewed_notice_installs_without_overwrite_and_versions_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "stage/lib/node_modules/example"
            package.mkdir(parents=True)
            metadata = package / "package.json"
            metadata.write_text(json.dumps({"name": "example", "version": "1", "license": "MIT"}))
            notice = root / "notice.txt"
            notice.write_text("Synthetic upstream fixture notice")
            item = {"packageRoot": "lib/node_modules/example", "name": "example", "version": "1",
                    "declaredLicense": "MIT", "manifestSha256": digest(metadata),
                    "noticeFile": "notice.txt", "noticeSha256": digest(notice)}
            install_supplements(root / "stage", root, {"supplements": [item]})
            self.assertEqual(notice.read_bytes(), (package / "LICENSE").read_bytes())
            install_supplements(root / "stage", root, {"supplements": [item]})
            (package / "LICENSE").write_text("Different upstream notice")
            with self.assertRaisesRegex(ValueError, "never overwrite"):
                install_supplements(root / "stage", root, {"supplements": [item]})
            metadata.write_text(json.dumps({"name": "example", "version": "2", "license": "MIT"}))
            with self.assertRaisesRegex(ValueError, "package changed"):
                install_supplements(root / "stage", root, {"supplements": [item]})

    def test_escaping_package_is_rejected_before_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "stage").mkdir()
            (root / "outside").mkdir()
            (root / "stage/package").symlink_to(root / "outside", target_is_directory=True)
            with self.assertRaises(ValueError):
                install_supplements(root / "stage", root, {"supplements": [{"packageRoot": "package"}]})
            self.assertFalse((root / "outside/LICENSE").exists())

    def test_checked_in_notice_keeps_exact_reviewed_upstream_bytes(self):
        pin = json.loads((REPO / "packaging/linux/npm-notice-supplements.json").read_text())
        for item in pin["supplements"]:
            self.assertEqual(item["noticeSha256"], digest(REPO / item["noticeFile"]))


if __name__ == "__main__":
    unittest.main()
