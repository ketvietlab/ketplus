#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Collect final package payloads through offline extraction, never execution."""
import json
from artifact_inventory import digest, encoded


def reconcile_recipe_sources(payload, evidence, kind, source_commit, version, pin):
    """Attach exact recipe/runtime provenance without concluding transitive licenses."""
    path = evidence / "COMPONENT-REVIEW.template.json"
    review = json.loads(path.read_text())
    prefix = "opt/ketplus-pro" if kind == "deb" else "rootfs/usr"
    license_path = prefix + "/share/licenses/ketplus/LICENSE"
    wrappers = ({"usr/bin/KetPlus", "usr/lib/systemd/user/ketplus-pro-daemon.service", "DEBIAN/control"}
                if kind == "deb" else {"rootfs/AppRun", "rootfs/ketplus-pro.desktop"})
    source = "https://github.com/ketvietlab/ketplus/blob/" + source_commit + "/packaging/linux/build-packages.py"
    for component in review["components"]:
        if component["id"] in {"file:" + name for name in wrappers}:
            if not (payload / license_path).is_file():
                raise ValueError("Final package lacks its wrapper license")
            component.update(version=version, source=source, license="MPL-2.0", notices=[license_path])
        elif kind == "appimage" and component["id"] == "file:runtime-prefix":
            notice = prefix + "/share/licenses/ketplus/AppImage-runtime.txt"
            if not (payload / notice).is_file() or digest(payload / notice) != pin["licenseSha256"]:
                raise ValueError("Final AppImage runtime notice is absent or differs from its source pin")
            component.update(version=pin["sourceCommit"], source=pin["sourceUrl"], notices=[notice])
            # The runtime notice names static dependencies; it is not a license
            # conclusion/source offer for all of them. Keep those gates unresolved.
    path.write_bytes(encoded(review))
def extract_payload(artifact, destination, kind, run, runtime=None):
    if destination.exists():
        raise ValueError("Use a new payload extraction directory")
    if kind == "deb":
        # Includes control metadata and all data: /usr launchers, service, icons,
        # /opt product prefix, and any future maintainer scripts.
        run("dpkg-deb", "--raw-extract", artifact, destination)
        return {"format": "deb-raw-extract"}
    if kind != "appimage" or runtime is None:
        raise ValueError("Unknown final payload format")
    destination.mkdir(parents=True)
    size = runtime.stat().st_size
    prefix = destination / "runtime-prefix"
    with artifact.open("rb") as source, prefix.open("xb") as output:
        remaining = size
        while remaining:
            block = source.read(min(remaining, 1024 * 1024))
            if not block:
                raise ValueError("Truncated AppImage runtime prefix")
            output.write(block)
            remaining -= len(block)
    if digest(prefix) != digest(runtime):
        raise ValueError("AppImage prefix differs from the verified runtime")
    # Unlike invoking AppImage --appimage-extract, unsquashfs does not run bytes
    # from the package. AppRun/desktop entries/icons are part of this rootfs.
    run("unsquashfs", "-o", str(size), "-no-progress", "-d", destination / "rootfs", artifact)
    return {"format": "appimage-prefix-and-squashfs", "runtimePrefixBytes": size}
