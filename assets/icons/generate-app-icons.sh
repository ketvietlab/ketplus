#!/bin/sh
# Export the approved app assets for both editions and standalone Terminal.
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
"$root/core/assets/icons/generate-app-icons.sh" all
cp "$root/core/assets/brand/ketplus.svg" "$root/assets/brand/ketplus.svg"
cp "$root/core/assets/brand/ketplus-app-icon.png" "$root/assets/brand/ketplus-app-icon.png"
for extension in icns ico; do
    cp "$root/core/assets/icons/KetPlus.$extension" "$root/assets/icons/KetPlus.$extension"
done
