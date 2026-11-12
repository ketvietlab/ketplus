#!/bin/sh
# Export the KetPlus Terminal icon from its approved drawing without altering the artwork.
# The ketPlus (Lite) and ketPlus Pro icons come from assets/icons/generate-app-icons.sh.
# macOS tools: AppKit/Swift, sips, iconutil; Python 3 writes PNG-backed ICOs.
# Usage: generate-app-icons.sh [all|terminal]. No install/sign/deploy steps.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
brand="$here/../brand"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat >"$work/rasterize.swift" <<'SWIFT'
import AppKit
let arguments = CommandLine.arguments
guard let image = NSImage(contentsOfFile: arguments[1]) else { fatalError("cannot read the SVG") }
let side = 1024
let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: side, pixelsHigh: side,
                              bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
                              isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0,
                              bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
image.draw(in: NSRect(x: 0, y: 0, width: side, height: side))
NSGraphicsContext.restoreGraphicsState()
try bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: arguments[2]))
SWIFT
export_icon() {
    source_name="$1"
    png_name="$2"
    bundle_name="$3"
    master="$work/$source_name.png"
    swift "$work/rasterize.swift" "$brand/$source_name.svg" "$master"
    cp "$master" "$brand/$png_name.png"
    iconset="$work/$bundle_name.iconset"
    mkdir "$iconset"
    for size in 16 32 128 256 512; do
        sips -z "$size" "$size" "$master" --out "$iconset/icon_${size}x${size}.png" >/dev/null
        double=$((size * 2))
        sips -z "$double" "$double" "$master" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$iconset" -o "$here/$bundle_name.icns"
    for size in 16 32 48 64 128 256; do
        sips -z "$size" "$size" "$master" --out "$work/ico-$size.png" >/dev/null
    done
    python3 - "$work" "$here/$bundle_name.ico" <<'PYICO'
import pathlib, struct, sys
work, output = map(pathlib.Path, sys.argv[1:])
sizes = (16, 32, 48, 64, 128, 256)
images = [(size, (work / f"ico-{size}.png").read_bytes()) for size in sizes]
header = struct.pack("<HHH", 0, 1, len(images))
offset = len(header) + 16 * len(images)
entries = []
for size, data in images:
    entries.append(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
    offset += len(data)
output.write_bytes(header + b"".join(entries) + b"".join(data for _, data in images))
PYICO
}
case "${1:-all}" in
    all|terminal) export_icon ketplus-terminal ketplus-terminal-icon KetPlusTerminal ;;
    *) echo "Usage: $0 [all|terminal]" >&2; exit 2 ;;
esac
