#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Fail-closed staged inventory/license review check, never release authorization."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import sys

from artifact_inventory import digest, encoded, inventory, object_digest, spdx


def reviewed_spdx(inv, review):
    result = spdx(inv)
    result["packages"], result["relationships"] = [], []
    result["documentNamespace"] += "-review-" + object_digest(review)
    result["documentComment"] += " Review: " + object_digest(review)
    for component in review["components"]:
        identifier = "SPDXRef-Package-" + hashlib.sha256(component["id"].encode()).hexdigest()
        result["packages"].append({
            "SPDXID": identifier, "name": component["name"], "versionInfo": component["version"],
            "downloadLocation": "NOASSERTION", "sourceInfo": component["source"],
            "licenseDeclared": component["license"], "licenseConcluded": component["license"],
            "filesAnalyzed": False, "copyrightText": "NOASSERTION",
            "comment": "Supplied review; notices: " + json.dumps(component["notices"])
                       + "; source materials: " + json.dumps(component["sourceMaterials"])})
        result["relationships"].append({"spdxElementId": "SPDXRef-DOCUMENT",
                                        "relationshipType": "DESCRIBES", "relatedSpdxElement": identifier})
        for name in component["files"]:
            result["relationships"].append({"spdxElementId": identifier, "relationshipType": "CONTAINS",
                                            "relatedSpdxElement": "SPDXRef-File-" + hashlib.sha256(name.encode()).hexdigest()})
    return result


