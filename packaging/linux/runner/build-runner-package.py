#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Assemble the unsigned runner .deb from the Runner install component; never publish or sign.

    build-runner-package.py --build BUILD --out OUT --expected-control-url URL [--revision N]
                            [--test-only]

BUILD is a Pro Release tree configured with -DKETPLUS_LINUX_RUNNER_PACKAGE=ON. The package holds
the runner, its job executor and their Qt runtime under /usr/lib/ketplus-runner, the root
wrapper /usr/bin/ketplus-runner, the system service and the executor's AppArmor profile.
A build for an http loopback Relay is a test package: it needs --test-only and is never
published. SOURCE_DATE_EPOCH must be the source commit's timestamp.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
INSTALL_ROOT = "usr/lib/ketplus-runner"
SYSTEM_LIBRARY_DIRS = ("/lib/", "/lib64/", "/usr/lib/", "/usr/lib64/")
REQUIRED = ["bin/ketplus-runner", "bin/ketplus_job_executor", "bin/qt.conf",
            "lib/libQt6Core.so.6", "lib/libQt6Network.so.6", "lib/libQt6Sql.so.6",
            "plugins/sqldrivers/libqsqlite.so"]
# Packages the runner needs at run time beyond its shared libraries: the job sandbox runs in
# the account's user manager over its session bus, and the wrapper switches to the account.
RUNTIME_PACKAGES = ["systemd", "dbus-user-session", "passwd", "util-linux"]


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def output(*args, env=None):
    return run(*args, capture_output=True, text=True, env=env).stdout


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_cache(build):
    values = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line and ":" in line:
            key, value = line.split("=", 1)
            values[key.split(":", 1)[0]] = value
    if values.get("KETPLUS_EDITION") != "pro":
        raise ValueError("KETPLUS_EDITION must be pro for the runner package")
    if values.get("CMAKE_BUILD_TYPE") != "Release":
        raise ValueError("CMAKE_BUILD_TYPE must be Release for the runner package")
    if values.get("KETPLUS_LINUX_RUNNER_PACKAGE") != "ON":
        raise ValueError("Configure the build with -DKETPLUS_LINUX_RUNNER_PACKAGE=ON")
    if values.get("KETPLUS_LINUX_DESKTOP_PACKAGE") == "ON":
        raise ValueError("The runner package needs a build without the desktop package")
    return values


def project_version():
    text = (REPO / "CMakeLists.txt").read_text()
    match = re.search(r"project\(\s*\w+\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", text)
    if not match:
        raise ValueError("No project version in CMakeLists.txt")
    return match.group(1)


def loopback_http(url):
    return re.fullmatch(r"http://(127\.0\.0\.1|localhost|\[::1\])(:[0-9]+)?(/.*)?", url) is not None


def elf_files(prefix):
    for path in sorted(prefix.rglob("*")):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as stream:
                if stream.read(4) == b"\x7fELF":
                    yield path


def check_prefix(prefix, build):
    missing = [name for name in REQUIRED if not (prefix / name).is_file()]
    if missing:
        raise ValueError("Incomplete Runner component: " + ", ".join(missing))
    for name in ["bin/ketplus-runner", "bin/ketplus_job_executor"]:
        if not os.access(prefix / name, os.X_OK):
            raise ValueError(f"Not executable: {name}")
    extra = {path.name for path in (prefix / "plugins/sqldrivers").glob("*.so")} - {"libqsqlite.so"}
    if extra:
        raise ValueError("Only the SQLite driver may be bundled: " + ", ".join(sorted(extra)))
    for unwanted in ["plugins/platforms", "plugins/platformthemes", "plugins/styles"]:
        if (prefix / unwanted).exists():
            raise ValueError(f"A GUI plugin type was deployed: {unwanted}")
    for path in prefix.rglob("*"):
        if path.is_symlink():
            try:
                path.resolve(strict=True).relative_to(prefix.resolve())
            except (ValueError, FileNotFoundError) as error:
                raise ValueError(f"Symlink escapes the package: {path.relative_to(prefix)}") from error
    for line in (prefix / "bin/qt.conf").read_text().splitlines():
        if "=" in line and line.split("=", 1)[1].strip().startswith("/"):
            raise ValueError(f"qt.conf names an absolute path: {line.strip()}")
    forbidden = [str(build.resolve()), str(REPO)]
    for path in elf_files(prefix):
        dynamic = output("readelf", "-d", path)
        for paths in re.findall(r"\((?:RPATH|RUNPATH)\)\s+Library \w+: \[(.*)\]", dynamic):
            for entry in paths.split(":"):
                if entry and not entry.startswith("$ORIGIN"):
                    raise ValueError(f"{path.relative_to(prefix)} has a run path outside the "
                                     f"package: {entry}")
        data = path.read_bytes()
        for name in forbidden:
            if name.encode() in data:
                raise ValueError(f"{path.relative_to(prefix)} names the build or source tree")


