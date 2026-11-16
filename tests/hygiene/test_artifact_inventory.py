# SPDX-License-Identifier: MPL-2.0
"""Disposable staging and artifact checks; no SDK, signing, network or provider."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "packaging"))
import artifact_inventory as inventory
import artifact_gate as gate


class ArtifactInventoryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.stage = self.base / "stage"
        self.out = self.base / "out"
        self.stage.mkdir()
        fixtures = {
            "bin/node": b"\x7fELFsynthetic-node",
            "lib/libQt6Core.so": b"\x7fELFsynthetic-qt",
            "plugins/platforms/libqfixture.so": b"\x7fELFsynthetic-plugin",
            "fonts/fixture.ttf": b"synthetic-font",
            "icons/fixture.svg": b"<svg/>",
            "bin/launcher": b"#!/bin/sh\nexit 0\n",
            "node_modules/outer/package.json": b'{"name":"outer","version":"1.0.0","license":"MIT","repository":"https://github.com/example/outer"}',
            "node_modules/outer/index.js": b"// synthetic fixture",
            "node_modules/outer/LICENSE": b"Synthetic package notice",
            "node_modules/outer/esm/package.json": b'{"type":"module"}',
            "node_modules/outer/node_modules/inner/package.json": b'{"name":"inner","version":"2.0.0"}',
            "node_modules/outer/node_modules/inner/addon.node": b"\x7fELFsynthetic-addon",
            "node_modules/@fixture/scoped/package.json": b'{"name":"@fixture/scoped","version":"1.0.0"}',
            "licenses/NOTICE": b"Synthetic fixture copyright and permission text.",
            "sources/source-offer.txt": b"Synthetic matching-source and build instructions.",
            "audit/build.json": b'{"sourceCommit":"' + b"a" * 40 + b'"}',
            "audit/dependencies.txt": b"Synthetic native and embedded dependency audit.",
        }
        for name, data in fixtures.items():
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.stage / "bin/node").chmod(0o755)
        self.inv = inventory.write_evidence(self.stage, self.out, "a" * 40, "fixture-amd64", 1)
        (self.out / "fixture.pkg").write_bytes(b"synthetic artifact; never executed")
        (self.out / "BUILD-METADATA.json").write_text(json.dumps({"sourceCommit": "a" * 40}))
        self.evidence = inventory.bind_artifacts(self.out, [self.out / "fixture.pkg"],
                                                 self.out / "BUILD-METADATA.json")
        self.review = json.loads((self.out / "COMPONENT-REVIEW.template.json").read_text())
        for component in self.review["components"]:
            component.update(license="MIT", source="https://example.invalid/source",
                             notices=["licenses/NOTICE"], sourceMaterials=["sources/source-offer.txt"])
            if component["version"] == "NOASSERTION":
                component["version"] = "fixture-1"
        self.review.update(embeddedInventoryComplete=True, dependencyAuditComplete=True,
                           buildEvidence=["audit/build.json"], dependencyEvidence=["audit/dependencies.txt"],
                           artifactEvidenceSha256=inventory.object_digest(self.evidence))
        self.review["components"].append({
            "id": "embedded-fixture", "name": "Embedded fixture", "version": "1.0",
            "source": "https://example.invalid/embedded", "license": "MIT", "embedded": True,
            "files": ["bin/node"], "notices": ["licenses/NOTICE"],
            "sourceMaterials": ["sources/source-offer.txt"]})

    def check(self):
        return gate.check(self.stage, self.inv, self.review, self.evidence, self.out)

    def test_positive_inventory_covers_actual_files_and_nested_packages(self):
        result = self.check()
        self.assertTrue(result["inventoryLicenseGatePassed"], result)
        self.assertFalse(result["publicationAuthorized"])
        files = {entry["path"]: entry for entry in self.inv["files"]}
        for name, kind in (("bin/node", "node"), ("lib/libQt6Core.so", "native"),
                           ("plugins/platforms/libqfixture.so", "plugin-or-qml"),
                           ("fonts/fixture.ttf", "font"), ("icons/fixture.svg", "icon-or-image")):
            self.assertIn(kind, files[name]["kinds"])
        self.assertEqual(files["node_modules/outer/node_modules/inner/addon.node"]["component"],
                         "npm:node_modules/outer/node_modules/inner")
        sbom = json.loads((self.out / "STAGED-SBOM.spdx.json").read_text())
        self.assertEqual(len(sbom["files"]), len(files))
        self.assertGreater(len(sbom["packages"]), 3)
        self.assertIn("a" * 40, sbom["documentComment"])
        reviewed = gate.reviewed_spdx(self.inv, self.review)
        self.assertEqual(inventory.object_digest(reviewed), result["reviewedSbomSha256"])
        self.assertIn("Embedded fixture", [p["name"] for p in reviewed["packages"]])
        self.assertTrue(all(p["licenseConcluded"] == "MIT" for p in reviewed["packages"]))

    def test_npm_source_and_notice_reconciliation_keeps_approval_separate(self):
        template = json.loads((self.out / "COMPONENT-REVIEW.template.json").read_text())
        component = next(c for c in template["components"] if c["id"] == "npm:node_modules/outer")
        self.assertEqual(component["source"], "https://github.com/example/outer")
        self.assertEqual(component["license"], "MIT")
        self.assertEqual(component["notices"], ["node_modules/outer/LICENSE"])
        self.assertIn("node_modules/outer/index.js", component["sourceMaterials"])
        self.assertFalse(template["embeddedInventoryComplete"])
        self.assertEqual(inventory.public_npm_source("https://user:secret@github.com/example/repo"), "NOASSERTION")
        self.assertEqual(inventory.public_npm_source("https://example.invalid/repo"), "NOASSERTION")

    def test_template_is_not_release_ready(self):
        self.review = json.loads((self.out / "COMPONENT-REVIEW.template.json").read_text())
        errors = self.check()["errors"]
        self.assertIn("component-identity-or-license-unresolved", errors)
        self.assertIn("component-notices-missing", errors)
        self.assertIn("embedded-or-dependency-evidence-missing", errors)

    def test_unresolved_source_and_changed_npm_identity_fail(self):
        self.inv["sourceCommit"] = "NOASSERTION"
        self.assertIn("source-identity-unresolved", self.check()["errors"])
        npm = next(c for c in self.review["components"] if c["id"].startswith("npm:"))
        npm["version"] = "unrelated-version"
        self.assertIn("discovered-component-identity-changed", self.check()["errors"])

    def test_each_discovered_component_and_file_requires_coverage(self):
        self.review["components"] = self.review["components"][1:]
        self.assertIn("discovered-components-uncovered", self.check()["errors"])
        self.assertIn("staged-files-uncovered", self.check()["errors"])

    def test_missing_or_empty_notice_and_source_materials_fail(self):
        for field in ("notices", "sourceMaterials"):
            with self.subTest(field=field):
                saved = self.review["components"][0][field]
                self.review["components"][0][field] = ["missing.txt"]
                self.assertFalse(self.check()["inventoryLicenseGatePassed"])
                self.review["components"][0][field] = saved
        (self.stage / "licenses/NOTICE").write_bytes(b"")
        self.assertIn("component-notices-missing", self.check()["errors"])

    def test_new_file_bytes_and_mode_changes_invalidate_review(self):
        path = self.stage / "bin/node"
        original = path.read_bytes()
        path.write_bytes(b"\x7fELFchanged")
        self.assertIn("staging-changed", self.check()["errors"])
        path.write_bytes(original)
        path.chmod(0o644)
        self.assertIn("staging-changed", self.check()["errors"])
        (self.stage / "unexpected.dll").write_bytes(b"MZfixture")
        self.assertIn("staged-files-uncovered", self.check()["errors"])

    def test_artifact_inventory_sbom_and_review_binding_fail_closed(self):
        for name in ("fixture.pkg", "STAGED-INVENTORY.json", "STAGED-SBOM.spdx.json"):
            with self.subTest(name=name):
                path = self.out / name
                original = path.read_bytes()
                path.write_bytes(b"modified")
                self.assertIn("artifact-digest-mismatch", self.check()["errors"])
                path.write_bytes(original)
        self.review["inventorySha256"] = "0" * 64
        self.assertIn("review-inventory-mismatch", self.check()["errors"])
        self.evidence["files"][0]["sha256"] = "0" * 64
        self.assertIn("review-artifact-evidence-mismatch", self.check()["errors"])

    def test_artifact_source_identity_and_required_roles(self):
        metadata = self.out / "BUILD-METADATA.json"
        metadata.write_text(json.dumps({"sourceCommit": "b" * 40}))
        self.assertIn("artifact-source-identity-mismatch", self.check()["errors"])
        self.evidence["files"][0]["role"] = "sbom"
        self.assertIn("artifact-roles-incomplete", self.check()["errors"])

    def test_per_artifact_evidence_directory_and_path_fencing(self):
        nested = self.out / "deb-evidence"
        nested.mkdir()
        for name in ("STAGED-INVENTORY.json", "STAGED-SBOM.spdx.json"):
            shutil.copyfile(self.out / name, nested / name)
        self.evidence = inventory.bind_artifacts(nested, [self.out / "fixture.pkg"],
            self.out / "BUILD-METADATA.json", base_dir=self.out)
        self.review["artifactEvidenceSha256"] = inventory.object_digest(self.evidence)
        self.assertTrue(self.check()["inventoryLicenseGatePassed"])
        self.assertIn("deb-evidence/STAGED-INVENTORY.json", [f["name"] for f in self.evidence["files"]])
        for bad in ("../fixture.pkg", "/absolute.pkg", "deb-evidence/../fixture.pkg", "bad\\file"):
            self.evidence["files"][0]["name"] = bad
            self.assertIn("artifact-name-invalid", self.check()["errors"])

    def test_prefix_only_review_cannot_pass_for_linux_distribution(self):
        for extension, rule in (("deb", "final-deb-payload-incomplete"),
                                ("AppImage", "final-appimage-payload-incomplete")):
            artifact = self.out / ("fixture." + extension)
            artifact.write_bytes(b"synthetic distribution bytes")
            self.evidence = inventory.bind_artifacts(self.out, [artifact], self.out / "BUILD-METADATA.json")
            self.review["artifactEvidenceSha256"] = inventory.object_digest(self.evidence)
            self.assertIn(rule, self.check()["errors"])

    @unittest.skipIf(os.name == "nt", "Symlink creation may require privileges on Windows")
    def test_symlink_coverage_and_invalid_links(self):
        link = self.stage / "bin/node-link"
        link.symlink_to("node")
        result = inventory.inventory(self.stage, "a" * 40, "fixture", 1)
        self.assertEqual(next(entry for entry in result["files"] if entry["path"] == "bin/node-link")["target"], "node")
        for target in ("missing", str(self.out / "fixture.pkg"), "../../out/fixture.pkg", "node-link"):
            link.unlink()
            link.symlink_to(target)
            with self.assertRaises(ValueError):
                inventory.inventory(self.stage, "a" * 40, "fixture", 1)

    def test_no_overwrite_or_output_inside_stage(self):
        for out in (self.out, self.stage / "evidence"):
            with self.assertRaises(ValueError):
                inventory.write_evidence(self.stage, out, "a" * 40, "fixture", 1)

    def test_cli_malformed_input_is_redacted_and_returns_failure(self):
        bad = self.out / "bad.json"
        bad.write_text('{"untrusted":"do-not-echo-fixture"}')
        result = subprocess.run([sys.executable, str(ROOT / "packaging/artifact_gate.py"),
                                 "--stage", str(self.stage), "--inventory", str(bad),
                                 "--review", str(bad), "--evidence", str(bad),
                                 "--artifacts-dir", str(self.out)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stdout)["errors"], ["invalid-or-unreadable-input"])
        self.assertNotIn("do-not-echo-fixture", result.stdout + result.stderr)

    def test_cli_exports_reviewed_sbom_without_overwriting(self):
        review, evidence = self.out / "review.json", self.out / "ARTIFACT-EVIDENCE.json"
        review.write_bytes(inventory.encoded(self.review))
        evidence.write_bytes(inventory.encoded(self.evidence))
        output = self.out / "REVIEWED-SBOM.spdx.json"
        command = [sys.executable, str(ROOT / "packaging/artifact_gate.py"),
                   "--stage", str(self.stage), "--inventory", str(self.out / "STAGED-INVENTORY.json"),
                   "--review", str(review), "--evidence", str(evidence),
                   "--artifacts-dir", str(self.out), "--sbom-out", str(output)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["reviewedSbomSha256"], inventory.digest(output))
        original = output.read_bytes()
        repeated = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(repeated.returncode, 1)
        self.assertEqual(output.read_bytes(), original)

    @unittest.skipIf(os.name == "nt", "POSIX special file check")
    def test_special_file_fails_without_reading_it(self):
        os.mkfifo(self.stage / "pipe")
        with self.assertRaisesRegex(ValueError, "special file"):
            inventory.inventory(self.stage, "a" * 40, "fixture", 1)


if __name__ == "__main__":
    unittest.main()
