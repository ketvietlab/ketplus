"""Checks which Actions cache entries tools/ci/prune_caches.py deletes. No network."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ci"))
import prune_caches  # noqa: E402

HASH_A = "a" * 64
HASH_B = "b" * 64


def entry(id_, key, ref="refs/pull/7/merge", created="2026-11-19T01:00:00Z", size=100):
    return {"id": id_, "key": key, "ref": ref, "created_at": created, "size_in_bytes": size}


class PruneTest(unittest.TestCase):
    def test_keeps_only_the_newest_entry_of_a_family_per_ref(self):
        entries = [
            entry(1, "fetchcontent-pro-macOS-ARM64-clang-qt-6.11.1-" + HASH_A,
                  created="2026-11-18T01:00:00Z"),
            entry(2, "fetchcontent-pro-macOS-ARM64-clang-qt-6.11.1-" + HASH_B,
                  created="2026-11-19T01:00:00Z"),
            entry(3, "fetchcontent-pro-macOS-ARM64-clang-qt-6.11.1-" + HASH_A,
                  ref="refs/heads/develop", created="2026-11-17T01:00:00Z"),
            entry(4, "fetchcontent-lite-Linux-X64-gcc-qt-6.10.*-" + HASH_A,
                  created="2026-11-17T01:00:00Z"),
        ]
        self.assertEqual([e["id"] for e in prune_caches.plan(entries, set())], [1])

    def test_leaves_entries_without_a_hashed_family(self):
        entries = [
            entry(1, "sccache/f/f/e/ffedb2aa429a713162e88e5c6cd8859fdcdc7be1"),
            entry(2, "sccache/f/f/e/ffe416c77fa03035eff6e69c3dcd44914f24732d"),
            entry(3, "install-qt-action-mac-24.6.0-desktop-clang_64-6.10.3-/Users/runner/Qt"),
            entry(4, "install-qt-action-mac-25.6.0-desktop-clang_64-6.10.3-/Users/runner/Qt"),
        ]
        self.assertEqual(prune_caches.plan(entries, set()), [])

    def test_deletes_everything_of_a_closed_ref_once_largest_first(self):
        entries = [
            entry(1, "sccache/a", ref="refs/pull/9/merge", size=10),
            entry(2, "node-cache-Linux-x64-npm-" + HASH_A, ref="refs/pull/9/merge",
                  created="2026-11-18T01:00:00Z", size=50),
            entry(3, "node-cache-Linux-x64-npm-" + HASH_B, ref="refs/pull/9/merge",
                  created="2026-11-19T01:00:00Z", size=30),
            entry(4, "sccache/b", ref="refs/heads/develop"),
        ]
        doomed = prune_caches.plan(entries, {"refs/pull/9/merge"})
        self.assertEqual([e["id"] for e in doomed], [2, 3, 1])

    def test_family_needs_a_trailing_content_hash(self):
        self.assertEqual(prune_caches.family("fetchcontent-lite-Windows-X64-msvc-qt-6.10.*-"
                                             + HASH_A),
                         "fetchcontent-lite-Windows-X64-msvc-qt-6.10.*")
        self.assertIsNone(prune_caches.family("fetchcontent-lite-Windows-X64-msvc-qt-6.10.*-"))
        self.assertIsNone(prune_caches.family("node-cache-Linux-x64-npm-not-a-hash"))


if __name__ == "__main__":
    unittest.main()
