"""Checks the CI test sharding in tools/ci/shard_tests.py. Needs no build tree."""
from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ci"))
import shard_tests  # noqa: E402


class PartitionTest(unittest.TestCase):
    def test_every_test_runs_in_exactly_one_shard(self):
        tests = [("t%03d" % i, float(i % 7 + 1)) for i in range(250)]
        for shards in (1, 2, 5, 16, 300):
            split = shard_tests.partition(tests, shards)
            self.assertEqual(len(split), shards)
            flat = [name for shard in split for name in shard]
            self.assertEqual(sorted(flat), sorted(name for name, _ in tests))

    def test_long_tests_spread_before_short_ones(self):
        tests = [("long_a", 100.0), ("long_b", 90.0), ("long_c", 80.0)]
        tests += [("short_%02d" % i, 1.0) for i in range(30)]
        split = shard_tests.partition(tests, 3)
        self.assertEqual([shard[0] for shard in split], ["long_a", "long_b", "long_c"])
        loads = [sum(dict(tests)[name] for name in shard) for shard in split]
        self.assertLessEqual(max(loads) - min(loads), 20.0)

    def test_split_does_not_depend_on_input_order(self):
        tests = [("t%02d" % i, float(i % 4)) for i in range(40)]
        self.assertEqual(shard_tests.partition(tests, 6),
                         shard_tests.partition(list(reversed(tests)), 6))

    def test_rejects_bad_input(self):
        with self.assertRaises(ValueError):
            shard_tests.partition([("a", 1.0)], 0)
        with self.assertRaises(ValueError):
            shard_tests.partition([("a", 1.0), ("a", 2.0)], 2)


class RegexTest(unittest.TestCase):
    def test_matches_exactly_the_listed_names(self):
        pattern = re.compile(shard_tests.name_regex(["a_tests", "b.v2-tests"]))
        self.assertTrue(pattern.match("a_tests"))
        self.assertTrue(pattern.match("b.v2-tests"))
        self.assertFalse(pattern.match("a_tests_more"))
        self.assertFalse(pattern.match("bxv2-tests"))
        self.assertFalse(pattern.match("xa_tests"))

    def test_refuses_names_that_need_escaping(self):
        with self.assertRaises(ValueError):
            shard_tests.name_regex(["a|b"])


class CostFileTest(unittest.TestCase):
    def test_keeps_the_largest_cost_and_drops_short_and_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory, "one.txt")
            second = Path(directory, "two.txt")
            first.write_text("slow_tests 3 42.4\nquick_tests 9 0.5\n---\nslow_tests\n")
            second.write_text("slow_tests 1 61.6\nother_tests 2 12\n")
            costs = shard_tests.read_cost_data([str(first), str(second)])
        self.assertEqual(costs, {"slow_tests": 61.6, "quick_tests": 0.5, "other_tests": 12.0})
        text = shard_tests.cost_file(costs, 10.0)
        rows = [line for line in text.splitlines() if not line.startswith("#")]
        self.assertEqual(rows, ["slow_tests 62", "other_tests 12"])

    def test_checked_in_costs_match_the_cmake_pattern(self):
        pattern = re.compile(r"^[A-Za-z0-9_.-]+ +[0-9]+$")
        lines = (ROOT / "tests" / "test-costs.txt").read_text().splitlines()
        rows = [line for line in lines if line and not line.startswith("#")]
        self.assertTrue(rows)
        self.assertEqual([line for line in rows if not pattern.match(line)], [])


if __name__ == "__main__":
    unittest.main()
