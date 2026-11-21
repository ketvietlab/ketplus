# Shared by win-sync.sh and win-run.sh. Sourced, not executed.
#
# KETPLUS_WIN_HOST   required   ssh target, e.g. user@host (never committed)
# KETPLUS_WIN_ROOT   optional   parent folder on Windows (default C:/ketplus-work)
# KETPLUS_WIN_NAME   optional   folder/session name (default: this worktree's directory name)
export PATH="$HOME/.local/bin:$PATH"

: "${KETPLUS_WIN_HOST:?set KETPLUS_WIN_HOST to the ssh target (user@host)}"
KETPLUS_WIN_ROOT="${KETPLUS_WIN_ROOT:-C:/ketplus-work}"
REPO_ROOT="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
KETPLUS_WIN_NAME="${KETPLUS_WIN_NAME:-$(basename "$REPO_ROOT")}"
# Mutagen session names allow letters, digits and dashes only.
SESSION="kpwin-$(printf '%s' "$KETPLUS_WIN_NAME" | tr -c 'A-Za-z0-9\n' '-')"
WIN_DIR="$KETPLUS_WIN_ROOT/$KETPLUS_WIN_NAME"
WIN_DIR_BS="${WIN_DIR//\//\\}"
