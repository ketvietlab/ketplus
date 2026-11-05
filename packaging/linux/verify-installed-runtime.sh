#!/usr/bin/env bash
# Deterministic package acceptance in Dockerfile.runtime-test, with no provider account/Qt SDK.
set -euo pipefail
test "${KETPLUS_RUNTIME_FIXTURE:-}" = 1
test ! -d /opt/qt
test -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}"
runtime_prefix=${1:?Pass the installed runtime prefix}
test ! -d "$runtime_prefix/plugins/platforms"
env -u LD_LIBRARY_PATH -u QT_PLUGIN_PATH "$runtime_prefix/bin/ketplus_daemon" --check-runtime
"$runtime_prefix/bin/ketplus_daemon" --build-info
smoke_root=$(mktemp -d /tmp/ketplus-runtime-smoke.XXXXXX)
daemon_pid=
cleanup() {
    if [ -n "$daemon_pid" ]; then
        kill -TERM "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -rf "$smoke_root"
}
trap cleanup EXIT
ldd "$runtime_prefix/bin/ketplus_daemon" > "$smoke_root/ldd.log"
if grep -q 'not found' "$smoke_root/ldd.log"; then
    cat "$smoke_root/ldd.log"
    exit 1
fi
readelf -d "$runtime_prefix/bin/ketplus_daemon" | grep -E 'RPATH|RUNPATH'
export KETPLUS_DAEMON_DATA_DIR="$smoke_root/data"
export KETPLUS_DAEMON_SOCKET="$smoke_root/daemon.sock"
export XDG_RUNTIME_DIR="$smoke_root/runtime"
mkdir -m 0700 "$XDG_RUNTIME_DIR"
mkdir "$smoke_root/repository"
git -C "$smoke_root/repository" init -q
start_daemon() {
    "$runtime_prefix/bin/ketplus_daemon" --codex /missing-fixture-codex \
        --claude /missing-fixture-sidecar --cursor /missing-fixture-cursor \
        > "$smoke_root/daemon.log" 2>&1 &
    daemon_pid=$!
    for attempt in $(seq 1 50); do
        if [ -S "$KETPLUS_DAEMON_SOCKET" ]; then return; fi
        sleep .1
    done
    cat "$smoke_root/daemon.log"
    exit 1
}
start_daemon
"$runtime_prefix/bin/ketplusctl" workspace add "$smoke_root/repository" > "$smoke_root/add.json"
"$runtime_prefix/bin/ketplusctl" workspace list > "$smoke_root/catalog.json"
grep -Fq "$smoke_root/repository" "$smoke_root/catalog.json"
test "$(stat -c %a "$smoke_root/data/daemon/local.token")" = 600
kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=
start_daemon
"$runtime_prefix/bin/ketplusctl" workspace list > "$smoke_root/restarted.json"
grep -Fq "$smoke_root/repository" "$smoke_root/restarted.json"
kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=
unit="$runtime_prefix/share/systemd/user/ketplus-daemon.service"
grep -Fxq 'ExecStart=%h/.local/opt/ketplus/current/bin/ketplus_daemon' "$unit"
# Validate the complete unit against this staging prefix without installing/enabling a service.
sed "s|^ExecStart=.*|ExecStart=$runtime_prefix/bin/ketplus_daemon|" "$unit" > "$smoke_root/ketplus-daemon.service"
systemd-analyze verify --man=no "$smoke_root/ketplus-daemon.service" > "$smoke_root/unit.log" 2>&1 || {
    cat "$smoke_root/unit.log"
    exit 1
}
printf '%s\n' 'Clean runtime passed: daemon/CLI, token 0600, catalog restart, SIGTERM, unit syntax; no SDK/display/provider account.'
