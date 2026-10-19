#!/usr/bin/env bash
# Builds and signs "KetPlus Computer.app" for the ADR 0026 spike.
#
# TCC keys its grants to the code signature's designated requirement. Sign with
# a real identity (pass its SHA-1 hash with --identity) so grants survive
# rebuilds; an ad-hoc signature changes on every build and resets them.
set -euo pipefail

usage() {
    echo "usage: build-app.sh [--identity <sha1-or-name>] [--out <dir>]" >&2
    exit 2
}

here="$(cd "$(dirname "$0")/.." && pwd)"
identity="-"
out="$here/dist"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --identity) identity="${2:-}"; shift 2 ;;
        --out) out="${2:-}"; shift 2 ;;
        *) usage ;;
    esac
done
[[ -n "$identity" && -n "$out" ]] || usage

swift build --package-path "$here" -c release
bin="$(swift build --package-path "$here" -c release --show-bin-path)"

app="$out/KetPlus Computer.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
cp "$here/packaging/Info.plist" "$app/Contents/Info.plist"
cp "$bin/KetPlusComputer" "$app/Contents/MacOS/KetPlusComputer"
cp "$bin/ketplus-computer-mcp" "$app/Contents/MacOS/ketplus-computer-mcp"

if [[ "$identity" == "-" ]]; then
    echo "warning: ad-hoc signature; TCC grants reset on every rebuild" >&2
    sign=(codesign --force --sign -)
else
    sign=(codesign --force --timestamp --options runtime --sign "$identity")
fi
# Sign the nested tool first, then the bundle (no --deep).
"${sign[@]}" "$app/Contents/MacOS/ketplus-computer-mcp"
"${sign[@]}" "$app"
codesign --verify --strict "$app"
echo "$app"
