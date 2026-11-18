#!/usr/bin/env bash
# Run a command on the Windows build machine, in the synced copy of this worktree,
# inside the MSVC x64 + CMake + Ninja + Qt + OpenSSL environment (vsenv.cmd).
# Flushes the sync first so the Windows tree matches the Mac one.
#
#   KETPLUS_WIN_HOST=user@host tools/windows/win-run.sh cmake --preset lite-dev
#   KETPLUS_WIN_HOST=user@host tools/windows/win-run.sh cmake --build --preset lite-dev
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
[ $# -gt 0 ] || { echo "usage: $0 <command> [args...]" >&2; exit 2; }

if command -v mutagen >/dev/null && mutagen sync list "$SESSION" >/dev/null 2>&1; then
    mutagen sync flush "$SESSION"
else
    echo "no sync session $SESSION; run win-sync.sh start first" >&2
    exit 1
fi
# The remote shell is cmd.exe: join arguments verbatim.
ssh -o BatchMode=yes "$KETPLUS_WIN_HOST" "cd /d \"$WIN_DIR_BS\" && tools\\windows\\vsenv.cmd $*"
