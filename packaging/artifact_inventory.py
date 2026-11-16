#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Inventory an installed tree without executing its contents; emit review evidence."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import uuid
from urllib.parse import urlsplit, urlunsplit


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def encoded(value):
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode()


def object_digest(value):
    return hashlib.sha256(encoded(value)).hexdigest()


def npm_manifest(relative):
    parts = Path(relative).parts
    if not parts or parts[-1] != "package.json" or "node_modules" not in parts:
        return False
    index = max(index for index, value in enumerate(parts) if value == "node_modules")
    suffix = parts[index + 1:]
    return len(suffix) == (3 if suffix[0].startswith("@") else 2)


def public_npm_source(repository):
    value = repository.get("url", "") if isinstance(repository, dict) else repository
    if not isinstance(value, str):
        return "NOASSERTION"
    value = value.removeprefix("git+")
    parsed = urlsplit(value)
    if (parsed.scheme != "https" or parsed.username or parsed.password
            or parsed.hostname not in {"github.com", "gitlab.com", "codeberg.org", "bitbucket.org"}):
        return "NOASSERTION"
    return urlunsplit((parsed.scheme, parsed.netloc, parsed.path, "", ""))


def classify(path, relative):
    kinds = []
    with path.open("rb") as stream:
        magic = stream.read(4)
    if magic == b"\x7fELF" or magic[:2] == b"MZ" or magic in (
        b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
        b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
        b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
    ):
        kinds.append("native")
    if path.stat().st_mode & 0o111 or magic[:2] == b"#!":
        kinds.append("executable")
    if path.name.lower() in ("node", "node.exe"):
        kinds.append("node")
    if "qt" in path.name.lower() or ".framework" in relative.lower():
        kinds.append("qt-or-framework")
    if any(part.lower() in ("plugins", "qml") for part in Path(relative).parts):
        kinds.append("plugin-or-qml")
    if path.suffix.lower() in (".ttf", ".otf", ".woff", ".woff2"):
        kinds.append("font")
    if path.suffix.lower() in (".icns", ".ico", ".svg", ".png"):
        kinds.append("icon-or-image")
    if path.suffix.lower() in (".service", ".desktop") or relative.startswith("DEBIAN/"):
        kinds.append("package-metadata")
    return kinds or ["resource"]


def inventory(root, source_commit, platform, epoch):
    if source_commit != "NOASSERTION" and not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", source_commit):
        raise ValueError("Use a full source commit identity or explicit NOASSERTION")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", platform):
        raise ValueError("An explicit platform/architecture label is required")
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("Staging must be a directory")
    files, npm, directories = [], {}, []
    def walk_error(error):
        raise ValueError("Unreadable staging directory") from error
    # os.walk never follows directory links. Special files are rejected, not skipped.
    for directory, dirs, names in os.walk(root, followlinks=False, onerror=walk_error):
        for name in sorted(dirs + names):
            path = Path(directory) / name
            relative = path.relative_to(root).as_posix()
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode):
                target = os.readlink(path)
                try:
                    path.resolve(strict=True).relative_to(root)
                except (ValueError, OSError, RuntimeError) as error:
                    raise ValueError("Invalid staging symlink") from error
                if os.path.isabs(target):
                    raise ValueError("Staging symlinks must be relative")
                files.append({"path": relative, "type": "symlink", "target": target,
                              "sha256": hashlib.sha256(os.fsencode(target)).hexdigest(),
                              "mode": stat.S_IMODE(mode), "kinds": ["symlink"]})
            elif stat.S_ISDIR(mode):
                directories.append({"path": relative, "mode": stat.S_IMODE(mode)})
                continue
            elif stat.S_ISREG(mode):
                files.append({"path": relative, "type": "file", "size": path.stat().st_size,
                              "sha256": digest(path), "mode": stat.S_IMODE(mode),
                              "kinds": classify(path, relative)})
                if npm_manifest(relative):
                    data = json.loads(path.read_text(encoding="utf-8"))
                    if not isinstance(data, dict) or not isinstance(data.get("name"), str) or not data["name"]:
                        raise ValueError("Invalid installed npm package manifest")
                    npm[path.parent.relative_to(root).as_posix()] = {
                        "name": data["name"], "version": str(data.get("version", "NOASSERTION")),
                        "kind": "npm", "source": public_npm_source(data.get("repository", "")),
                        "declaredLicense": data.get("license") if isinstance(data.get("license"), str)
                        else "NOASSERTION"}
            else:
                raise ValueError("Unsupported special file in staging")
    if not files:
        raise ValueError("Staging has no files")
    components = {}
    for entry in sorted(files, key=lambda item: item["path"]):
        name = entry["path"]
        owners = [parent for parent in npm if name.startswith(parent + "/")]
        owner = max(owners, key=len) if owners else None
        if owner:
            key, metadata = "npm:" + owner, npm[owner]
        elif set(entry["kinds"]) - {"resource", "symlink"}:
            key, metadata = "file:" + name, {"name": name, "kind": ",".join(entry["kinds"])}
        else:
            key, metadata = "resources", {"name": "Installed resources", "kind": "resource"}
        component = components.setdefault(key, {**metadata, "files": [], "version": metadata.get("version", "NOASSERTION")})
        component["files"].append(name)
        entry["component"] = key
    for component in components.values():
        if component["kind"] == "npm":
            component["notices"] = [name for name in component["files"]
                if re.match(r"^(LICENSE|LICENCE|COPYING|NOTICE)(\b|[._-])", Path(name).name, re.I)]
            component["sourceMaterials"] = [name for name in component["files"]
                if Path(name).suffix.lower() in (".js", ".mjs", ".cjs", ".ts", ".c", ".h", ".cpp")]
    return {"schemaVersion": 1, "sourceCommit": source_commit, "platform": platform,
            "created": datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "files": sorted(files, key=lambda item: item["path"]), "components": components,
            "directories": sorted(directories, key=lambda item: item["path"]),
            "rootMode": stat.S_IMODE(root.stat().st_mode),
            "limitations": ["Embedded/static components require build evidence and manual inventory.",
                            "Native dependency closure, license conclusions and source offers require review.",
                            "Final archive extraction, signing and legal acceptance are separate checks."]}


