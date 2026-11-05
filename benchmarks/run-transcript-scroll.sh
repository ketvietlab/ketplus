#!/usr/bin/env bash
# Transcript scroll benchmark: interleaved arms, N rounds, M1/M2 metrics reduced
# from the layout trace.
#
# Arms are ALWAYS interleaved round by round and scenario by scenario. A previous
# non-interleaved batch had to be discarded because the display refresh rate
# changed mid-session, which shifted every later arm; interleaving makes any such
# drift hit both arms equally instead of one of them.
#
# Usage:
#   run-transcript-scroll.sh --journal PATH [options]
#
#   --journal PATH      daemon journal to replay (operator's own data; copied to a
#                       temp state dir, never written back, never committed)
#   --arm NAME=BINARY   an arm to measure; repeatable. Defaults to a single
#                       "baseline" arm at build/release/...
#   --rounds N          rounds per arm (default 3)
#   --scenarios "A B"   default "pad-up flick-up pad-down"
#   --title-substr S    conversation title substring to open (default $BENCH_TITLE)
#   --out DIR           results directory (default benchmarks/results/scroll-<stamp>)
#   --platform P        Qt platform plugin (default offscreen)
#
# Env: BENCH_MAX_MS, BENCH_LOAD_PAGES are passed through to the binary.
#
# Without a private journal, generate a synthetic one (no real data) and point
# Mermaid at a stub renderer so diagrams go through their loading state:
#   ketplus_synthetic_transcript_journal /tmp/synth 120 3
#   KETPLUS_MMDC=benchmarks/stub-mmdc.sh run-transcript-scroll.sh \
#       --journal /tmp/synth/journal.sqlite3 --title-substr synthetic-scroll
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/.." && pwd)"
python="${PYTHON:-python3}"

journal=""
rounds=3
scenarios="pad-up flick-up pad-down"
title="${BENCH_TITLE:-}"
out=""
platform="offscreen"
arm_names=()
arm_binaries=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --journal) journal="$2"; shift 2 ;;
        --arm) arm_names+=("${2%%=*}"); arm_binaries+=("${2#*=}"); shift 2 ;;
        --rounds) rounds="$2"; shift 2 ;;
        --scenarios) scenarios="$2"; shift 2 ;;
        --title-substr) title="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --platform) platform="$2"; shift 2 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$journal" ]]; then
    echo "--journal is required" >&2
    exit 2
fi
if [[ ! -f "$journal" ]]; then
    echo "journal not found: $journal" >&2
    exit 2
fi
if [[ ${#arm_names[@]} -eq 0 ]]; then
    arm_names=(baseline)
    arm_binaries=("$repo/build/release/ketplus_transcript_scroll_benchmark")
fi
if [[ -z "$out" ]]; then
    out="$repo/benchmarks/results/scroll-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$out"

for binary in "${arm_binaries[@]}"; do
    if [[ ! -x "$binary" ]]; then
        echo "arm binary not executable: $binary" >&2
        echo "build with -DKETPLUS_BUILD_BENCHMARKS=ON -DKETPLUS_BUILD_TRANSCRIPT_SCROLL_BENCH=ON" >&2
        exit 2
    fi
done

export QT_QPA_PLATFORM="$platform"
export KETPLUS_DISABLE_DAEMON_AUTOSTART=1

for round in $(seq 1 "$rounds"); do
    for scenario in $scenarios; do
        for i in "${!arm_names[@]}"; do
            arm="${arm_names[$i]}"
            tag="$arm-$scenario-r$round"
            run_dir="$out/$tag"
            mkdir -p "$run_dir"
            title_args=()
            [[ -n "$title" ]] && title_args=(--title-substr "$title")
            KETPLUS_CHAT_LAYOUT_TRACE="$run_dir/layout-trace.jsonl" \
                "${arm_binaries[$i]}" "$journal" "$scenario" "$run_dir" \
                "${title_args[@]}" >"$run_dir/run.log" 2>&1 \
                && status=0 || status=$?
            echo "$tag exit=$status"
        done
    done
done

echo
echo "== M1 / M2 =="
for i in "${!arm_names[@]}"; do
    arm="${arm_names[$i]}"
    traces=("$out/$arm"-*/layout-trace.jsonl)
    if [[ -e "${traces[0]}" ]]; then
        echo "--- arm: $arm"
        "$python" "$here/transcript-scroll-metrics.py" "${traces[@]}" \
            --json "$out/$arm-metrics.json"
    fi
done
echo "results: $out"
