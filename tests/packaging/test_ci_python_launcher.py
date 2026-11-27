"""Configure CTest with one Python image, then run it with another; no SDK or signing."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


LAUNCHER = Path(__file__).resolve().parents[2] / "packaging/macos/ci-python3"


def cmake_quote(value):
    return '"' + str(value).replace('\\', '/').replace('$', '\\$').replace('"', '\\"') + '"'


class CiPythonLauncherTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ketplus-ci-python-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source with spaces"
        self.source.mkdir()
        self.build = self.root / "restored build"
        self.trace = self.root / "interpreter-calls"
        self.cmake = shutil.which("cmake")
        self.ctest = shutil.which("ctest")
        self.assertIsNotNone(self.cmake, "CMake is required for the configuration regression")
        self.assertIsNotNone(self.ctest, "CTest is required for the artifact regression")
        self.old_bin = self.interpreter("build image")
        self.new_bin = self.interpreter("test image")

    def interpreter(self, name):
        directory = self.root / name
        directory.mkdir()
        executable = directory / "python3"
        executable.write_text(
            "#!/bin/sh\n"
            f"printf '%s\\n' {shlex.quote(name)} >> {shlex.quote(str(self.trace))}\n"
            f"exec {shlex.quote(sys.executable)} \"$@\"\n"
        )
        executable.chmod(0o755)
        return directory

    def environment(self, directory):
        return dict(os.environ, PATH=str(directory) + os.pathsep + os.environ.get("PATH", ""))

    def configure(self, signing_exit=0):
        probe = self.source / "probe.py"
        probe.write_text(
            "from pathlib import Path\nimport sys\n"
            "assert sys.argv[2:4] == ['--check', 'path with spaces and Tiếng Việt']\n"
            "Path(sys.argv[1]).write_text('executed')\n"
            "raise SystemExit(int(sys.argv[4]))\n",
            encoding="utf-8",
        )
        # The production registrations use both the variable and imported-target forms.
        self.source.joinpath("CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\nproject(PythonArtifact NONE)\n"
            "enable_testing()\nfind_package(Python3 REQUIRED COMPONENTS Interpreter)\n"
            f"set(probe {cmake_quote(probe)})\n"
            "set(args --check \"path with spaces and Tiếng Việt\")\n"
            "add_test(NAME ketplus_macos_signing_cli_tests COMMAND \"${Python3_EXECUTABLE}\" "
            f"\"${{probe}}\" {cmake_quote(self.root / 'signing.ran')} ${{args}} {signing_exit})\n"
            "add_test(NAME ketplus_team_translation_catalog_tests COMMAND Python3::Interpreter "
            f"\"${{probe}}\" {cmake_quote(self.root / 'catalog.ran')} ${{args}} 0)\n"
            "add_test(NAME ketplus_ssh_install_transaction_tests COMMAND \"${Python3_EXECUTABLE}\" "
            f"\"${{probe}}\" {cmake_quote(self.root / 'ssh.ran')} ${{args}} 77)\n"
            "set_tests_properties(ketplus_ssh_install_transaction_tests PROPERTIES SKIP_RETURN_CODE 77)\n",
            encoding="utf-8",
        )
        configured = subprocess.run(
            [self.cmake, "-S", str(self.source), "-B", str(self.build),
             f"-DPython3_EXECUTABLE={LAUNCHER}"],
            env=self.environment(self.old_bin), capture_output=True, text=True,
        )
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        generated = self.build.joinpath("CTestTestfile.cmake").read_text()
        self.assertIn(str(LAUNCHER), generated)
        self.assertNotIn(str(self.old_bin), generated)
        self.assertIn("build image", self.trace.read_text())
        # The build-image interpreter is genuinely unavailable when CTest starts.
        shutil.rmtree(self.old_bin)
        self.trace.unlink()

    def run_ctest(self, env):
        return subprocess.run(
            [self.ctest, "--test-dir", str(self.build), "--output-on-failure"],
            env=env, capture_output=True, text=True,
        )

    def test_restored_commands_use_the_test_images_interpreter(self):
        self.configure()
        result = self.run_ctest(self.environment(self.new_bin))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.trace.read_text().splitlines(), ["test image"] * 3)
        for name in ("signing.ran", "catalog.ran", "ssh.ran"):
            self.assertTrue(self.root.joinpath(name).is_file(), name)
        self.assertIn("(Skipped)", result.stdout)
        self.assertNotIn("Not Run", result.stdout)

    def test_child_failure_and_platform_skip_keep_their_status(self):
        self.configure(signing_exit=7)
        env = self.environment(self.new_bin)
        result = self.run_ctest(env)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("1 tests failed out of 3", result.stdout)
        self.assertIn("ketplus_ssh_install_transaction_tests (Skipped)", result.stdout)
        direct = subprocess.run([str(LAUNCHER), "-c", "raise SystemExit(7)"], env=env)
        self.assertEqual(direct.returncode, 7)

    def test_missing_runtime_python_fails_instead_of_skipping(self):
        self.configure()
        result = self.run_ctest(dict(os.environ, PATH=""))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("3 tests failed out of 3", result.stdout)
        self.assertNotIn("(Skipped)", result.stdout)
        self.assertFalse(self.root.joinpath("signing.ran").exists())


if __name__ == "__main__":
    unittest.main()