def spdx(inv):
    packages, relationships, files = [], [], []
    for key, value in sorted(inv["components"].items()):
        identifier = "SPDXRef-Package-" + hashlib.sha256(key.encode()).hexdigest()
        packages.append({"SPDXID": identifier, "name": value["name"], "versionInfo": value["version"],
                         "downloadLocation": "NOASSERTION", "filesAnalyzed": False,
                         "licenseDeclared": "NOASSERTION", "licenseConcluded": "NOASSERTION",
                         "copyrightText": "NOASSERTION", "comment": "Candidate from staging; requires review."})
        relationships.append({"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                              "relatedSpdxElement": identifier})
        for name in value["files"]:
            relationships.append({"spdxElementId": identifier, "relationshipType": "CONTAINS",
                                  "relatedSpdxElement": "SPDXRef-File-" + hashlib.sha256(name.encode()).hexdigest()})
    for entry in inv["files"]:
        files.append({"SPDXID": "SPDXRef-File-" + hashlib.sha256(entry["path"].encode()).hexdigest(),
                      "fileName": "./" + entry["path"],
                      "checksums": [{"algorithm": "SHA256", "checksumValue": entry["sha256"]}],
                      "licenseConcluded": "NOASSERTION", "licenseInfoInFiles": ["NOASSERTION"],
                      "copyrightText": "NOASSERTION",
                      "comment": "Symlink checksum covers link text." if entry["type"] == "symlink" else "Staged file bytes."})
    return {"spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
            "name": "KetPlus staged inventory " + inv["platform"],
            "documentNamespace": "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, object_digest(inv))),
            "creationInfo": {"created": inv["created"], "creators": ["Tool: KetPlus artifact inventory"]},
            "documentComment": "Engineering evidence; no publication authorization. Source: " + inv["sourceCommit"],
            "packages": packages, "files": files, "relationships": relationships}


def write_evidence(root, out, source_commit, platform, epoch):
    # Evidence is external to staging to avoid self-referential checksums.
    out = out.resolve()
    if out == root.resolve() or root.resolve() in out.parents:
        raise ValueError("Evidence output must be outside staging")
    inv = inventory(root, source_commit, platform, epoch)
    out.mkdir(parents=True, exist_ok=True)
    outputs = {"STAGED-INVENTORY.json": inv, "STAGED-SBOM.spdx.json": spdx(inv)}
    outputs["COMPONENT-REVIEW.template.json"] = {
        "schemaVersion": 1, "inventorySha256": object_digest(inv),
        "components": [{"id": key, "name": value["name"], "version": value["version"],
                        "files": value["files"], "source": value.get("source", "NOASSERTION"),
                        "license": value.get("declaredLicense", "NOASSERTION"),
                        "notices": value.get("notices", []),
                        "sourceMaterials": value.get("sourceMaterials", []), "embedded": False}
                       for key, value in sorted(inv["components"].items())],
        "embeddedInventoryComplete": False, "dependencyAuditComplete": False,
        "buildEvidence": [], "dependencyEvidence": []}
    for name in outputs:
        if os.path.lexists(out / name):
            raise ValueError("Evidence output already exists")
    for name, value in outputs.items():
        (out / name).write_bytes(encoded(value))
    return inv


def bind_artifacts(out, artifacts, metadata, base_dir=None):
    base_dir = (base_dir or out).resolve()
    paths = [(path, "artifact") for path in artifacts] + [
        (metadata, "build-metadata"), (out / "STAGED-INVENTORY.json", "inventory"),
        (out / "STAGED-SBOM.spdx.json", "sbom")]
    names = [path.resolve().relative_to(base_dir).as_posix() for path, _ in paths]
    if not artifacts or len(set(names)) != len(paths):
        raise ValueError("Artifact evidence names must be unique")
    return {"schemaVersion": 1, "publicationAuthorized": False,
            "files": [{"name": name, "role": role, "sha256": digest(path)}
                      for name, (path, role) in zip(names, paths)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--epoch", type=int, required=True)
    args = parser.parse_args()
    write_evidence(args.stage, args.out, args.source_commit, args.platform, args.epoch)
    print("Staged inventory and unapproved component review template created.")


if __name__ == "__main__":
    main()
