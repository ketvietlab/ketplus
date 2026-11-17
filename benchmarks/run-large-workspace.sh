#!/usr/bin/env bash
# Opens ketPlus Pro on a synthetic install with thousands of conversations, against a
# private fixture daemon, and reports launch-to-idle CPU, memory and an optional
# time profile. Nothing touches the installed app's preferences, its daemon, the OS
# credential store or a provider: the daemon is ketplus_fixture_daemon (file-backed
# synthetic credentials), provider paths do not exist, and the profile directory and
# journal are temporary copies.
#
# Usage: benchmarks/run-large-workspace.sh <build-dir> [conversations=3000]
#        [repositories=150] [seconds=20] [sample-file]
# The build needs KETPLUS_BUILD_TESTS=ON and KETPLUS_BUILD_BENCHMARKS=ON.
set -euo pipefail

build=${1:?build directory}
conversations=${2:-3000}
repositories=${3:-150}
seconds=${4:-20}
sample_file=${5:-}

app="$build/KetPlus.app/Contents/MacOS/KetPlus"
daemon="$build/ketplus_fixture_daemon"
generator="$build/ketplus_large_workspace_journal"
for tool in "$app" "$daemon" "$generator"; do
    [[ -x "$tool" ]] || { echo "missing $tool" >&2; exit 2; }
done

cache="${TMPDIR:-/tmp}/ketplus-large-workspace-$conversations-$repositories"
if [[ ! -f "$cache/journal.sqlite3" ]]; then
    mkdir -p "$cache"
    "$generator" "$cache" "$conversations" "$repositories" 3 10 >/dev/null
fi

run=$(mktemp -d "${TMPDIR:-/tmp}/ketplus-large-run.XXXXXX")
daemon_pid=""
app_pid=""
cleanup() {
    [[ -n "$app_pid" ]] && kill "$app_pid" 2>/dev/null || true
    [[ -n "$daemon_pid" ]] && kill "$daemon_pid" 2>/dev/null || true
    wait 2>/dev/null || true
    if [[ -n "${KEEP_RUN:-}" ]]; then
        echo "run kept in $run" >&2
    else
        rm -rf "$run"
    fi
}
trap cleanup EXIT

mkdir -p "$run/data/daemon" "$run/profile"
# The daemon migrates and appends to its journal; the cached fixture stays pristine.
cp "$cache/journal.sqlite3" "$run/data/daemon/journal.sqlite3"
export KETPLUS_DAEMON_DATA_DIR="$run/data"
export KETPLUS_DAEMON_SOCKET="$run/daemon.sock"
export KETPLUS_DAEMON_STATE="$run/data/daemon/journal.sqlite3"
export KETPLUS_DAEMON_TOKEN_FILE="$run/data/daemon/local.token"
export KETPLUS_DISABLE_DAEMON_AUTOSTART=1
export KETPLUS_PROFILE_DIR="$run/profile"
export KETPLUS_SIDEBAR_TRACE=1
export KETPLUS_BENCHMARK_AGENT_WORKSPACE=1

missing="$run/no-provider"
"$daemon" --socket "$KETPLUS_DAEMON_SOCKET" --state "$KETPLUS_DAEMON_STATE" \
    --token-file "$KETPLUS_DAEMON_TOKEN_FILE" --codex "$missing" --claude "$missing" \
    --cursor "$missing" --copilot "$missing" >"$run/daemon.log" 2>&1 &
daemon_pid=$!
for _ in $(seq 1 100); do
    [[ -S "$KETPLUS_DAEMON_SOCKET" ]] && break
    sleep 0.1
done
[[ -S "$KETPLUS_DAEMON_SOCKET" ]] || { cat "$run/daemon.log" >&2; exit 3; }

start=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
# Open the busiest repository, as a user returning to their work does.
# WINDOW_GEOMETRY=WxH+X+Y places the window, e.g. on a second display, out of the user's way.
geometry=()
[[ -n "${WINDOW_GEOMETRY:-}" ]] && geometry=(-qwindowgeometry "$WINDOW_GEOMETRY")
"$app" "${geometry[@]}" "$(cd "$cache/repos/repo-000" && pwd -P)" >"$run/app.log" 2>&1 &
app_pid=$!
if [[ -n "$sample_file" ]]; then
    sample "$app_pid" "$seconds" 1 -mayDie -file "$sample_file" >/dev/null 2>&1 &
fi
sleep "$seconds"
if [[ -n "${CAPTURE_DISPLAY:-}" ]]; then
    # One display only (screencapture -D), never the user's main screen.
    screencapture -x -D "$CAPTURE_DISPLAY" "$run/display.png" || true
    cp "$run/display.png" "${CAPTURE_FILE:-/dev/null}" 2>/dev/null || true
fi
end=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')

read -r app_cpu app_rss < <(ps -o cputime=,rss= -p "$app_pid" | awk '{print $1, $2}')
read -r daemon_cpu daemon_rss < <(ps -o cputime=,rss= -p "$daemon_pid" | awk '{print $1, $2}')
# Idle: CPU over the last five seconds, after launch has settled.
before=$(ps -o cputime= -p "$app_pid" | awk -F'[:.]' '{print ($1*60+$2)*100+$3}')
sleep 5
after=$(ps -o cputime= -p "$app_pid" | awk -F'[:.]' '{print ($1*60+$2)*100+$3}')

echo "LARGE_CONVERSATIONS=$conversations"
echo "LARGE_REPOSITORIES=$repositories"
echo "LARGE_WINDOW_SECONDS=$(echo "$end - $start" | bc)"
echo "LARGE_APP_CPU=$app_cpu"
echo "LARGE_APP_RSS_MB=$((app_rss / 1024))"
echo "LARGE_DAEMON_CPU=$daemon_cpu"
echo "LARGE_DAEMON_RSS_MB=$((daemon_rss / 1024))"
refreshes=$(grep -c KETPLUS_SIDEBAR_REFRESH "$run/app.log" || true)
echo "LARGE_SIDEBAR_REFRESHES=$refreshes"
grep KETPLUS_SIDEBAR_REFRESH "$run/app.log" | awk '{split($2,a,"="); t+=a[2]; if (a[2]>m) m=a[2]} END {printf "LARGE_SIDEBAR_REFRESH_TOTAL_MS=%.1f\nLARGE_SIDEBAR_REFRESH_MAX_MS=%.1f\n", t, m}'
tail -1 "$run/app.log" | grep KETPLUS_SIDEBAR_REFRESH | sed 's/^/LARGE_SIDEBAR_LAST: /' || true
echo "LARGE_APP_IDLE_CPU_PERCENT=$(echo "scale=1; ($after - $before) / 5" | bc)"
