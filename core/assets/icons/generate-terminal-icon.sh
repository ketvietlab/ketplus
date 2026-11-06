#!/bin/sh
# Regenerates the KétPlus Terminal app icon from its Két Symbol source drawing
# (core/assets/brand/ketplus-terminal.svg, CC BY 4.0). macOS only: AppKit (through
# swift, from the Xcode command line tools) draws the SVG with its transparent corners,
# then sips and iconutil make the iconset. Commit the two outputs. Do not use qlmanage:
# its thumbnails have an opaque white background.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
brand="$here/../brand"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

master="$work/ketplus-terminal.png"
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
swift "$work/rasterize.swift" "$brand/ketplus-terminal.svg" "$master"
cp "$master" "$brand/ketplus-terminal-icon.png"

iconset="$work/KetPlusTerminal.iconset"
mkdir "$iconset"
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$master" --out "$iconset/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z "$double" "$double" "$master" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$here/KetPlusTerminal.icns"
