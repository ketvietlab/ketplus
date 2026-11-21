#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Assemble unsigned engineering packages from cmake --install; never publish or sign."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import urllib.request
import uuid

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def architecture(value):
    mapping = {"x86_64": ("x86_64", "amd64"), "amd64": ("x86_64", "amd64"),
               "aarch64": ("aarch64", "arm64"), "arm64": ("aarch64", "arm64")}
    if value not in mapping:
        raise ValueError(f"Unsupported architecture: {value}")
    return mapping[value]


def read_cache(build):
    values = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line and ":" in line:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    for key, required in {"KETPLUS_EDITION": "pro", "CMAKE_BUILD_TYPE": "Release",
                          "KETPLUS_PRODUCTION_RELEASE": "ON",
                          "KETPLUS_LINUX_DESKTOP_PACKAGE": "ON"}.items():
        if values.get(key) != required:
            raise ValueError(f"{key} must be {required} for Linux desktop packages")
    return values


def verified_runtime(cache, arch, pin):
    spec = pin["assets"][arch]
    path = cache / ("runtime-" + arch)
    if path.is_symlink():
        raise ValueError("The runtime cache entry must not be a symlink")
    if not path.exists():
        request = urllib.request.Request(spec["url"], headers={
            "Accept": "application/octet-stream", "User-Agent": "KetPlus-Linux-packaging"})
        with urllib.request.urlopen(request, timeout=120) as response:
            data = response.read(8 * 1024 * 1024 + 1)
        if len(data) > 8 * 1024 * 1024 or not data.startswith(b"\x7fELF"):
            raise ValueError("AppImage runtime is not a bounded ELF executable")
        if hashlib.sha256(data).hexdigest() != spec["sha256"]:
            raise ValueError("AppImage runtime download failed SHA-256 verification")
        path.write_bytes(data)
    if digest(path) != spec["sha256"]:
        raise ValueError("Cached AppImage runtime failed SHA-256 verification")
    return path


def validate_prefix(prefix):
    required = ["bin/KetPlus", "bin/ketplus_daemon", "bin/ketplusctl", "ketplus-pro",
                "bin/qt.conf", "libexec/qt.conf", "lib/libQt6Core.so.6", "lib/libQt6Widgets.so.6",
                "lib/libQt6PrintSupport.so.6", "lib/libQt6SvgWidgets.so.6",
                "lib/libQt6Core5Compat.so.6", "libexec/QtWebEngineProcess",
                "resources/qtwebengine_resources.pak", "qml/QtQuick/qmldir",
                "plugins/platforms/libqoffscreen.so", "plugins/platforms/libqxcb.so",
                "plugins/sqldrivers/libqsqlite.so",
                "share/applications/ketplus-pro.desktop",
                "share/icons/hicolor/512x512/apps/ketplus-pro.png",
                "lib/ketplus/ketplus-browser/runtime.json",
                "lib/ketplus/claude-sidecar/runtime.json"]
    missing = [name for name in required if not (prefix / name).is_file()]
    if missing:
        raise ValueError("Incomplete installed runtime: " + ", ".join(missing))
    for name in ["bin/KetPlus", "bin/ketplus_daemon", "bin/ketplusctl", "ketplus-pro",
                 "libexec/QtWebEngineProcess", "lib/ketplus/ketplus-browser/bin/node"]:
        if not os.access(prefix / name, os.X_OK):
            raise ValueError(f"Installed runtime is not executable: {name}")
    claude = list((prefix / "lib/ketplus/claude-sidecar/node_modules/@anthropic-ai").glob(
        "claude-agent-sdk-linux-*/claude"))
    if len(claude) != 1 or not os.access(claude[0], os.X_OK):
        raise ValueError("The native Claude sidecar executable is missing or not executable")
    if not list((prefix / "plugins/webview").glob("*.so")):
        raise ValueError("The WebView backend plugin was not deployed")
    if any(path.name != "libqsqlite.so" for path in (prefix / "plugins/sqldrivers").glob("*.so")):
        raise ValueError("Only the product's SQLite driver may be bundled")
    for path in prefix.rglob("*"):
        if path.is_symlink():
            try:
                path.resolve(strict=True).relative_to(prefix.resolve())
            except (ValueError, FileNotFoundError) as error:
                raise ValueError(f"Symlink escapes the installed runtime: {path.relative_to(prefix)}") from error


