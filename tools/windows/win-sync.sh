#!/usr/bin/env bash
# Keep this worktree mirrored to a folder on the Windows build machine (one-way,
# Mac -> Windows) with mutagen. Build outputs and VCS data are not synced, so a
# Windows build tree is never overwritten or deleted by the sync.
#
#   KETPLUS_WIN_HOST=user@host tools/windows/win-sync.sh start|stop|status|flush
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/_common.sh"
command -v mutagen >/dev/null || { echo "mutagen not found (see docs/development.md, Windows)" >&2; exit 1; }

case "${1:-}" in
start)
    mutagen daemon start >/dev/null
    if mutagen sync list "$SESSION" >/dev/null 2>&1; then
        echo "session $SESSION already exists"
    else
        # Mutagen does not create the parent folder on a Windows endpoint.
        ssh -o BatchMode=yes "$KETPLUS_WIN_HOST" "if not exist \"${KETPLUS_WIN_ROOT//\//\\}\" mkdir \"${KETPLUS_WIN_ROOT//\//\\}\"" >/dev/null
        mutagen sync create --name "$SESSION" --mode one-way-replica --ignore-vcs \
            --ignore '/build' --ignore '/build-*' --ignore '/.cache' --ignore '.DS_Store' \
            --ignore '/.claude' --ignore '/.ketplus' \
            "$REPO_ROOT" "$KETPLUS_WIN_HOST:$WIN_DIR_BS"
    fi
    mutagen sync flush "$SESSION"
    echo "synced to $WIN_DIR_BS"
    ;;
stop) mutagen sync terminate "$SESSION" ;;
status) mutagen sync list "$SESSION" ;;
flush) mutagen sync flush "$SESSION" ;;
*) echo "usage: $0 start|stop|status|flush" >&2; exit 2 ;;
esac
