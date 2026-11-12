#!/bin/sh
# Export the approved ketPlus brand rasters for both editions from the 1024 px mark
# (assets/brand/ketplus-mark.png) and KÉT Sans Bold. Writes, per edition:
#   macOS .icns (16–512 @1x/@2x), a PNG-backed Windows .ico, the 1024 px window icon PNG and
#   the Linux hicolor set (16–512): core/assets/linux/hicolor (Lite), packaging/linux/hicolor
#   (Pro). Jay's decision: both editions share one icon; "Pro" appears only in the lockup.
# Pro also gets the DMG lockup (packaging/macos/dmg-lockup.png).
# macOS tools: swiftc (AppKit/CoreText), sips, iconutil; Python 3 writes the ICOs.
# KET_SANS_BOLD=/path/KETSans-Bold.ttf skips the download of the pinned KÉT Sans release.
# Terminal keeps its own icon: core/assets/icons/generate-terminal-icon.sh.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mark="$root/assets/brand/ketplus-mark.png"

if [ -z "${KET_SANS_BOLD:-}" ]; then
    # The same pinned archive as core/cmake/KetFonts.cmake.
    curl -fsSL -o "$work/ket-sans.zip" \
        https://github.com/ketvietlab/ket-sans-font/releases/download/v1.1.0/KETSans-1.100.zip
    echo "cc977aefc90a67bf6ba392b4845deae6750abbbb29a02097d4a848eedd25e34f  $work/ket-sans.zip" |
        shasum -a 256 -c - >/dev/null
    unzip -q "$work/ket-sans.zip" -d "$work/ket-sans"
    KET_SANS_BOLD=$(find "$work/ket-sans" -name KETSans-Bold.ttf | head -n 1)
fi
swiftc -O -o "$work/render" "$root/assets/icons/render-brand.swift"
render() { "$work/render" "$@"; }

# The mark derivatives the QML lockup loads, so it never scales 1024 px down at runtime.
# 256 px is the widget lockup's source (core's copy serves the Lite app).
for size in 64 128 256; do
    sips -z "$size" "$size" "$mark" --out "$root/assets/brand/ketplus-mark-$size.png" >/dev/null
done
cp "$root/assets/brand/ketplus-mark-256.png" "$root/core/assets/brand/ketplus-mark-256.png"

export_edition() {
    edition="$1"   # lite | pro
    brand_dir="$2" # where ketplus-app-icon.png goes
    icon_dir="$3"  # where KetPlus.icns / KetPlus.ico go
    icon_name="$4" # Linux icon name (the .desktop Icon= value)
    hicolor="$5"   # Linux hicolor theme directory
    out="$work/$edition"
    mkdir -p "$out"
    for size in 16 32 48 64 128 256 512 1024; do
        render icon "$edition" "$mark" "$KET_SANS_BOLD" "$size" "$out/$size.png"
    done
    cp "$out/1024.png" "$brand_dir/ketplus-app-icon.png"
    iconset="$out/KetPlus.iconset"
    mkdir "$iconset"
    for size in 16 32 128 256 512; do
        cp "$out/$size.png" "$iconset/icon_${size}x${size}.png"
        cp "$out/$((size * 2)).png" "$iconset/icon_${size}x${size}@2x.png"
    done
    iconutil -c icns "$iconset" -o "$icon_dir/KetPlus.icns"
    python3 -I - "$out" "$icon_dir/KetPlus.ico" <<'PYICO'
import pathlib, struct, sys
work, output = map(pathlib.Path, sys.argv[1:])
sizes = (16, 32, 48, 64, 128, 256)
images = [(size, (work / f"{size}.png").read_bytes()) for size in sizes]
header = struct.pack("<HHH", 0, 1, len(images))
offset = len(header) + 16 * len(images)
entries = []
for size, data in images:
    entries.append(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
    offset += len(data)
output.write_bytes(header + b"".join(entries) + b"".join(data for _, data in images))
PYICO
    rm -rf "$hicolor"
    for size in 16 32 48 64 128 256 512; do
        mkdir -p "$hicolor/${size}x${size}/apps"
        cp "$out/$size.png" "$hicolor/${size}x${size}/apps/$icon_name.png"
    done
}

# core/ builds without the Pro directories, so Lite's Linux files live inside it.
export_edition lite "$root/core/assets/brand" "$root/core/assets/icons" ketplus \
    "$root/core/assets/linux/hicolor"
export_edition pro "$root/assets/brand" "$root/assets/icons" ketplus-pro \
    "$root/packaging/linux/hicolor"
# README lockups, @2x of a 48 px wordmark, one per GitHub colour scheme.
for theme in light dark; do
    render lockup lite "$mark" "$KET_SANS_BOLD" 96 "$theme" \
        "$root/core/assets/brand/ketplus-lockup-$theme.png"
    render lockup pro "$mark" "$KET_SANS_BOLD" 96 "$theme" \
        "$root/assets/brand/ketplus-pro-lockup-$theme.png"
done
# The DMG installs Pro; its background is dark, so the lockup uses the dark-theme colours.
# 2× the sidebar's 17 px wordmark, drawn at half size on the 660 × 420 pt background.
render lockup pro "$mark" "$KET_SANS_BOLD" 34 dark "$root/packaging/macos/dmg-lockup.png"