def build_info(prefix, expected_url):
    result = run(prefix / "ketplus-pro", "--build-info", capture_output=True, text=True)
    info = json.loads(result.stdout)
    if info.get("control_url") != expected_url or info.get("control_url_overridable") is not False:
        raise ValueError("Installed daemon has the wrong production control policy")
    if not re.fullmatch(r"[0-9a-f]{12}", info.get("build_revision", "")):
        raise ValueError("Installed daemon must identify a clean public source commit")
    return info


def write_sbom(prefix, out, info, version, qt_version, arch, pin, epoch):
    created = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    identity = json.dumps([info, version, qt_version, arch, pin], sort_keys=True)
    sbom = {
        "spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"KetPlus-Pro-{version}-{arch}",
        "documentNamespace": "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, identity)),
        "creationInfo": {"created": created, "creators": ["Tool: KetPlus Linux packaging"]},
        "packages": [
            {"SPDXID": "SPDXRef-KetPlus", "name": "KetPlus Pro", "versionInfo": version,
             "downloadLocation": "https://github.com/ketvietlab/ketplus", "filesAnalyzed": False,
             "licenseConcluded": "MPL-2.0", "licenseDeclared": "MPL-2.0", "copyrightText": "NOASSERTION"},
            {"SPDXID": "SPDXRef-Qt", "name": "Qt", "versionInfo": qt_version,
             "downloadLocation": "https://download.qt.io/official_releases/qt/",
             "filesAnalyzed": False, "licenseConcluded": "NOASSERTION", "licenseDeclared": "NOASSERTION",
             "copyrightText": "NOASSERTION",
             "comment": "Engineering build. Exact Qt third-party notices and corresponding-source offer remain publication gates."},
            {"SPDXID": "SPDXRef-AppImageRuntime", "name": "AppImage type2 runtime",
             "versionInfo": pin["sourceCommit"], "downloadLocation": pin["sourceUrl"],
             "filesAnalyzed": False, "licenseConcluded": "NOASSERTION", "licenseDeclared": "NOASSERTION",
             "copyrightText": "See AppImage-runtime.txt",
             "checksums": [{"algorithm": "SHA256", "checksumValue": pin["assets"][arch]["sha256"]}],
             "comment": "MIT runtime with statically linked third-party components listed in its license; source/relink obligations must be satisfied before publication."}],
        "relationships": [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                           "relatedSpdxElement": "SPDXRef-KetPlus"}]}
    data = json.dumps(sbom, indent=2, sort_keys=True) + "\n"
    licenses = prefix / "share/licenses/ketplus"
    licenses.mkdir(parents=True, exist_ok=True)
    (licenses / "SBOM.spdx.json").write_text(data)
    (out / "SBOM.spdx.json").write_text(data)


def make_deb(prefix, work, out, version, deb_arch, epoch):
    root = work / "deb"
    destination = root / "opt/ketplus-pro"
    destination.parent.mkdir(parents=True)
    shutil.copytree(prefix, destination, symlinks=True)
    for directory in ["applications", "icons"]:
        shutil.copytree(prefix / "share" / directory, root / "usr/share" / directory, symlinks=True)
    (root / "usr/bin").mkdir(parents=True)
    (root / "usr/bin/KetPlus").write_text("#!/bin/sh\nexec /opt/ketplus-pro/ketplus-pro \"$@\"\n")
    (root / "usr/bin/KetPlus").chmod(0o755)
    unit = root / "usr/lib/systemd/user/ketplus-pro-daemon.service"
    unit.parent.mkdir(parents=True)
    unit.write_text("[Unit]\nDescription=KetPlus Pro user daemon\n\n[Service]\n"
                    "ExecStart=/opt/ketplus-pro/ketplus-pro --daemon\nRestart=on-failure\n"
                    "RestartSec=5\nNoNewPrivileges=true\n\n[Install]\nWantedBy=default.target\n")
    packages = [line.strip() for line in (HERE / "runtime-packages.txt").read_text().splitlines()
                if line.strip() and not line.startswith("#")]
    packages = ["libc6 (>= 2.39)" if name == "libc6" else name for name in packages]
    control = root / "DEBIAN"
    control.mkdir()
    (control / "control").write_text(
        f"Package: ketplus-pro\nVersion: {version}\nArchitecture: {deb_arch}\n"
        "Maintainer: KetPlus maintainers\nSection: devel\nPriority: optional\n"
        f"Depends: {', '.join(packages)}\n"
        "Description: KetPlus Pro desktop engineering build\n"
        " Agent Workspace, editor, Git and the optional user daemon.\n")
    artifact = out / f"ketplus-pro_{version}_{deb_arch}.deb"
    run("dpkg-deb", "--root-owner-group", "--build", root, artifact,
        env={**os.environ, "SOURCE_DATE_EPOCH": str(epoch)})
    return artifact


