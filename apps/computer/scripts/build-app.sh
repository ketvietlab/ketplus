#!/usr/bin/env bash
# Builds and signs "KetPlus Computer.app" (ADR 0026).
#
# TCC keys its grants to the code signature's designated requirement. Sign with
# a real identity (pass its SHA-1 hash with --identity) so grants survive
# rebuilds; an ad-hoc signature changes on every build and resets them.
set -euo pipefail

usage() {
    echo "usage: build-app.sh [--identity <sha1-or-name>] [--out <dir>] [--scratch <dir>] [--testing]" >&2
    exit 2
}

here="$(cd "$(dirname "$0")/.." && pwd)"
identity="-"
out="$here/dist"
scratch="$here/.build"
flags=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --identity) identity="${2:-}"; shift 2 ;;
        --out) out="${2:-}"; shift 2 ;;
        --scratch) scratch="${2:-}"; shift 2 ;;
        # Lets KETPLUS_COMPUTER_TEST_APPROVAL answer the approval panel. Never for release.
        --testing) flags=(-Xswiftc -DKETPLUS_COMPUTER_TESTING); shift ;;
        *) usage ;;
    esac
done
[[ -n "$identity" && -n "$out" && -n "$scratch" ]] || usage

swift build --package-path "$here" --scratch-path "$scratch" -c release ${flags[@]+"${flags[@]}"}
bin="$(swift build --package-path "$here" --scratch-path "$scratch" -c release ${flags[@]+"${flags[@]}"} --show-bin-path)"

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
