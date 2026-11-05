#!/bin/sh
# Stand-in for mmdc in the transcript scroll benchmark: takes a realistic
# moment to finish, so a diagram is seen in its loading state before its image
# arrives. Set KETPLUS_MMDC to this path. BENCH_MMDC_DELAY overrides the delay.
sleep "${BENCH_MMDC_DELAY:-0.8}"
while [ "$#" -gt 0 ]; do
    if [ "$1" = "-o" ]; then output="$2"; shift 2; else shift; fi
done
printf '%s' '<svg xmlns="http://www.w3.org/2000/svg" width="400" height="700"><rect width="400" height="700" fill="#2563eb"/></svg>' >"$output"
