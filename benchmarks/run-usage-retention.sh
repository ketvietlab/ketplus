#!/usr/bin/env bash
set -euo pipefail

repository_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$repository_dir/build/daemon-release}"
if ! grep -q '^CMAKE_BUILD_TYPE:STRING=Release$' "$build_dir/CMakeCache.txt"; then
    echo "Configure a Release daemon build with KETPLUS_BUILD_TESTS=ON first." >&2
    exit 2
fi
export QT_QPA_PLATFORM=offscreen
printf 'commit=%s\n' "$(git -C "$repository_dir" rev-parse HEAD)"
printf 'os=%s\n' "$(uname -srvmp)"
if [[ "$(uname -s)" == Darwin ]]; then
    printf 'hardware=%s\n' "$(sysctl -n machdep.cpu.brand_string)"
    printf 'memory_bytes=%s\n' "$(sysctl -n hw.memsize)"
fi
for run in 1 2 3; do
    printf '\nRUN=%s\n' "$run"
    "$build_dir/ketplus_daemon_usage_retention_tests" rawBacklogUsesTurnLookup commandBudgetResumesAndRollbackRestoresPayloads \
        toolAndReasoningRetentionUsesTurnGuards:mcpToolCall-eligible \
        toolAndReasoningRetentionUsesTurnGuards:dynamicToolCall-eligible \
        toolAndReasoningRetentionUsesTurnGuards:reasoning-eligible
done