def system_libraries(prefix):
    """Every shared library the package loads from the system, by ldd without LD_* set."""
    env = {key: value for key, value in os.environ.items() if not key.startswith("LD_")}
    found = set()
    for path in elf_files(prefix):
        for line in output("ldd", path, env=env).splitlines():
            if "not found" in line:
                raise ValueError(f"{path.relative_to(prefix)}: {line.strip()}")
            match = re.search(r"=>\s+(/\S+)", line)
            if not match:
                continue
            library = Path(match.group(1)).resolve()
            if library.is_relative_to(prefix.resolve()):
                continue
            if not str(library).startswith(SYSTEM_LIBRARY_DIRS):
                raise ValueError(f"{path.relative_to(prefix)} loads {library} from outside the "
                                 "system library directories")
            found.add(str(library))
    return sorted(found)


def library_packages(libraries):
    packages = set()
    for library in libraries:
        # dpkg knows the path under /usr after the merge of /lib into /usr/lib.
        candidates = [library, library.replace("/usr/lib/", "/lib/", 1)]
        for candidate in candidates:
            result = subprocess.run(["dpkg-query", "-S", candidate], capture_output=True,
                                    text=True)
            if result.returncode == 0:
                packages.add(result.stdout.split(":", 1)[0].strip())
                break
        else:
            raise ValueError(f"No package provides {library}")
    return sorted(packages)


def depends(libraries):
    packages = sorted(set(library_packages(libraries)) | set(RUNTIME_PACKAGES))
    return ["libc6 (>= 2.39)" if name == "libc6" else name for name in packages]


def copy_file(source, target, mode):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    target.chmod(mode)


def make_deb(prefix, work, out, version, arch, packages, test_only):
    root = work / "deb"
    shutil.copytree(prefix, root / INSTALL_ROOT, symlinks=True)
    copy_file(HERE / "start-user-manager", root / INSTALL_ROOT / "libexec/start-user-manager",
              0o755)
    copy_file(HERE / "ketplus-runner.wrapper", root / "usr/bin/ketplus-runner", 0o755)
    copy_file(HERE / "ketplus-runner.service",
              root / "usr/lib/systemd/system/ketplus-runner.service", 0o644)
    copy_file(HERE / "ketplus-job-executor.apparmor", root / "etc/apparmor.d/ketplus-job-executor",
              0o644)
    control = root / "DEBIAN"
    control.mkdir()
    for script in ["postinst", "prerm", "postrm"]:
        copy_file(HERE / script, control / script, 0o755)
    (control / "conffiles").write_text("/etc/apparmor.d/ketplus-job-executor\n")
    summary = "KetPlus runner (TEST ONLY: loopback Relay, never publish)" if test_only \
        else "KetPlus runner"
    (control / "control").write_text(
        f"Package: ketplus-runner\nVersion: {version}\nArchitecture: {arch}\n"
        "Maintainer: KetPlus maintainers\nSection: devel\nPriority: optional\n"
        f"Depends: {', '.join(packages)}\nRecommends: apparmor\n"
        f"Description: {summary}\n"
        " Runs the build and test steps a KetPlus desktop sends it, each job in its own\n"
        " sandbox, as a system service under the ketplus-runner account.\n")
    root.chmod(0o755)
    for path in root.rglob("*"):
        if path.is_symlink():
            continue
        if path.is_dir() or os.access(path, os.X_OK):
            path.chmod(0o755)
        else:
            path.chmod(0o644)
    suffix = "_test-only" if test_only else ""
    artifact = out / f"ketplus-runner_{version}_{arch}{suffix}.deb"
    run("dpkg-deb", "--root-owner-group", "-Zxz", "--build", root, artifact)
    return artifact


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--expected-control-url", required=True)
    parser.add_argument("--revision", type=int, default=1)
    parser.add_argument("--test-only", action="store_true")
    args = parser.parse_args()
    if args.revision < 1:
        raise ValueError("--revision must be 1 or more")
    cache = read_cache(args.build)
    if cache.get("KETPLUS_CONTROL_URL") != args.expected_control_url:
        raise ValueError("The requested control URL does not match the configured build")
    if loopback_http(args.expected_control_url) != args.test_only:
        raise ValueError("A loopback http Relay is for a --test-only package, and only it")
    if int(os.environ.get("SOURCE_DATE_EPOCH", "0")) <= 0:
        raise ValueError("SOURCE_DATE_EPOCH must be the source commit timestamp")
    upstream = project_version()
    arch = output("dpkg", "--print-architecture").strip()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ketplus-runner-package-") as temporary:
        work = Path(temporary)
        prefix = work / "prefix"
        run("cmake", "--install", args.build, "--component", "Runner", "--prefix", prefix)
        reported = output(prefix / "bin/ketplus-runner", "--version").strip()
        if reported != f"ketplus-runner {upstream}":
            raise ValueError(f"The runner reports {reported!r}, the project is {upstream}")
        check_prefix(prefix, args.build)
        packages = depends(system_libraries(prefix))
        artifact = make_deb(prefix, work, args.out, f"{upstream}-{args.revision}", arch,
                            packages, args.test_only)
    sums = args.out / "SHA256SUMS"
    sums.write_text(f"{digest(artifact)}  {artifact.name}\n")
    print(artifact)


if __name__ == "__main__":
    main()
