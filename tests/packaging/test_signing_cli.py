"""Exercise replacement signing without certificates, Apple services or provider calls."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "packaging/macos/sign-notarize-dmg.sh"
IDENTITY = "Developer ID Application: Test Company (TESTTEAM01)"


class SigningCliTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ketplus-sign-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "staged app/KetPlus.app"
        self.output = self.root / "signed app/KetPlus.app"
        for relative in ["MacOS/KetPlus", "Helpers/ketplus_daemon",
                         "Resources/ketplus-browser/bin/node", "Resources/foreign"]:
            path = self.source / "Contents" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(bytes.fromhex("cffaedfe") + b"synthetic")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.calls = self.root / "calls.jsonl"
        self.env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"],
                        SIGN_CALLS=str(self.calls))
        self.stub("ditto", "import shutil,sys\nshutil.copytree(sys.argv[1], sys.argv[2])\n")
        self.stub("defaults", "print('KetPlus')\n")
        self.stub("hdiutil", "raise SystemExit('app mode must not mount a DMG')\n")
        self.stub("xcrun", "raise SystemExit('sign-only must not contact Apple services')\n")
        self.stub("codesign", '''import json,os,sys
args = sys.argv[1:]
if "--sign" in args:
    with open(os.environ["SIGN_CALLS"], "a") as file:
        file.write(json.dumps(args) + "\\n")
elif "-dv" in args:
    team = "FOREIGN001" if args[-1].endswith("/foreign") else os.environ.get("FINAL_TEAM", "TESTTEAM01")
    print("Authority=Developer ID Application: Company (" + team + ")")
    print("TeamIdentifier=" + team)
elif "-r-" in args:
    print('designated => cdhash H"1234"' if os.environ.get("BAD_REQUIREMENT")
          else 'designated => identifier "dev.ketplus.editor" and anchor apple generic and certificate leaf[subject.OU] = "TESTTEAM01"')
elif "--entitlements" in args:
    print("<plist><dict><key>com.apple.security.cs.allow-jit</key><true/></dict></plist>")
''')

    def stub(self, name, body):
        path = self.bin / name
        path.write_text("#!" + sys.executable + "\n" + body)
        path.chmod(0o755)

    def run_sign(self, identity=IDENTITY, extra=(), env=None):
        return subprocess.run(
            ["/bin/bash", str(SCRIPT), "--app", str(self.source), "--output", str(self.output),
             "--identity", identity, "--skip-notarize", *extra],
            env=env or self.env, text=True, capture_output=True,
        )

    def test_copies_and_signs_inside_out_with_node_entitlements(self):
        result = self.run_sign()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        self.assertEqual(calls[-1][-1], str(self.output))
        node = next(call for call in calls if call[-1].endswith("/bin/node"))
        self.assertIn("--entitlements", node)
        self.assertTrue(node[node.index("--entitlements") + 1].endswith("ketplus-browser-node.entitlements"))
        helper = next(call for call in calls if call[-1].endswith("/Helpers/ketplus_daemon"))
        self.assertNotIn("--entitlements", helper)
        self.assertFalse(any(call[-1].endswith("/foreign") for call in calls))
        for call in calls:
            self.assertIn("--timestamp", call)
            self.assertEqual(call[call.index("--options") + 1], "runtime")
            self.assertEqual(call[call.index("--sign") + 1], IDENTITY)
            self.assertNotIn("--deep", call)
        self.assertEqual((self.source / "Contents/MacOS/KetPlus").read_bytes(),
                         (self.output / "Contents/MacOS/KetPlus").read_bytes())

    def test_rejects_ad_hoc_identity_before_copying(self):
        result = self.run_sign(identity="-")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Developer ID Application identity is required", result.stderr)
        self.assertFalse(self.output.exists())

    def test_rejects_development_identity(self):
        result = self.run_sign(identity="Apple Development: Test (TESTTEAM01)")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_rejects_both_app_and_dmg_inputs(self):
        dmg = self.root / "unsigned.dmg"
        dmg.touch()
        self.assertNotEqual(self.run_sign(extra=("--dmg", str(dmg))).returncode, 0)
        self.assertFalse(self.output.exists())

    def test_never_overwrites_an_existing_output(self):
        self.output.mkdir(parents=True)
        marker = self.output / "keep.txt"
        marker.write_text("keep")
        self.assertNotEqual(self.run_sign().returncode, 0)
        self.assertEqual(marker.read_text(), "keep")

    def test_rejects_a_build_specific_requirement(self):
        result = self.run_sign(env=dict(self.env, BAD_REQUIREMENT="1"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("build-specific cdhash", result.stderr)

    def test_rejects_a_different_team(self):
        result = self.run_sign(env=dict(self.env, FINAL_TEAM="WRONGTEAM1"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Team ID does not match", result.stderr)

    def test_dmg_still_signs_the_embedded_app_and_container(self):
        self.stub("hdiutil", '''import os,shutil,sys
from pathlib import Path
args = sys.argv[1:]
if args[0] == "convert":
    Path(args[args.index("-o") + 1]).touch()
elif args[0] == "attach":
    mount = Path(args[args.index("-mountpoint") + 1])
    shutil.copytree(os.environ["SIGN_SOURCE_APP"], mount / "KetPlus.app")
elif args[0] == "detach":
    shutil.rmtree(Path(args[-1]) / "KetPlus.app")
''')
        dmg = self.root / "unsigned.dmg"
        dmg.touch()
        output = self.root / "signed.dmg"
        result = subprocess.run(
            ["/bin/bash", str(SCRIPT), "--dmg", str(dmg), "--output", str(output),
             "--identity", IDENTITY, "--skip-notarize"],
            env=dict(self.env, SIGN_SOURCE_APP=str(self.source)), text=True, capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output.exists())
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        self.assertTrue(calls[-2][-1].endswith("/mount/KetPlus.app"))
        self.assertEqual(calls[-1][-1], str(output))
        self.assertIn("--timestamp", calls[-1])
        self.assertEqual(calls[-1][calls[-1].index("--sign") + 1], IDENTITY)


if __name__ == "__main__":
    unittest.main()
