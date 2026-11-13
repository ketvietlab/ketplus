"""Throwaway Ed25519 archive signatures and per-architecture feed policy; no network/client launch."""
import base64
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

POLICY = Path(__file__).resolve().parents[2] / "packaging/macos/update-release-policy.py"
SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
RELEASE = "urn:ketplus:release"
ET.register_namespace("sparkle", SPARKLE)
ET.register_namespace("ketplus", RELEASE)


class QaArchitectureFeedTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ketplus-qa-feed-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.openssl = os.environ.get("OPENSSL") or shutil.which("openssl")
        self.assertTrue(self.openssl, "OpenSSL with Ed25519 support is required")
        self.key, self.public = self.keypair("qa")

    def command(self, *args):
        return subprocess.run([self.openssl, *map(str, args)], capture_output=True, timeout=10)

    def keypair(self, name):
        key, public = self.root / (name + ".pem"), self.root / (name + ".pub")
        self.assertEqual(self.command("genpkey", "-algorithm", "ED25519", "-out", key).returncode, 0)
        self.assertEqual(self.command("pkey", "-in", key, "-pubout", "-out", public).returncode, 0)
        return key, public

    def sign(self, archive):
        result = self.command("pkeyutl", "-sign", "-rawin", "-inkey", self.key, "-in", archive)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(len(result.stdout), 64)
        return result.stdout

    def verify(self, archive, signature, public=None):
        sig = self.root / "signature.bin"
        sig.write_bytes(signature)
        return self.command("pkeyutl", "-verify", "-rawin", "-pubin", "-inkey",
                            public or self.public, "-in", archive, "-sigfile", sig).returncode == 0

    def policy(self, operation, arch, prefix, *extra):
        return subprocess.run([sys.executable, str(POLICY), operation, "--mode", "local-qa",
            "--channel", "staging", "--arch", arch, "--download-prefix", prefix, *map(str, extra)],
            capture_output=True, timeout=10)

    def test_signed_qa_feeds_are_architecture_scoped(self):
        for arch, other in (("arm64", "x86_64"), ("x86_64", "arm64")):
            with self.subTest(arch=arch):
                directory = self.root / "staging" / arch
                directory.mkdir(parents=True)
                archive = directory / "synthetic.zip"
                archive.write_bytes(("synthetic archive for " + arch).encode())
                signature = self.sign(archive)
                self.assertTrue(self.verify(archive, signature))
                prefix = "http://127.0.0.1:4443/staging/" + arch + "/"
                rss = ET.Element("rss", version="2.0")
                channel = ET.SubElement(rss, "channel")
                ET.SubElement(channel, "{" + RELEASE + "}releaseChannel").text = "staging"
                ET.SubElement(channel, "{" + RELEASE + "}architecture").text = arch
                ET.SubElement(ET.SubElement(channel, "item"), "enclosure", {
                    "url": prefix + archive.name,
                    "{" + SPARKLE + "}edSignature": base64.b64encode(signature).decode(),
                    "length": str(archive.stat().st_size),
                })
                feed = directory / "appcast.xml"
                ET.ElementTree(rss).write(feed, encoding="utf-8", xml_declaration=True)
                self.assertEqual(self.policy("check-feed", arch, prefix, "--appcast", feed).returncode, 0)
                self.assertEqual(self.policy("paths", arch, prefix, "--updates-dir", directory,
                    "--compiled-feed", prefix + "appcast.xml").returncode, 0)
                other_prefix = prefix.replace("/" + arch + "/", "/" + other + "/")
                self.assertNotEqual(self.policy("check-feed", other, other_prefix,
                    "--appcast", feed).returncode, 0)
                self.assertNotEqual(self.policy("paths", other, other_prefix, "--updates-dir", directory,
                    "--compiled-feed", prefix + "appcast.xml").returncode, 0)

    def test_qa_key_refuses_tampered_unsigned_and_wrong_key_archives(self):
        archive = self.root / "synthetic.zip"
        archive.write_bytes(b"synthetic archive")
        signature = self.sign(archive)
        self.assertTrue(self.verify(archive, signature))
        self.assertFalse(self.verify(archive, b""))
        _, wrong_public = self.keypair("wrong-qa")
        self.assertFalse(self.verify(archive, signature, wrong_public))
        archive.write_bytes(b"tampered archive")
        self.assertFalse(self.verify(archive, signature))


if __name__ == "__main__":
    unittest.main()
