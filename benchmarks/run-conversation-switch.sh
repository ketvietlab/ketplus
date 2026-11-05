#!/usr/bin/env bash
set -euo pipefail
build_dir="${1:-build/release}"
result_file="${2:-/tmp/ketplus-conversation-switch.txt}"
baseline="${3:-}"
if ! grep -q '^CMAKE_BUILD_TYPE:STRING=Release$' "$build_dir/CMakeCache.txt"; then
    echo 'Configure a Release tree with KETPLUS_BUILD_BENCHMARKS=ON first.' >&2
    exit 2
fi
cmake --build "$build_dir" --target ketplus_conversation_switch_benchmark --parallel 6
{
    echo "revision=$(git rev-parse HEAD)"
    echo "tracked_diff_sha256=$(git diff HEAD | shasum -a 256 | cut -d ' ' -f 1)"
    grep -E '^CMAKE_BUILD_TYPE:|^CMAKE_CXX_FLAGS_RELEASE:' "$build_dir/CMakeCache.txt"
    echo "os=$(uname -srvmp)"
    if [[ "$(uname -s)" == Darwin ]]; then
        echo "hardware=$(sysctl -n machdep.cpu.brand_string)"
        echo "memory_bytes=$(sysctl -n hw.memsize)"
    fi
    for run in 1 2 3; do
        echo "run=$run"
        if [[ -n "$baseline" ]]; then
            echo 'arm=baseline'
            QT_QPA_PLATFORM=offscreen "$baseline" history 8
            QT_QPA_PLATFORM=offscreen "$baseline" measure 8
        fi
        echo 'arm=candidate'
        QT_QPA_PLATFORM=offscreen "$build_dir/ketplus_conversation_switch_benchmark" history 8
        QT_QPA_PLATFORM=offscreen "$build_dir/ketplus_conversation_switch_benchmark" measure 8
    done
} 2>&1 | tee "$result_file"
