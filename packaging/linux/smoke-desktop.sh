#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Disposable --network=none container only. Never run on the host or enable a service.
set -euo pipefail
test ! -d /opt/qt
artifact=${1:?Pass an AppImage or deb mounted read-only}
expected_url=${2:?Pass the configured production URL}
work=$(mktemp -d)
case "$artifact" in
  *.AppImage)
    cd "$work"
    "$artifact" --appimage-extract > extract.log
    prefix="$work/squashfs-root/usr"
    launcher="$work/squashfs-root/AppRun"
    ;;
  *.deb)
    dpkg -i "$artifact"
    prefix=/opt/ketplus-pro
    launcher="$prefix/ketplus-pro"
    test -f /usr/lib/systemd/user/ketplus-pro-daemon.service
    systemd-analyze verify --man=no /usr/lib/systemd/user/ketplus-pro-daemon.service
    ;;
  *) echo 'Unsupported artifact' >&2; exit 1 ;;
esac
export HOME="$work/home" XDG_CONFIG_HOME="$work/config" XDG_CACHE_HOME="$work/cache"
export XDG_DATA_HOME="$work/data" XDG_RUNTIME_DIR="$work/run"
mkdir -m 700 "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
unset LD_LIBRARY_PATH QT_PLUGIN_PATH QML_IMPORT_PATH QML2_IMPORT_PATH
"$launcher" --build-info > "$work/build-info.json"
python3 - "$work/build-info.json" "$expected_url" <<'PY'
import json, sys
info = json.load(open(sys.argv[1]))
assert info['control_url'] == sys.argv[2], info
assert info['control_url_overridable'] is False, info
print('Production build-info policy passed')
PY
linkage_ok=1
check_linkage() {
  if ! ldd "$1" > "$work/ldd.log" 2>&1 || grep -q 'not found' "$work/ldd.log"; then
    printf 'Unresolved executable, plugin or library: %s\n' "$1"
    cat "$work/ldd.log"
    linkage_ok=0
  fi
}
for executable in "$prefix/bin/KetPlus" "$prefix/bin/ketplus_daemon" "$prefix/libexec/QtWebEngineProcess"; do
  check_linkage "$executable"
done
while IFS= read -r library; do
  check_linkage "$library"
done < <(find "$prefix/lib" "$prefix/plugins" "$prefix/qml" -type f \( -name '*.so' -o -name '*.so.[0-9]*' \))
test "$linkage_ok" = 1
"$prefix/lib/ketplus/ketplus-browser/bin/node" --version
export KETPLUS_DAEMON_DATA_DIR="$work/daemon-data" KETPLUS_DAEMON_SOCKET="$work/daemon.sock"
daemon_pid=
trap 'if [ -n "$daemon_pid" ]; then kill -TERM "$daemon_pid"; wait "$daemon_pid"; fi' EXIT
"$launcher" --daemon --codex /missing-fixture --claude /missing-fixture --cursor /missing-fixture > "$work/daemon.log" 2>&1 &
daemon_pid=$!
for attempt in $(seq 1 100); do
  if [ -S "$KETPLUS_DAEMON_SOCKET" ]; then break; fi
  sleep .1
done
test -S "$KETPLUS_DAEMON_SOCKET" || { cat "$work/daemon.log"; exit 1; }
mkdir "$work/repository"
git -C "$work/repository" init -q
"$prefix/bin/ketplusctl" workspace add "$work/repository" > "$work/workspace.json"
"$prefix/bin/ketplusctl" workspace list | grep -F "$work/repository"
test "$(stat -c %a "$work/daemon-data/daemon/local.token")" = 600
kill -TERM "$daemon_pid"
wait "$daemon_pid"
daemon_pid=
# Root is used only by this disposable test image. Product launcher never disables sandboxing.
export QTWEBENGINE_DISABLE_SANDBOX=1 QTWEBENGINE_CHROMIUM_FLAGS='--disable-gpu'
export LIBGL_ALWAYS_SOFTWARE=1 KETPLUS_BENCHMARK_STARTUP=1 QT_QPA_PLATFORM=xcb
check_child_environment() {
  timeout 60 xvfb-run -a "$launcher" --check-child-environment > "$work/child-environment.json"
  python3 - "$work/child-environment.json" <<'PY'
import json, os, sys
expected = {key: value for key, value in os.environ.items()
            if key in {'LD_LIBRARY_PATH', 'QT_PLUGIN_PATH'} or key.startswith(('QML', 'QTWEBENGINE'))}
actual = json.load(open(sys.argv[1]))
assert actual == expected, 'Installed app changed loader/Qt environment inherited by its child'
print('Installed app child retains original loader/Qt environment')
PY
}
check_child_environment
export LD_LIBRARY_PATH="$work/user-libraries" QT_PLUGIN_PATH="" QML2_IMPORT_PATH="$work/user-qml"
export QTWEBENGINE_TEST_ORIGINAL='synthetic original value'
check_child_environment
unset LD_LIBRARY_PATH QT_PLUGIN_PATH QML2_IMPORT_PATH QTWEBENGINE_TEST_ORIGINAL
timeout 60 xvfb-run -a "$launcher" > "$work/gui.log" 2>&1 || { cat "$work/gui.log"; exit 1; }
grep '^KETPLUS_STARTUP_MS=' "$work/gui.log"
if grep -E 'module .* is not installed|Cannot load library|Could not load the Qt platform plugin' "$work/gui.log"; then exit 1; fi
echo 'Clean SDK-free smoke passed: installed GUI, daemon/CLI, token permissions and runtime linkage; network disabled.'
