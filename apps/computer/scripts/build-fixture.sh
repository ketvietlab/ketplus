#!/usr/bin/env bash
# Builds the computer-use fixture app (dev.ketplus.computer.fixture) and, next to it,
# fixture-cover: a bundle-less occluder for the background checks in e2e.mjs.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$here/dist}"
app="$out/KetPlus Computer Fixture.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
swiftc -O "$here/scripts/fixture-target.swift" -o "$app/Contents/MacOS/KetPlusComputerFixture"
/usr/libexec/PlistBuddy -c "Add :CFBundleIdentifier string dev.ketplus.computer.fixture" \
    -c "Add :CFBundleName string KetPlus Computer Fixture" \
    -c "Add :CFBundleExecutable string KetPlusComputerFixture" \
    -c "Add :CFBundlePackageType string APPL" "$app/Contents/Info.plist" >/dev/null
codesign --force --sign - "$app"
swiftc -O "$here/scripts/fixture-cover.swift" -o "$out/fixture-cover"
echo "$app"
