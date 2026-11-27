#!/usr/bin/env bash
# Opt-in production helper/runtime/IPC checks. No host-policy/bootstrap changes.
set -euo pipefail
if [[ $# != 1 || ! -d "$1" ]]; then
    echo 'Usage: executor-acceptance.sh <Linux daemon build directory>' >&2
    exit 64
fi
if [[ $(uname -s) != Linux || $(id -u) == 0 ]]; then
    echo 'Use an exclusively owned Linux guest as the non-root daemon user.' >&2
    exit 64
fi
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"
export KETPLUS_RUN_LINUX_JOB_TESTS=1
export QT_QPA_PLATFORM=offscreen
ctest --test-dir "$1" --output-on-failure \
    -R '^ketplus_(linux_job_executor|managed_task_(host|ipc)|background_(output|ledger)|managed_task_spec)_tests$'
