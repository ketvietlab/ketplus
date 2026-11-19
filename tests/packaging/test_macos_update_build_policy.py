"""Exercise the Sparkle configure preset and bundle verifier with synthetic inputs."""
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
VERIFY = ROOT / "packaging/macos/VerifySparkleBundle.cmake"
PUBLIC_KEY = "A" * 43 + "="
ORIGIN = "https://updates.example.invalid/nested"


class MacOSUpdateBuildPolicyTest(unittest.TestCase):
    cmake = os.environ.get("KETPLUS_TEST_CMAKE", "cmake")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ketplus-update-build-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sdk = self.root / "sdk"
        framework = self.sdk / "Sparkle.framework/Versions/B"
        framework.mkdir(parents=True)
        (framework / "Sparkle").touch()
        shutil.copyfile(ROOT / "CMakePresets.json", self.root / "CMakePresets.json")
        (self.root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.24)\n'
            'project(UpdateFixture NONE)\n'
            f'include("{ROOT}/cmake/Sparkle.cmake")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/updates-enabled" "${KETPLUS_UPDATES_ENABLED}")\n')
        self.bundle = self.root / "KetPlus.app"
        self.contents = self.bundle / "Contents"
        (self.contents / "MacOS").mkdir(parents=True)

    def call(self, cmd, expected=None, env=None):
        result = subprocess.run(cmd, cwd=self.root, env=env, capture_output=True, text=True, timeout=30)
        output = result.stdout + result.stderr
        if expected:
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn(expected, " ".join(output.split()), output)
        else:
            self.assertEqual(result.returncode, 0, output)
        return result

    def feed(self, channel="production", arch="arm64"):
        return f"{ORIGIN}/{channel}/{arch}/appcast.xml"

    def configure(self, preset="release-macos-updates", expected=None, channel="production", arch="arm64",
                  feed=None, key=PUBLIC_KEY):
        if feed is None:
            feed = self.feed(channel, arch)
        env = dict(os.environ, KETPLUS_UPDATE_CHANNEL=channel, KETPLUS_UPDATE_ARCH=arch,
                   KETPLUS_UPDATE_FEED_URL=feed, KETPLUS_UPDATE_PUBLIC_ED_KEY=key)
        self.call([self.cmake, "--preset", preset, "--fresh",
                   f"-DFETCHCONTENT_SOURCE_DIR_KETPLUS_SPARKLE={self.sdk}"],
                  expected=expected, env=env)
        if expected is None:
            path = self.root / "build" / preset
            if preset == "release-macos-updates":
                path = path / channel / arch
            return (path / "updates-enabled").read_text()

    def install_fixture(self, feed, arch, key=PUBLIC_KEY):
        framework = self.contents / "Frameworks/Sparkle.framework"
        if not framework.exists():
            shutil.copytree(self.sdk / "Sparkle.framework", framework)
            (framework / "Sparkle").symlink_to("Versions/B/Sparkle")
        (self.contents / "Info.plist").write_bytes(plistlib.dumps({
            "CFBundleExecutable": "ketplus-fixture", "SUFeedURL": feed,
            "SUPublicEDKey": key,
        }))
        self.call(["/usr/bin/clang", "-arch", arch, "-x", "c", "-c", "/dev/null",
                   "-o", str(self.contents / "MacOS/ketplus-fixture")])

    def verify(self, feed, arch, channel="production", enabled=True, expected=None):
        self.call([self.cmake, f"-DKETPLUS_APP_BUNDLE={self.bundle}",
                   f"-DKETPLUS_UPDATES_ENABLED={'ON' if enabled else 'OFF'}",
                   f"-DKETPLUS_UPDATE_FEED_URL={feed}",
                   f"-DKETPLUS_UPDATE_PUBLIC_ED_KEY={PUBLIC_KEY}",
                   f"-DKETPLUS_UPDATE_CHANNEL={channel}", f"-DKETPLUS_UPDATE_ARCH={arch}",
                   "-P", str(VERIFY)], expected=expected)

    def test_dev_and_generic_release_ignore_release_environment(self):
        for preset in ("dev", "release"):
            with self.subTest(preset=preset):
                self.assertEqual(self.configure(preset), "OFF")

    def test_opt_in_preset_accepts_each_thin_architecture(self):
        for channel, arch in (("production", "arm64"), ("staging", "x86_64")):
            with self.subTest(channel=channel, arch=arch):
                self.assertEqual(self.configure(channel=channel, arch=arch), "ON")

    def test_configure_rejects_missing_pair_and_bad_key(self):
        for feed, key, message in (
            ("", "", "This build requires"),
            (self.feed(), "", "must be set together"),
            ("", PUBLIC_KEY, "must be set together"),
            (self.feed(), "invalid", "must be the base64 public key"),
        ):
            with self.subTest(feed=feed, key=key):
                self.configure(feed=feed, key=key, expected=message)

    def test_configure_rejects_unscoped_feed_and_invalid_architecture(self):
        for feed in (self.feed(channel="staging"), self.feed(arch="x86_64"),
                     ORIGIN + "/appcast.xml", self.feed().replace("/nested/", "/../"),
                     self.feed().replace("/nested/", "/%2e%2e/")):
            with self.subTest(feed=feed):
                self.configure(feed=feed, expected="ending in /production/arm64/appcast.xml")
        self.configure(feed=self.feed() + "?branch=staging", expected="without a query")
        self.configure(arch="arm64;x86_64", feed=self.feed(), expected="exactly one explicit")
        self.configure(arch="", feed=self.feed(), expected="exactly one explicit")
        self.configure(channel="", feed=self.feed(), expected="KETPLUS_UPDATE_CHANNEL must be")

    def test_bundle_accepts_each_architecture_with_its_feed(self):
        for arch in ("arm64", "x86_64"):
            with self.subTest(arch=arch):
                feed = self.feed(arch=arch)
                self.install_fixture(feed, arch)
                self.verify(feed, arch)

    def test_bundle_rejects_matching_config_and_plist_with_wrong_scope(self):
        for feed in (self.feed(channel="staging"), self.feed(arch="x86_64")):
            with self.subTest(feed=feed):
                self.install_fixture(feed, "arm64")
                self.verify(feed, "arm64", expected="ending in /production/arm64/appcast.xml")

    def test_bundle_rejects_scope_correct_feed_on_wrong_binary_arch(self):
        self.install_fixture(self.feed(), "x86_64")
        self.verify(self.feed(), "arm64", expected="must contain only arm64")

    def test_bundle_requires_exact_configured_feed_and_key(self):
        self.install_fixture(self.feed(), "arm64")
        self.verify(self.feed().replace("/nested/", "/other/"), "arm64", expected="configured update feed and key")
        self.install_fixture(self.feed(), "arm64", key="B" + PUBLIC_KEY[1:])
        self.verify(self.feed(), "arm64", expected="configured update feed and key")

    def test_disabled_bundle_rejects_a_stray_key(self):
        plist = self.contents / "Info.plist"
        plist.write_bytes(plistlib.dumps({}))
        self.verify("", "", enabled=False)
        plist.write_bytes(plistlib.dumps({"SUPublicEDKey": PUBLIC_KEY}))
        self.verify("", "", enabled=False, expected="although updates are not configured")


if __name__ == "__main__":
    unittest.main()
