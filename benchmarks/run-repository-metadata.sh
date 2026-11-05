#!/usr/bin/env bash
set -euo pipefail
repository_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake --preset release -S "$repository_dir" -DKETPLUS_BUILD_BENCHMARKS=ON
cmake --build "$repository_dir/build/release" --target ketplus_repository_metadata_benchmark --parallel 4
for run in 1 2 3; do
    echo "RUN=$run"
    "$repository_dir/build/release/ketplus_repository_metadata_benchmark"
done
