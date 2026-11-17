#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Compare inventoried npm files with lock-pinned archives, offline and without extraction."""
import argparse
import base64
import datetime
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys
import tarfile
from urllib.parse import urlsplit

from artifact_inventory import digest, encoded, inventory, object_digest


def archive_name(url):
    return hashlib.sha256(url.encode()).hexdigest() + ".tgz"


def compare_component(identifier, component, files, lock_root, lock, archives):
    root = identifier.removeprefix("npm:")
    relative = root.removeprefix(lock_root + "/")
    package = lock["packages"].get(relative, {})
    if package.get("version") != component["version"] or package.get("license") != component["declaredLicense"]:
        raise ValueError("lock-manifest-mismatch")
    url = package.get("resolved", "")
    parsed = urlsplit(url)
    if parsed.scheme != "https" or parsed.netloc != "registry.npmjs.org" or parsed.query or parsed.fragment:
        raise ValueError("non-public-registry-url")
    integrity = package.get("integrity", "")
    if not re.fullmatch(r"sha512-[A-Za-z0-9+/]{86}==", integrity):
        raise ValueError("unsupported-lock-integrity")
    result = {"id": identifier, "name": component["name"], "version": component["version"],
              "declaredLicense": component["declaredLicense"], "url": url, "integrity": integrity,
              "stagedFileCount": len(component["files"]), "installedNotices": component["notices"],
              "installedSourceFileCount": len(component["sourceMaterials"])}
    archive = archives / archive_name(url)
    if not archive.is_file():
        return {**result, "issue": "archive-not-supplied"}
    if archive.stat().st_size > 256 * 1024 * 1024:
        raise ValueError("archive-size-limit")
    checksum = hashlib.sha512()
    with archive.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    if integrity != "sha512-" + base64.b64encode(checksum.digest()).decode():
        raise ValueError("archive-integrity-mismatch")
    expected = {"package/" + name[len(root) + 1:]: files[name] for name in component["files"]}
    matched, seen, notices, mismatches, total = set(), set(), [], [], 0
    with tarfile.open(archive, "r:gz") as contents:
        for member in contents:
            name = member.name.removeprefix("./")
            path = PurePosixPath(name)
            total += member.size
            if (path.is_absolute() or ".." in path.parts or name in seen or len(seen) >= 100000
                    or total > 512 * 1024 * 1024):
                raise ValueError("archive-member-invalid")
            seen.add(name)
            if member.isfile() and re.match(r"^(LICENSE|LICENCE|NOTICE|COPYING)(\b|[._-])", path.name, re.I):
                notices.append(name.removeprefix("package/"))
            if name not in expected:
                continue
            entry = expected[name]
            if not member.isfile() or entry["type"] != "file" or member.size != entry["size"]:
                mismatches.append(entry["path"])
                continue
            checksum = hashlib.sha256()
            with contents.extractfile(member) as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    checksum.update(block)
            if checksum.hexdigest() != entry["sha256"]:
                mismatches.append(entry["path"])
            else:
                matched.add(name)
    mismatches += [entry["path"] for name, entry in expected.items() if name not in seen]
    return {**result, "archiveSha256": digest(archive), "integrityVerified": True,
            "matchedFileCount": len(matched), "upstreamNoticeFiles": sorted(notices),
            "mismatches": sorted(mismatches), **({"issue": "staged-files-differ"} if mismatches else {})}


def verify(inv, locks, archives):
    files = {entry["path"]: entry for entry in inv["files"]}
    results = []
    for identifier, component in sorted(inv["components"].items()):
        if component["kind"] != "npm":
            continue
        owners = [root for root in locks if identifier[4:].startswith(root + "/")]
        if not owners:
            results.append({"id": identifier, "issue": "lock-not-supplied"})
            continue
        root = max(owners, key=len)
        try:
            results.append(compare_component(identifier, component, files, root, locks[root], archives))
        except (ValueError, KeyError, OSError, EOFError, tarfile.TarError) as error:
            # Never echo paths, archive contents, URL credentials or parser exceptions.
            rule = str(error) if isinstance(error, ValueError) and re.fullmatch(r"[a-z-]+", str(error)) else "invalid-input"
            results.append({"id": identifier, "issue": rule})
    return {"schemaVersion": 1, "inventorySha256": object_digest(inv), "sourceCommit": inv["sourceCommit"],
            "lockSha256": {root: object_digest(lock) for root, lock in sorted(locks.items())},
            "components": results, "allNpmFilesMatch": bool(results) and all("issue" not in r for r in results),
            "publicationAuthorized": False,
            "limitations": ["Registry integrity and file equality do not conclude licenses or redistribution rights.",
                            "Notices, corresponding source, embedded components and dependency audit still require review."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--archives", type=Path, required=True)
    parser.add_argument("--lock", action="append", required=True, metavar="STAGED_RUNTIME_ROOT=LOCK_FILE")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.stage.resolve() == args.out.resolve() or args.stage.resolve() in args.out.resolve().parents:
        parser.error("output-must-be-outside-staging")
    inv = json.loads(args.inventory.read_text())
    epoch = int(datetime.datetime.strptime(inv["created"], "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc).timestamp())
    if inventory(args.stage, inv["sourceCommit"], inv["platform"], epoch) != inv:
        parser.error("staging-changed")
    locks = {}
    for item in args.lock:
        root, filename = item.split("=", 1)
        if root in locks or PurePosixPath(root).is_absolute() or ".." in PurePosixPath(root).parts or "\\" in root:
            parser.error("invalid-or-duplicate-lock-root")
        locks[root] = json.loads(Path(filename).read_text())
    report = verify(inv, locks, args.archives)
    with args.out.open("xb") as output:
        output.write(encoded(report))
    return 0 if report["allNpmFilesMatch"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, TypeError, KeyError, AttributeError, EOFError, tarfile.TarError):
        sys.stderr.write("npm-evidence-invalid-or-unreadable-input\n")
        raise SystemExit(2)
