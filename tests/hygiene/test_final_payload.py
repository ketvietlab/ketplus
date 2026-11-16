# SPDX-License-Identifier: MPL-2.0
"""Final extractor regression, including bytes outside the installed product prefix."""
import importlib.util
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "packaging"))
import artifact_inventory as inventory
spec = importlib.util.spec_from_file_location("payload_evidence", ROOT / "packaging/linux/payload_evidence.py")
payload = importlib.util.module_from_spec(spec)
spec.loader.exec_module(payload)


class FinalPayloadTest(unittest.TestCase):
    def fixture_archive(self, path, entries, prefix=b""):
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w") as archive:
            for name, contents in entries.items():
                member = tarfile.TarInfo(name)
                member.size = len(contents)
                archive.addfile(member, io.BytesIO(contents))
        path.write_bytes(prefix + data.getvalue())

    def extractor_fixture(self, *args, **kwargs):
        # Synthetic offline stand-ins for dpkg-deb/unsquashfs; read the assembled
        # archive bytes, not the original staging tree.
        if args[0] == "dpkg-deb":
            artifact, destination, offset = args[2], args[3], 0
        else:
            self.assertEqual(kwargs.get("umask"), 0)
            offset, destination, artifact = int(args[2]), args[5], args[6]
        destination.mkdir(parents=True)
        with artifact.open("rb") as stream:
            stream.seek(offset)
            with tarfile.open(fileobj=stream, mode="r|") as archive:
                archive.extractall(destination, filter="data")

    def test_deb_inventory_requires_wrappers_control_and_future_added_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = root / "fixture.deb"
            self.fixture_archive(artifact, {
                "opt/ketplus-pro/bin/KetPlus": b"\x7fELFproduct",
                "usr/bin/KetPlus": b"#!/bin/sh\nexec /opt/ketplus-pro/bin/KetPlus\n",
                "usr/lib/systemd/user/ketplus.service": b"[Service]\n",
                "opt/ketplus-pro/share/licenses/ketplus/LICENSE": b"Synthetic product license",
                "DEBIAN/control": b"Package: fixture\n", "DEBIAN/postinst": b"#!/bin/sh\nexit 0\n",
                "usr/share/new-future-component.txt": b"Must not disappear from evidence"})
            stage = root / "payload"
            payload.extract_payload(artifact, stage, "deb", self.extractor_fixture)
            inv = inventory.inventory(stage, "a" * 40, "fixture", 1)
            names = {entry["path"] for entry in inv["files"]}
            self.assertIn("DEBIAN/postinst", names)
            self.assertIn("usr/lib/systemd/user/ketplus.service", names)
            self.assertIn("usr/share/new-future-component.txt", names)
            self.assertEqual(len(names), 7)
            evidence = root / "deb-evidence"
            inventory.write_evidence(stage, evidence, "a" * 40, "fixture", 1)
            payload.reconcile_recipe_sources(stage, evidence, "deb", "a" * 40, "1.0.0", {})
            import json
            review = json.loads((evidence / "COMPONENT-REVIEW.template.json").read_text())
            wrapper = next(c for c in review["components"] if c["id"] == "file:usr/bin/KetPlus")
            self.assertEqual(wrapper["license"], "MPL-2.0")
            self.assertIn("a" * 40, wrapper["source"])
            self.assertTrue(wrapper["notices"])

    def test_appimage_inventory_contains_actual_runtime_prefix_and_approot(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime = root / "runtime"
            runtime.write_bytes(b"\x7fELFsynthetic-prefix")
            artifact = root / "fixture.AppImage"
            notice = b"Synthetic runtime and static dependency notice"
            self.fixture_archive(artifact, {"AppRun": b"#!/bin/sh\nexit 0\n",
                "fixture.desktop": b"[Desktop Entry]\n", "usr/bin/KetPlus": b"\x7fELFproduct",
                "usr/share/licenses/ketplus/LICENSE": b"Synthetic product license",
                "usr/share/licenses/ketplus/AppImage-runtime.txt": notice},
                runtime.read_bytes())
            artifact.chmod(0o755)
            stage = root / "payload"
            layout = payload.extract_payload(artifact, stage, "appimage", self.extractor_fixture, runtime)
            inv = inventory.inventory(stage, "a" * 40, "fixture", 1)
            names = {entry["path"] for entry in inv["files"]}
            self.assertIn("runtime-prefix", names)
            self.assertIn("rootfs/AppRun", names)
            self.assertIn("rootfs/fixture.desktop", names)
            self.assertEqual(len(names), 6)
            self.assertEqual((stage / "runtime-prefix").read_bytes(), runtime.read_bytes())
            self.assertEqual((stage / "runtime-prefix").stat().st_mode & 0o777, 0o755)
            self.assertEqual(layout["runtimePrefixBytes"], runtime.stat().st_size)
            evidence = root / "appimage-evidence"
            inventory.write_evidence(stage, evidence, "a" * 40, "fixture", 1)
            pin = {"sourceCommit": "b" * 40, "sourceUrl": "https://github.com/example/runtime/tree/" + "b" * 40,
                   "licenseSha256": inventory.digest(stage / "rootfs/usr/share/licenses/ketplus/AppImage-runtime.txt")}
            payload.reconcile_recipe_sources(stage, evidence, "appimage", "a" * 40, "1.0.0", pin)
            (stage / "rootfs/usr/share/licenses/ketplus/AppImage-runtime.txt").write_bytes(b"wrong")
            with self.assertRaisesRegex(ValueError, "notice"):
                payload.reconcile_recipe_sources(stage, evidence, "appimage", "a" * 40, "1.0.0", pin)
            artifact.write_bytes(b"X" + artifact.read_bytes()[1:])
            with self.assertRaisesRegex(ValueError, "differs"):
                payload.extract_payload(artifact, root / "bad", "appimage", self.extractor_fixture, runtime)


if __name__ == "__main__":
    unittest.main()
