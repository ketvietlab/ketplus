#!/usr/bin/env python3
"""Summarize the benchmark's phase-tagged strace output."""

import argparse
from collections import Counter
import csv
from pathlib import Path
import re
import sys


PHASES = (
    "sql",
    "canonical-stat",
    "worktree-repository",
    "repository-checkouts",
    "source-context",
    "contains",
)
CALL = re.compile(r"^(\d+)\s+(\w+)\(")
MARKER = re.compile(r'write\(2, "ketplus-phase:(\d+|end)\\n"')


def count(path):
    active = {}
    counts = [Counter() for _ in PHASES]
    for line in path.open(encoding="utf-8"):
        match = CALL.match(line)
        if not match:
            continue
        pid, syscall = match.groups()
        marker = MARKER.search(line) if syscall == "write" else None
        if marker:
            value = marker.group(1)
            if value == "end":
                active.pop(pid)
            else:
                active[pid] = int(value)
        elif pid in active and syscall != "write":
            counts[active[pid]][syscall] += 1
    if active or not any(counts):
        raise ValueError("Expected a complete phase-tagged strace")
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    counts = count(args.trace)
    names = sorted({name for phase in counts for name in phase})
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(["phase", "total", *names])
    for phase, calls in zip(PHASES, counts):
        writer.writerow([phase, sum(calls.values()), *(calls[name] for name in names)])


if __name__ == "__main__":
    main()
