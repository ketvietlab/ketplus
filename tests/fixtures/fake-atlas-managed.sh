#!/bin/sh
# A local CLI fixture: record opt-in, never open a browser or start a real renderer.
printf '%s\n' "$@" > "$KETPLUS_ATLAS_FIXTURE_RECORD/argv"
pwd > "$KETPLUS_ATLAS_FIXTURE_RECORD/cwd"
port=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --port) shift; port="$1";;
        --renderer) printf 'renderer\n' > "$KETPLUS_ATLAS_FIXTURE_RECORD/renderer";;
    esac
    shift
done
printf '{"type":"ready","url":"http://127.0.0.1:%s"}\n' "$port"
while IFS= read -r line; do
    case "$line" in *stop*) exit 0;; esac
done
