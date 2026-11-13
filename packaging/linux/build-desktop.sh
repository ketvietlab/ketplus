#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Run inside Dockerfile.build on a native amd64 or arm64 host.
set -euo pipefail
source_dir=${1:-/src}
build_dir=${2:-/build/desktop}
output_dir=${3:-$build_dir/linux-packages}
if [ -n "$(git -C "$source_dir" status --porcelain)" ]; then
  echo 'Commit and sync the exact public source before building engineering artifacts.' >&2
  exit 1
fi
export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH=$(git -C "$source_dir" show -s --format=%ct HEAD)
export QT_QPA_PLATFORM=offscreen LANG=C.UTF-8
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_PREFIX_PATH="${QT_ROOT:-/opt/qt/current}" \
  -DKETPLUS_EDITION=pro -DKETPLUS_BUILD_DESKTOP=ON -DKETPLUS_BUILD_TESTS=ON \
  -DKETPLUS_PRODUCTION_RELEASE=ON -DKETPLUS_ENABLE_REMOTE_DEMO=OFF \
  -DKETPLUS_LINUX_DESKTOP_PACKAGE=ON \
  -DKETPLUS_LINUX_PACKAGE_OUTPUT_DIR="$output_dir"
nice -n 10 cmake --build "$build_dir" --parallel 3 --target \
  ketplus ketplus_daemon ketplusctl ketplus_bundled_browser_runtime_tests ketplus_daemon_claude_adapter_tests
ctest --test-dir "$build_dir" --output-on-failure \
  -R '^ketplus_(bundled_browser_runtime|production_control|daemon_control_build_info|daemon_claude_adapter)_tests$'
python3 "$source_dir/tests/packaging/test_linux_desktop_packages.py"
nice -n 10 cmake --build "$build_dir" --parallel 3 --target ketplus_linux_packages
