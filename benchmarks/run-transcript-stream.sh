#!/usr/bin/env bash
set -euo pipefail
# Pass a Release build directory. No provider or running daemon is contacted.
build_dir="${1:-build/release}"
output="${2:-/tmp/ketplus-transcript-stream-benchmark.log}"
QT_QPA_PLATFORM=offscreen KETPLUS_DISABLE_DAEMON_AUTOSTART=1 \
  KETPLUS_RUN_STREAM_BENCHMARK=1 \
  "$build_dir/ketplus_agent_streaming_tests" streamBenchmark 2>&1 | tee "$output"
