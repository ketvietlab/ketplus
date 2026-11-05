#!/usr/bin/env bash

set -euo pipefail

benchmark_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repository_dir="$(cd "$benchmark_dir/.." && pwd)"
build_dir="$repository_dir/build/benchmark"
results_dir="$benchmark_dir/results"
instances="${1:-5}"
conversations="${2:-6}"
messages="${3:-200}"
timestamp="$(date -u +%Y%m%dT%H%M%SZ)"
result_file="${4:-$results_dir/raw-$timestamp-agent-multi-app.txt}"
temporary_dir="$(mktemp -d /tmp/ketplus-agent-multi.XXXXXX)"

cleanup() {
    if [[ "$temporary_dir" == /tmp/ketplus-agent-multi.* && -d "$temporary_dir" ]]; then
        rm -rf -- "$temporary_dir"
    fi
}
trap cleanup EXIT

if [[ -z "${QT_ROOT:-}" ]]; then
    QT_ROOT="$(brew --prefix qt)"
    export QT_ROOT
fi

cmake -S "$repository_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_ROOT" \
    -DKETPLUS_BUILD_TESTS=OFF \
    -DKETPLUS_BUILD_BENCHMARKS=ON
cmake --build "$build_dir" --target ketplus_agent_benchmark --parallel

mkdir -p "$(dirname "$result_file")"
{
    declare -a pids=()
    declare -a outputs=()

    for ((instance = 1; instance <= instances; ++instance)); do
        repo="$temporary_dir/repo-$instance"
        mkdir -p "$repo"
        for ((conversation = 1; conversation <= conversations; ++conversation)); do
            mkdir -p "$repo/conversation-$conversation"
        done
        output="$temporary_dir/instance-$instance.txt"
        outputs+=("$output")
        "$build_dir/ketplus_agent_benchmark" \
            --multi-app-stress \
            --instance "$instance" \
            --instances "$instances" \
            --repository "$repo" \
            --conversations "$conversations" \
            --messages-per-conversation "$messages" >"$output" 2>&1 &
        pids+=("$!")
    done

    pid_csv="$(IFS=,; echo "${pids[*]}")"
    echo "KetPlus multi-app Agent Workspace raw results"
    echo "timestamp_utc=$timestamp"
    echo "commit=$(git -C "$repository_dir" rev-parse --short HEAD)"
    echo "instances=$instances"
    echo "conversations_per_instance=$conversations"
    echo "messages_per_conversation=$messages"
    echo "total_resident_messages=$((instances * conversations * messages))"
    echo
    echo "[aggregate-process-samples]"
    second=0
    while true; do
        process_rows="$(ps -o rss= -o %cpu= -o stat= -p "$pid_csv" 2>/dev/null || true)"
        sample="$(awk '
            NF >= 3 && $3 !~ /^Z/ { rss += $1; cpu += $2; count += 1 }
            END { printf "%d %.3f %d", rss, cpu, count }
        ' <<<"$process_rows")"
        read -r rss cpu alive <<<"$sample"
        if [[ "$alive" -eq 0 ]]; then
            break
        fi
        echo "SAMPLE_SECOND=$second ALIVE=$alive TOTAL_RSS_KIB=$rss TOTAL_CPU_PERCENT=$cpu"
        second=$((second + 1))
        sleep 1
    done

    echo
    echo "[per-instance]"
    failed=0
    for index in "${!pids[@]}"; do
        if ! wait "${pids[$index]}"; then
            failed=1
        fi
        cat "${outputs[$index]}"
    done
    exit "$failed"
} 2>&1 | tee "$result_file"