def check(stage, inv, review, evidence, artifacts_dir):
    errors = set()
    epoch = int(datetime.datetime.strptime(inv["created"], "%Y-%m-%dT%H:%M:%SZ").replace(
        tzinfo=datetime.timezone.utc).timestamp())
    actual = inventory(stage, inv["sourceCommit"], inv["platform"], epoch)
    if inv["sourceCommit"] == "NOASSERTION":
        errors.add("source-identity-unresolved")
    if actual != inv:
        errors.add("staging-changed")
    if review.get("schemaVersion") != 1 or review.get("inventorySha256") != object_digest(inv):
        errors.add("review-inventory-mismatch")
    files = {entry["path"]: entry for entry in actual["files"]}

    def materials(value):
        if not isinstance(value, list) or not value:
            return False
        for name in value:
            entry = files.get(name) if isinstance(name, str) else None
            if not entry or entry["type"] != "file" or not entry["size"]:
                return False
        return len(set(value)) == len(value)

    def concluded(value):
        return (isinstance(value, str) and value.strip() == value and bool(value)
                and not re.search(r"NOASSERTION|\bNONE\b|\bTODO\b|\bUNKNOWN\b", value, re.I))

    components = review.get("components")
    if not isinstance(components, list) or not components:
        components = []
        errors.add("components-missing")
    covered, ids, discovered = set(), set(), set()
    for component in components:
        identifier = component.get("id")
        if not isinstance(identifier, str) or not identifier or identifier in ids:
            errors.add("component-id-invalid")
        else:
            ids.add(identifier)
        for field in ("name", "version", "license", "source"):
            if not concluded(component.get(field)):
                errors.add("component-identity-or-license-unresolved")
        if not materials(component.get("notices")):
            errors.add("component-notices-missing")
        if not materials(component.get("sourceMaterials")):
            errors.add("component-source-materials-missing")
        names = component.get("files")
        if not isinstance(names, list) or not all(isinstance(name, str) for name in names):
            errors.add("component-files-invalid")
            continue
        if not names and component.get("embedded") is not True:
            errors.add("component-files-missing")
        for name in names:
            if name not in files:
                errors.add("component-file-absent")
            else:
                discovered.add(files[name]["component"])
            covered.add(name)
        # Each discovered npm/native/font/icon identity must survive review as a
        # distinct component; extra static/embedded entries can overlap its files.
        expected = actual["components"].get(identifier)
        if expected and set(names) != set(expected["files"]):
            errors.add("discovered-component-files-changed")
        if expected and (component.get("name") != expected["name"] or (
                expected["version"] != "NOASSERTION" and component.get("version") != expected["version"])):
            errors.add("discovered-component-identity-changed")
    if covered != set(files):
        errors.add("staged-files-uncovered")
    if not set(actual["components"]).issubset(ids) or discovered != set(actual["components"]):
        errors.add("discovered-components-uncovered")
    for flag, proof in (("embeddedInventoryComplete", "buildEvidence"),
                        ("dependencyAuditComplete", "dependencyEvidence")):
        if review.get(flag) is not True or not materials(review.get(proof)):
            errors.add("embedded-or-dependency-evidence-missing")

    bindings = evidence.get("files", [])
    names = [entry["name"] for entry in bindings]
    required = {"STAGED-INVENTORY.json": object_digest(inv),
                "STAGED-SBOM.spdx.json": object_digest(spdx(inv))}
    if evidence.get("schemaVersion") != 1 or evidence.get("publicationAuthorized") is not False:
        errors.add("artifact-evidence-invalid")
    if len(set(names)) != len(names) or len(names) < 4:
        errors.add("artifact-bindings-incomplete")
    roles = [entry.get("role") for entry in bindings]
    if (roles.count("inventory") != 1 or roles.count("sbom") != 1
            or roles.count("build-metadata") != 1 or roles.count("artifact") < 1
            or set(roles) - {"inventory", "sbom", "build-metadata", "artifact"}):
        errors.add("artifact-roles-incomplete")
    for entry in bindings:
        name = entry["name"]
        if not isinstance(name, str) or not name or Path(name).name != name or name in (".", ".."):
            errors.add("artifact-name-invalid")
            continue
        path = artifacts_dir / name
        if path.is_symlink() or not path.is_file() or digest(path) != entry.get("sha256"):
            errors.add("artifact-digest-mismatch")
        if name in required and required[name] != entry.get("sha256"):
            errors.add("artifact-inventory-or-sbom-mismatch")
        if entry.get("role") == "build-metadata" and path.is_file() and not path.is_symlink():
            metadata = json.loads(path.read_text())
            if metadata.get("sourceCommit") != inv["sourceCommit"]:
                errors.add("artifact-source-identity-mismatch")
    if not set(required).issubset(names):
        errors.add("artifact-inventory-or-sbom-missing")
    if review.get("artifactEvidenceSha256") != object_digest(evidence):
        errors.add("review-artifact-evidence-mismatch")
    return {"schemaVersion": 1, "inventoryLicenseGatePassed": not errors,
            "publicationAuthorized": False, "errors": sorted(errors),
            "sourceCommit": inv["sourceCommit"], "platform": inv["platform"],
            "inventorySha256": object_digest(inv), "reviewSha256": object_digest(review),
            "artifactEvidenceSha256": object_digest(evidence),
            "reviewedSbomSha256": object_digest(reviewed_spdx(inv, review)) if not errors else None,
            "remainingReleaseChecks": ["Final archive payload extraction and comparison",
                                       "Source/notice legal sufficiency and availability",
                                       "Platform build/smoke, signing and release approval"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--review", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--artifacts-dir", type=Path, required=True)
    parser.add_argument("--sbom-out", type=Path, help="Write a new reviewed SPDX file only on success")
    args = parser.parse_args()
    try:
        inv = json.loads(args.inventory.read_text())
        review = json.loads(args.review.read_text())
        result = check(args.stage, inv, review, json.loads(args.evidence.read_text()),
                       args.artifacts_dir)
        if result["inventoryLicenseGatePassed"] and args.sbom_out:
            output = args.sbom_out.absolute()
            if args.stage.resolve() == output.resolve() or args.stage.resolve() in output.resolve().parents:
                raise ValueError("Reviewed evidence must remain outside staging")
            with output.open("xb") as stream:
                stream.write(encoded(reviewed_spdx(inv, review)))
    except (ValueError, OSError, TypeError, KeyError, AttributeError):
        # Do not echo malformed input, staging content or machine paths.
        result = {"inventoryLicenseGatePassed": False, "publicationAuthorized": False,
                  "errors": ["invalid-or-unreadable-input"]}
    sys.stdout.buffer.write(encoded(result))
    return 0 if result["inventoryLicenseGatePassed"] else 1


if __name__ == "__main__":
    sys.exit(main())
