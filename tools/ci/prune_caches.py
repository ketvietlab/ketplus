#!/usr/bin/env python3
"""Delete GitHub Actions cache entries that no later run will restore.

The repository's Actions cache holds 10 GB; past that GitHub evicts the least recently used
entries, which were the compiler objects every build needs. Two kinds of entry are orphans:

- every entry of a closed pull request: its merge ref never runs again;
- a superseded entry of a family whose key ends in a content hash (FetchContent sources, the
  npm cache): once a newer entry of that family exists on the same ref, nothing restores the
  older one, because an exact key or the newest prefix match always wins.

Per-object sccache entries and Qt installs are left alone: their keys do not name a family
whose older members are unreachable, and LRU eviction already handles them.

  prune_caches.py --repo owner/name [--closed-ref refs/pull/N/merge] [--dry-run]

Without --closed-ref it also deletes the entries of every pull request ref whose pull request
is closed. Needs `gh` with a token that has actions: write.
"""
import argparse
import json
import re
import subprocess
import sys

# A key family: the key without its trailing content hash.
FAMILIES = (
    re.compile(r"^(fetchcontent-.+)-[0-9a-f]{40,64}$"),
    re.compile(r"^(node-cache-.+)-[0-9a-f]{40,64}$"),
)
PULL_REF = re.compile(r"^refs/pull/(\d+)/merge$")


def family(key):
    for pattern in FAMILIES:
        match = pattern.match(key)
        if match:
            return match.group(1)
    return None


def superseded(entries):
    """Entries with a newer entry of the same family on the same ref."""
    newest = {}
    for entry in entries:
        name = family(entry["key"])
        if name is None:
            continue
        group = (entry["ref"], name)
        if group not in newest or entry["created_at"] > newest[group]["created_at"]:
            newest[group] = entry
    keep = {entry["id"] for entry in newest.values()}
    return [e for e in entries if family(e["key"]) is not None and e["id"] not in keep]


def on_refs(entries, refs):
    return [entry for entry in entries if entry["ref"] in refs]


def plan(entries, closed_refs):
    """The entries to delete, each once, largest first."""
    chosen = {entry["id"]: entry for entry in on_refs(entries, closed_refs)}
    for entry in superseded(entries):
        chosen.setdefault(entry["id"], entry)
    return sorted(chosen.values(), key=lambda entry: -entry["size_in_bytes"])


def gh(*args):
    return subprocess.run(["gh", *args], check=True, stdout=subprocess.PIPE).stdout.decode()


def list_caches(repo):
    output = gh("api", "--paginate", "repos/%s/actions/caches?per_page=100" % repo,
                "--jq", ".actions_caches[]")
    return [json.loads(line) for line in output.splitlines() if line.strip()]


def closed_pull_refs(repo, entries):
    refs = set()
    for ref in sorted({entry["ref"] for entry in entries}):
        match = PULL_REF.match(ref)
        if match and gh("api", "repos/%s/pulls/%s" % (repo, match.group(1)),
                        "--jq", ".state").strip() == "closed":
            refs.add(ref)
    return refs


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", required=True)
    parser.add_argument("--closed-ref", action="append", default=[])
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    entries = list_caches(args.repo)
    closed = set(args.closed_ref) if args.closed_ref else closed_pull_refs(args.repo, entries)
    doomed = plan(entries, closed)
    failed = 0
    for entry in doomed:
        print("%s %7.1f MB  %s  %s" % ("would delete" if args.dry_run else "delete",
                                        entry["size_in_bytes"] / 1e6, entry["ref"], entry["key"]))
        if args.dry_run:
            continue
        try:
            gh("api", "-X", "DELETE", "repos/%s/actions/caches/%d" % (args.repo, entry["id"]))
        except subprocess.CalledProcessError:
            failed += 1
    total = sum(entry["size_in_bytes"] for entry in doomed) / 1e9
    print("%d of %d entries, %.2f GB%s" % (len(doomed), len(entries), total,
                                         "; %d deletions failed" % failed if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
