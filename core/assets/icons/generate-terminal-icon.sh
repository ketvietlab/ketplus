#!/bin/sh
# Regenerates the KétPlus Terminal app icon from its Két Symbol source drawing
# (core/assets/brand/ketplus-terminal.svg, CC BY 4.0). macOS only: it uses qlmanage,
# sips and iconutil, which ship with the OS. Commit the two outputs.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
brand="$here/../brand"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

qlmanage -t -s 1024 -o "$work" "$brand/ketplus-terminal.svg" >/dev/null
master="$work/ketplus-terminal.svg.png"
cp "$master" "$brand/ketplus-terminal-icon.png"

iconset="$work/KetPlusTerminal.iconset"
mkdir "$iconset"
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$master" --out "$iconset/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z "$double" "$double" "$master" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$here/KetPlusTerminal.icns"
