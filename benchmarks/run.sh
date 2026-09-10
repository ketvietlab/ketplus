#!/usr/bin/env bash

set -euo pipefail

benchmark_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repository_dir="$(cd "$benchmark_dir/.." && pwd)"
build_dir="$repository_dir/build/benchmark"
results_dir="$benchmark_dir/results"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
commit="$(git -C "$repository_dir" rev-parse --short HEAD)"
result_file="${1:-$results_dir/raw-$timestamp-$commit.txt}"
temporary_dir="$(mktemp -d /tmp/ketplus-benchmark.XXXXXX)"

cleanup() {
    if [[ "$temporary_dir" == /tmp/ketplus-benchmark.* && -d "$temporary_dir" ]]; then
        rm -rf -- "$temporary_dir"
    fi
}
trap cleanup EXIT

mkdir -p "$(dirname "$result_file")"

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
cmake --build "$build_dir" --parallel

benchmark_binary="$build_dir/ketplus_editor_benchmark"
if [[ "$(uname -s)" == "Darwin" ]]; then
    app_binary="$build_dir/KetPlusCM.app/Contents/MacOS/KetPlusCM"
else
    app_binary="$build_dir/KetPlusCM"
fi

"$benchmark_binary" --write-fixture "$temporary_dir/fixture-1mib.txt" 1048576
"$benchmark_binary" --write-fixture "$temporary_dir/fixture-10mib.txt" 10485760
"$benchmark_binary" --write-fixture "$temporary_dir/fixture-50mib.txt" 52428800
cp "$temporary_dir/fixture-10mib.txt" "$temporary_dir/fixture-10mib.cpp"
cp "$temporary_dir/fixture-10mib.txt" "$temporary_dir/fixture-10mib.unknown"

{
    echo "KetPlus benchmark raw results"
    echo "timestamp_utc=$timestamp"
    echo "commit=$commit"
    echo "os=$(uname -srvmp)"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        echo "hardware=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || true)"
        echo "memory_bytes=$(sysctl -n hw.memsize)"
    fi
    echo
    echo "[editor-engine]"
    QT_QPA_PLATFORM=offscreen "$benchmark_binary"

    if [[ "$(uname -s)" == "Darwin" ]]; then
        echo
        echo "[app-startup-native]"
        for specification in \
            "empty:" \
            "1mib_txt:$temporary_dir/fixture-1mib.txt" \
            "10mib_txt:$temporary_dir/fixture-10mib.txt" \
            "50mib_txt:$temporary_dir/fixture-50mib.txt" \
            "10mib_generic:$temporary_dir/fixture-10mib.unknown" \
            "10mib_cpp:$temporary_dir/fixture-10mib.cpp"; do
            label="${specification%%:*}"
            fixture="${specification#*:}"
            for run in 1 2 3 4 5 6 7; do
                echo "CASE=$label RUN=$run"
                if [[ -n "$fixture" ]]; then
                    /usr/bin/time -l env KETPLUS_BENCHMARK_STARTUP=1 "$app_binary" "$fixture"
                else
                    /usr/bin/time -l env KETPLUS_BENCHMARK_STARTUP=1 "$app_binary"
                fi
            done
        done
    fi
} 2>&1 | tee "$result_file"

echo "Saved raw results to $result_file"
