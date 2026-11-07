#!/usr/bin/env bash
set -euo pipefail
repository_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$repository_dir/build/daemon-release}"
if ! rg -q '^CMAKE_BUILD_TYPE:STRING=Release$' "$build_dir/CMakeCache.txt"; then
    echo "Configure a Release daemon build with tests first." >&2
    exit 2
fi
printf 'commit=%s\n' "$(git -C "$repository_dir" rev-parse HEAD)"
printf 'os=%s\n' "$(uname -srvmp)"
export QT_QPA_PLATFORM=offscreen
export KETPLUS_JOURNAL_SCALE_9GIB=1
"$build_dir/ketplus_daemon_journal_space_tests" migratesNineGiBAndChecksRetainedBytes