def make_appimage(prefix, appdir, work, out, version, arch, runtime, epoch):
    (appdir / "AppRun").write_text('#!/bin/sh\nset -eu\nappdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)\nexec "$appdir/usr/ketplus-pro" "$@"\n')
    (appdir / "AppRun").chmod(0o755)
    shutil.copyfile(prefix / "share/applications/ketplus-pro.desktop", appdir / "ketplus-pro.desktop")
    icon = "usr/share/icons/hicolor/256x256/apps/ketplus-pro.png"
    (appdir / "ketplus-pro.png").symlink_to(icon)
    (appdir / ".DirIcon").symlink_to(icon)
    squashfs = work / "desktop.squashfs"
    run("mksquashfs", appdir, squashfs, "-noappend", "-comp", "zstd", "-all-root",
        "-no-xattrs", "-processors", "3", "-mkfs-time", str(epoch), "-all-time", str(epoch),
        env={key: value for key, value in os.environ.items() if key != "SOURCE_DATE_EPOCH"})
    artifact = out / f"KetPlus-Pro-{version}-{arch}.AppImage"
    with artifact.open("wb") as output:
        for source in [runtime, squashfs]:
            with source.open("rb") as stream:
                shutil.copyfileobj(stream, output)
    artifact.chmod(0o755)
    return artifact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--architecture", required=True)
    parser.add_argument("--expected-control-url", required=True)
    parser.add_argument("--qt-version", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9]+)*", args.version):
        raise ValueError("Invalid package version")
    arch, deb_arch = architecture(args.architecture)
    build = args.build_dir.resolve(strict=True)
    cache = read_cache(build)
    if cache.get("KETPLUS_CONTROL_URL") != args.expected_control_url:
        raise ValueError("The requested control URL does not match the configured build")
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    if epoch <= 0:
        raise ValueError("SOURCE_DATE_EPOCH must be the public source commit timestamp")
    out = args.out.absolute()
    if out.exists() or out.is_symlink():
        raise ValueError("Use a new output directory; existing artifacts are never removed")
    out.mkdir(parents=True)
    work = Path(tempfile.mkdtemp(prefix="linux-package-", dir=build))
    appdir = work / "AppDir"
    prefix = appdir / "usr"
    prefix.mkdir(parents=True)
    run("cmake", "--install", build, "--prefix", prefix)
    validate_prefix(prefix)
    info = build_info(prefix, args.expected_control_url)
    if info.get("product_version") != args.version:
        raise ValueError("Installed daemon version differs from the package version")
    pin = json.loads((HERE / "appimage-runtime.json").read_text())
    if digest(REPO / pin["licenseFile"]) != pin["licenseSha256"]:
        raise ValueError("AppImage runtime license differs from the pinned upstream text")
    runtime_cache = build / "linux-package-cache"
    runtime_cache.mkdir(exist_ok=True)
    runtime = verified_runtime(runtime_cache, arch, pin)
    write_sbom(prefix, out, info, args.version, args.qt_version, arch, pin, epoch)
    artifacts = [make_deb(prefix, work, out, args.version, deb_arch, epoch),
                 make_appimage(prefix, appdir, work, out, args.version, arch, runtime, epoch)]
    metadata = {"edition": "pro", "architecture": arch, "version": args.version,
                "qtVersion": args.qt_version, "daemonBuildInfo": info,
                "sourceDateEpoch": epoch, "signed": False, "publicationAuthorized": False}
    (out / "BUILD-METADATA.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    files = artifacts + [out / "BUILD-METADATA.json", out / "SBOM.spdx.json"]
    (out / "SHA256SUMS").write_text("".join(f"{digest(path)}  {path.name}\n" for path in sorted(files)))
    print("Engineering artifacts created; no signing, upload or publication was performed.")


if __name__ == "__main__":
    main()
