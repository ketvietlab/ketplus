#!/usr/bin/env python3
"""Install a Qt 6 MSVC kit from the download.qt.io online repository.

aqtinstall does not yet understand the per-architecture repository layout used
for Qt 6.11, so this reads Updates.xml directly and unpacks the 7z archives.
Needs `py7zr` (pip install py7zr). No Qt account is required.

  python install_qt.py --version 6.11.3 --dest C:\\ketplus-tools\\Qt \\
      --module qtwebsockets --module qtwebview
"""
import argparse
import hashlib
import os
import sys
import tempfile
import urllib.request
import xml.etree.ElementTree as ET

BASE = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop"


def fetch(url, dest=None):
    with urllib.request.urlopen(url, timeout=120) as r:
        data = r.read()
    if dest:
        with open(dest, "wb") as f:
            f.write(data)
    return data


def download(url, dest):
    """Fetch url to dest and verify it against the repository's .sha1 file."""
    fetch(url, dest)
    try:
        expected = fetch(url + ".sha1").decode().split()[0].lower()
    except Exception:  # noqa: BLE001 - repositories without checksums are allowed
        print(f"warning: no checksum for {url.rsplit('/', 1)[-1]}", file=sys.stderr)
        return
    actual = hashlib.sha1(open(dest, "rb").read()).hexdigest()
    if actual != expected:
        sys.exit(f"checksum mismatch for {url}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="e.g. 6.11.3")
    ap.add_argument("--dest", required=True, help="install root; kit lands in <dest>/<version>/msvc2022_64")
    ap.add_argument("--arch", default="msvc2022_64")
    ap.add_argument("--openssl-dest", help="also unpack Qt's prebuilt OpenSSL 3 (include/lib/bin) here")
    ap.add_argument("--openssl-only", action="store_true", help="skip Qt, only unpack OpenSSL")
    ap.add_argument("--module", action="append", default=[], help="add-on, e.g. qtwebsockets")
    a = ap.parse_args()
    import py7zr

    ver = a.version.replace(".", "")
    repo = f"{BASE}/qt6_{ver}/qt6_{ver}_{a.arch}"
    root = ET.fromstring(fetch(f"{repo}/Updates.xml"))
    kit = f"win64_{a.arch}"
    wanted = [f"qt.qt6.{ver}.{kit}"] + [f"qt.qt6.{ver}.addons.{m}.{kit}" for m in a.module]
    found = {p.findtext("Name"): p for p in root.iter("PackageUpdate")}
    target = os.path.join(a.dest, a.version, a.arch)
    os.makedirs(target, exist_ok=True)
    for name in [] if a.openssl_only else wanted:
        pkg = found.get(name)
        if pkg is None:
            sys.exit(f"package not found: {name}")
        archives = [x.strip() for x in (pkg.findtext("DownloadableArchives") or "").split(",") if x.strip()]
        for arc in archives:
            url = f"{repo}/{name}/{pkg.findtext('Version')}{arc}"
            print(f"{name}: {arc}", flush=True)
            with tempfile.TemporaryDirectory() as tmp:
                p = os.path.join(tmp, "a.7z")
                download(url, p)
                with py7zr.SevenZipFile(p) as z:
                    z.extractall(target)
    print("Qt installed in", target)
    if a.openssl_dest:
        repo = f"{BASE}/tools_opensslv3_x64"
        name = "qt.tools.opensslv3.win_x64"
        pkg = next(
            (p for p in ET.fromstring(fetch(f"{repo}/Updates.xml")).iter("PackageUpdate") if p.findtext("Name") == name),
            None,
        )
        if pkg is None:
            sys.exit(f"package not found: {name}")
        arc = next(x.strip() for x in pkg.findtext("DownloadableArchives").split(",") if "openssl" in x)
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "o.7z")
            download(f"{repo}/{name}/{pkg.findtext('Version')}{arc}", p)
            with tempfile.TemporaryDirectory() as ex:
                with py7zr.SevenZipFile(p) as z:
                    z.extractall(ex)
                top = os.path.join(ex, os.listdir(ex)[0])
                import shutil
                shutil.copytree(top if os.path.isdir(top) and len(os.listdir(ex)) == 1 else ex, a.openssl_dest, dirs_exist_ok=True)
        print("OpenSSL installed in", a.openssl_dest)


if __name__ == "__main__":
    main()
