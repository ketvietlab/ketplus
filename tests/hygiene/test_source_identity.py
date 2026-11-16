# SPDX-License-Identifier: MPL-2.0
"""Build source identity must reflect the actual revision and unavailable/dirty inputs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SourceIdentityTest(unittest.TestCase):
    def test_clean_dirty_new_head_and_source_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo, hooks = root / "repo", root / "empty-hooks"
            repo.mkdir()
            hooks.mkdir()
            git = ["git", "-c", "commit.gpgsign=false", "-c", "core.hooksPath=" + str(hooks),
                   "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "-C", str(repo)]
            subprocess.run([*git, "init", "-q", "--template="], check=True, capture_output=True)
            source = repo / "fixture.cpp"
            source.write_text("// synthetic source\n")
            subprocess.run([*git, "add", "fixture.cpp"], check=True, capture_output=True)
            subprocess.run([*git, "commit", "-qm", "Create synthetic fixture"], check=True, capture_output=True)
            identity = root / "identity.h"

            def generate(source_dir):
                subprocess.run(["cmake", "-DSOURCE_DIR=" + str(source_dir), "-DOUTPUT=" + str(identity),
                    "-P", str(ROOT / "core/cmake/GenerateSourceIdentity.cmake")], check=True, capture_output=True)
                return identity.read_text()

            first = subprocess.check_output([*git, "rev-parse", "HEAD"], text=True).strip()
            self.assertIn('"' + first + '"', generate(repo))
            (repo / "new.cpp").write_text("// untracked compiled input\n")
            self.assertIn(first + "-dirty", generate(repo))
            (repo / "new.cpp").unlink()
            source.write_text("// changed synthetic source\n")
            self.assertIn(first + "-dirty", generate(repo))
            subprocess.run([*git, "commit", "-qam", "Update synthetic fixture"], check=True, capture_output=True)
            second = subprocess.check_output([*git, "rev-parse", "HEAD"], text=True).strip()
            self.assertNotEqual(first, second)
            self.assertIn('"' + second + '"', generate(repo))
            archive = root / "archive"
            archive.mkdir()
            self.assertIn('"unknown"', generate(archive))


if __name__ == "__main__":
    unittest.main()
