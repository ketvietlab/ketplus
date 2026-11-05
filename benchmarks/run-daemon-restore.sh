#!/usr/bin/env bash

set -euo pipefail

benchmark_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repository_dir="$(cd "$benchmark_dir/.." && pwd)"
build_dir="$repository_dir/build/benchmark"
results_dir="$benchmark_dir/results"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
commit="$(git -C "$repository_dir" rev-parse --short HEAD)"
result_file="${1:-$results_dir/raw-$timestamp-daemon-restore-$commit.txt}"

if [[ -z "${QT_ROOT:-}" ]]; then
    if command -v brew >/dev/null 2>&1; then
        QT_ROOT="$(brew --prefix qt)"
        export QT_ROOT
    else
        echo "QT_ROOT must point to a Qt 6 installation." >&2
        exit 2
    fi
fi

cmake -S "$repository_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_ROOT" \
    -DKETPLUS_BUILD_TESTS=OFF \
    -DKETPLUS_BUILD_BENCHMARKS=ON
cmake --build "$build_dir" --target ketplus_daemon_restore_benchmark --parallel
mkdir -p "$(dirname "$result_file")"

{
    echo "KetPlus daemon restore benchmark raw results"
    echo "timestamp_utc=$timestamp"
    echo "commit=$commit"
    echo "os=$(uname -srvmp)"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        echo "hardware=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)"
        echo "memory_bytes=$(sysctl -n hw.memsize)"
    fi
    for run in 1 2 3; do
        echo
        echo "RUN=$run"
        "$build_dir/ketplus_daemon_restore_benchmark"
    done
} 2>&1 | tee "$result_file"

echo "Saved raw results to $result_file"
