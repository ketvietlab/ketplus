#!/usr/bin/env python3
"""Split a CTest suite into balanced shards, and refresh the test costs it balances by.

plan   Prints the CTest name regex of one shard (1-based) of the tests in --test-dir, or nothing
       when the shard has no tests. Every job of a shard matrix runs this on the same build
       tree, so they agree on the split: the longest tests go first, each into the shard with
       the least total cost. The cost of a test is its COST property, which
       cmake/TestLabels.cmake sets from tests/test-costs.txt; others count DEFAULT_COST.
costs  Prints a new tests/test-costs.txt from CTestCostData.txt files (Testing/Temporary in a
       build tree, or the shard artifacts of a CI run), keeping each test's largest cost.
"""
import argparse
import heapq
import json
import re
import subprocess
import sys

DEFAULT_COST = 3.0
NAME = re.compile(r"^[A-Za-z0-9_.-]+$")


def partition(tests, shards):
    """Splits (name, cost) pairs into `shards` lists of names, longest first, least loaded next."""
    if shards < 1:
        raise ValueError("shards must be at least 1")
    names = [name for name, _ in tests]
    if len(set(names)) != len(names):
        raise ValueError("duplicate test names")
    loads = [(0.0, index) for index in range(shards)]
    result = [[] for _ in range(shards)]
    for name, cost in sorted(tests, key=lambda test: (-test[1], test[0])):
        load, index = heapq.heappop(loads)
        result[index].append(name)
        heapq.heappush(loads, (load + cost, index))
    assigned = sorted(name for shard in result for name in shard)
    if assigned != sorted(names):
        raise AssertionError("the shards do not cover every test exactly once")
    return result


def name_regex(names):
    """A CTest -R regex matching exactly these names."""
    for name in names:
        if not NAME.match(name):
            raise ValueError("test name needs escaping: %r" % name)
    return "^(" + "|".join(name.replace(".", "\\.") for name in names) + ")$"


def suite_tests(test_dir, ctest):
    output = subprocess.run([ctest, "--test-dir", test_dir, "--show-only=json-v1"],
                            check=True, stdout=subprocess.PIPE).stdout
    tests = []
    for test in json.loads(output.decode("utf-8"))["tests"]:
        cost = DEFAULT_COST
        for prop in test.get("properties", []):
            if prop["name"] == "COST":
                cost = float(prop["value"])
        tests.append((test["name"], cost))
    return tests


def read_cost_data(paths):
    costs = {}
    for path in paths:
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                fields = line.split()
                if fields == ["---"]:
                    break
                if len(fields) == 3:
                    costs[fields[0]] = max(costs.get(fields[0], 0.0), float(fields[2]))
    return costs


def cost_file(costs, min_seconds):
    lines = [
        "# Measured CTest durations in seconds: the COST of these tests and how CI shards are",
        "# balanced (cmake/TestLabels.cmake, tools/ci/shard_tests.py). Tests not listed count",
        "# %g seconds. Refresh: python3 tools/ci/shard_tests.py costs <CTestCostData.txt>..."
        % DEFAULT_COST,
    ]
    rows = sorted((-round(cost), name) for name, cost in costs.items() if cost >= min_seconds)
    lines += ["%s %d" % (name, -cost) for cost, name in rows]
    return "\n".join(lines) + "\n"


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    plan = commands.add_parser("plan")
    plan.add_argument("--test-dir", required=True)
    plan.add_argument("--shards", type=int, required=True)
    plan.add_argument("--index", type=int, required=True, help="1-based shard")
    plan.add_argument("--ctest", default="ctest")
    costs = commands.add_parser("costs")
    costs.add_argument("--min-seconds", type=float, default=10.0)
    costs.add_argument("cost_data", nargs="+")
    args = parser.parse_args(argv)

    if args.command == "costs":
        sys.stdout.write(cost_file(read_cost_data(args.cost_data), args.min_seconds))
        return 0
    if not 1 <= args.index <= args.shards:
        parser.error("--index must be between 1 and --shards")
    tests = suite_tests(args.test_dir, args.ctest)
    shards = partition(tests, args.shards)
    cost = dict(tests)
    for number, names in enumerate(shards, 1):
        marker = "*" if number == args.index else " "
        sys.stderr.write("%s shard %d/%d: %d tests, cost %.0f\n"
                         % (marker, number, args.shards, len(names), sum(cost[n] for n in names)))
    selected = shards[args.index - 1]
    for name in selected:
        sys.stderr.write("    %s\n" % name)
    if selected:
        sys.stdout.write(name_regex(selected) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
