# SPDX-License-Identifier: MPL-2.0
"""Offline negative checks for the packaging trust boundary."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("linux_packages", ROOT / "packaging/linux/build-packages.py")
packages = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packages)


class LinuxDesktopPackagesTest(unittest.TestCase):
    def test_launcher_preserves_original_environment_for_real_children(self):
        names = ('LD_LIBRARY_PATH', 'QT_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH',
                 'QTWEBENGINEPROCESS_PATH', 'QTWEBENGINE_RESOURCES_PATH', 'QTWEBENGINE_LOCALES_PATH')
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            (prefix / 'bin').mkdir()
            launcher = prefix / 'ketplus-pro'
            shutil.copyfile(ROOT / 'packaging/linux/run-desktop.sh', launcher)
            launcher.chmod(0o755)
            # Every launcher route spawns another real process. No mocked environment API.
            program = '#!' + sys.executable + '\nimport subprocess, sys\nsubprocess.run([sys.executable, "-c", "import json, os; print(json.dumps(dict(os.environ)))"], check=True)\n'
            for name in ('KetPlus', 'ketplus_daemon'):
                binary = prefix / 'bin' / name
                binary.write_text(program)
                binary.chmod(0o755)
            for value in (None, '', 'original user value with spaces'):
                environment = dict(os.environ)
                for name in names:
                    environment.pop(name, None)
                    if value is not None:
                        environment[name] = value
                for args in ([], ['--daemon'], ['--build-info']):
                    with self.subTest(value=value, args=args):
                        result = subprocess.run([str(launcher), *args], env=environment,
                                                text=True, capture_output=True, check=True)
                        actual = json.loads(result.stdout)
                        self.assertEqual({key: actual[key] for key in names if key in actual},
                                         {key: environment[key] for key in names if key in environment})

    def test_cached_runtime_is_verified_before_use(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            runtime = cache / "runtime-x86_64"
            runtime.write_bytes(b"\x7fELFfixture")
            pin = {"assets": {"x86_64": {"sha256": packages.digest(runtime)}}}
            self.assertEqual(packages.verified_runtime(cache, "x86_64", pin), runtime)
            runtime.write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                packages.verified_runtime(cache, "x86_64", pin)

    def test_unverified_download_is_not_cached(self):
        with tempfile.TemporaryDirectory() as directory:
            from io import BytesIO
            pin = {"assets": {"aarch64": {"url": "https://example.invalid/runtime",
                                            "sha256": "0" * 64}}}
            with patch.object(packages.urllib.request, "urlopen", return_value=BytesIO(b"\x7fELFwrong")):
                with self.assertRaisesRegex(ValueError, "SHA-256"):
                    packages.verified_runtime(Path(directory), "aarch64", pin)
            self.assertFalse((Path(directory) / "runtime-aarch64").exists())

    def test_cache_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            (cache / "target").write_bytes(b"fixture")
            (cache / "runtime-x86_64").symlink_to("target")
            with self.assertRaisesRegex(ValueError, "symlink"):
                packages.verified_runtime(cache, "x86_64", {"assets": {"x86_64": {}}})

    def test_production_configuration_is_required(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "CMakeCache.txt").write_text("KETPLUS_EDITION:STRING=pro\nCMAKE_BUILD_TYPE:STRING=Release\nKETPLUS_PRODUCTION_RELEASE:BOOL=OFF\nKETPLUS_LINUX_DESKTOP_PACKAGE:BOOL=ON\n")
            with self.assertRaisesRegex(ValueError, "PRODUCTION_RELEASE"):
                packages.read_cache(build)

    def test_wrong_installed_identity_is_rejected(self):
        class Result:
            stdout = json.dumps({"control_url": "https://example.invalid", "control_url_overridable": True})
        with patch.object(packages, "run", return_value=Result()):
            with self.assertRaisesRegex(ValueError, "production control"):
                packages.build_info(Path("/unused"), "https://api.ketplus.app")

    def test_incomplete_runtime_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "Incomplete installed runtime"):
                packages.validate_prefix(Path(directory))

    def test_dirty_build_identity_is_rejected(self):
        class Result:
            stdout = json.dumps({"control_url": "https://api.ketplus.app",
                                 "control_url_overridable": False,
                                 "build_revision": "0123456789ab-dirty"})
        with patch.object(packages, "run", return_value=Result()):
            with self.assertRaisesRegex(ValueError, "clean public source"):
                packages.build_info(Path("/unused"), "https://api.ketplus.app")

    def test_architecture_must_be_explicitly_supported(self):
        self.assertEqual(packages.architecture("aarch64"), ("aarch64", "arm64"))
        self.assertEqual(packages.architecture("x86_64"), ("x86_64", "amd64"))
        with self.assertRaises(ValueError):
            packages.architecture("riscv64")


if __name__ == "__main__":
    unittest.main()
