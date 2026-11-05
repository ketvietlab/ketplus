#!/bin/bash
# Signs a staged KetPlus.app or the app inside a DMG with a Developer ID,
# notarizes and staples the app, then signs, notarizes and staples the DMG so Gatekeeper opens
# both on other Macs without a warning or a long first-launch check.
#
# Signing is inside-out and never uses --deep: every Mach-O file, then nested .xpc/.app bundles,
# then frameworks, then the app. Contents/Resources/ketplus-browser/bin/node gets
# ketplus-browser-node.entitlements (ADR 0009) instead of its upstream signature. Any other nested
# executable already carrying another team's valid Developer ID signature (the Claude Agent SDK
# binary) keeps it.
#
# No credential passes through this script or its arguments: the identity's private key stays in
# the login Keychain and notarytool reads a profile created beforehand with
#   xcrun notarytool store-credentials <profile> --apple-id <id> --team-id <team>
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
usage: sign-notarize-dmg.sh --dmg KetPlus.dmg --identity "Developer ID Application: ..."
                            --notary-profile PROFILE [--output signed.dmg] [--skip-notarize]
       sign-notarize-dmg.sh --app KetPlus.app --identity "Developer ID Application: ..."
                            [--output signed.app] [--notary-profile PROFILE] [--skip-notarize]

  --dmg             Unsigned DMG produced by `cmake --build <dir> --target package`.
  --app             Complete app staged by `cmake --install`; copied before signing.
  --identity        Developer ID Application identity in the login Keychain.
  --notary-profile  notarytool Keychain profile (see store-credentials above).
  --output          Signed output path. Defaults to <input>-signed.dmg or -signed.app.
  --skip-notarize   Sign only; Gatekeeper still rejects the result on other Macs.
EOF
    exit 2
}

dmg=""
source_app=""
identity=""
profile=""
output=""
notarize=1
while [[ $# -gt 0 ]]; do
    case "$1" in
    --dmg) dmg="${2:-}"; shift 2 ;;
    --app) source_app="${2:-}"; shift 2 ;;
    --identity) identity="${2:-}"; shift 2 ;;
    --notary-profile) profile="${2:-}"; shift 2 ;;
    --output) output="${2:-}"; shift 2 ;;
    --skip-notarize) notarize=0; shift ;;
    *) usage ;;
    esac
done
if [[ -n "$source_app" ]]; then
    [[ -d "$source_app" && -z "$dmg" ]] || usage
    [[ -n "$output" ]] || output="${source_app%.app}-signed.app"
else
    [[ -f "$dmg" ]] || usage
    [[ -n "$output" ]] || output="${dmg%.dmg}-signed.dmg"
fi
[[ "$identity" == "Developer ID Application: "* ]] || {
    echo "error: a Developer ID Application identity is required; ad-hoc signing is not a replacement build" >&2
    exit 1
}
[[ $notarize -eq 0 || -n "$profile" ]] || usage
[[ ! -e "$output" ]] || { echo "error: $output already exists" >&2; exit 1; }

script_dir="$(cd "$(dirname "$0")" && pwd)"
node_entitlements="$script_dir/ketplus-browser-node.entitlements"
team="$(sed -n 's/.*(\([A-Z0-9]*\))$/\1/p' <<<"$identity")"
[[ -n "$team" ]] || { echo "error: the identity must include its Team ID" >&2; exit 1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/ketplus-sign.XXXXXX")"
mount="$work/mount"
cleanup() {
    [[ -d "$mount" ]] && hdiutil detach -quiet -force "$mount" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

if [[ -n "$source_app" ]]; then
    echo "==> Copying staged app to $output"
    ditto "$source_app" "$output"
    app="$output"
else
    echo "==> Expanding $dmg"
    hdiutil convert -quiet "$dmg" -format UDRW -o "$work/rw.dmg"
    # Signatures and seals grow the bundle slightly; compression discards the unused space later.
    hdiutil resize -quiet -size 2g "$work/rw.dmg"
    mkdir "$mount"
    hdiutil attach -quiet -nobrowse -noverify -noautoopen -owners on -mountpoint "$mount" "$work/rw.dmg"
    app="$mount/KetPlus.app"
    [[ -d "$app" ]] || { echo "error: KetPlus.app is not at the DMG root" >&2; exit 1; }
fi

# Apple's timestamp service occasionally fails a request; retry before giving up.
sign() {
    local attempt
    for attempt in 1 2 3 4; do
        codesign --force --timestamp --options runtime --sign "$identity" "$@" && return 0
        sleep $((attempt * 5))
    done
    return 1
}

is_macho() {
    case "$(xxd -p -l 4 "$1" 2>/dev/null)" in
    cffaedfe | feedfacf | cefaedfe) return 0 ;;
    cafebabe) file -b "$1" | grep -q Mach-O ;;
    *) return 1 ;;
    esac
}

keeps_foreign_signature() {
    local details
    details="$(codesign -dv --verbose=2 "$1" 2>&1)" || return 1
    grep -q '^Authority=Developer ID Application:' <<<"$details" || return 1
    ! grep -q "^TeamIdentifier=$team\$" <<<"$details" || return 1
    codesign --verify --strict "$1" 2>/dev/null
}

# Deepest paths first, so nested code is sealed before whatever contains it.
by_depth() {
    awk -F/ '{ print NF "\t" $0 }' | sort -rn | cut -f2-
}

echo "==> Signing nested code"
main_exec="$app/Contents/MacOS/$(defaults read "$app/Contents/Info.plist" CFBundleExecutable)"
while IFS= read -r file; do
    [[ "$file" != "$main_exec" ]] && is_macho "$file" || continue
    if [[ "$file" == "$app/Contents/Resources/ketplus-browser/bin/node" ]]; then
        # Upstream node is notarized with far broader entitlements (get-task-allow, dyld
        # environment variables); ADR 0009 allows only JIT.
        sign --entitlements "$node_entitlements" "$file"
    elif keeps_foreign_signature "$file"; then
        echo "    keep  ${file#"$app/"}"
    else
        sign --preserve-metadata=entitlements "$file"
    fi
done < <(find "$app/Contents" -type f | by_depth)

while IFS= read -r bundle; do
    sign --preserve-metadata=entitlements "$bundle"
done < <(find "$app/Contents" -type d \( -name '*.xpc' -o -name '*.app' \) | by_depth)

while IFS= read -r framework; do
    sign "$framework"
done < <(find "$app/Contents" -type d -name '*.framework' | by_depth)

sign "$app"
codesign --verify --deep --strict "$app"
details="$(codesign -dv --verbose=2 "$app" 2>&1)"
grep -q '^Authority=Developer ID Application:' <<<"$details" || {
    echo "error: the app does not have a Developer ID signature" >&2; exit 1;
}
grep -q "^TeamIdentifier=$team\$" <<<"$details" || {
    echo "error: the app's signing Team ID does not match the requested identity" >&2; exit 1;
}
requirement="$(codesign -d -r- "$app" 2>&1)"
if ! grep -q 'designated =>.*anchor apple' <<<"$requirement" ||
    grep -q 'designated =>.*cdhash' <<<"$requirement"; then
    echo "error: a build-specific cdhash cannot identify replacement builds" >&2
    exit 1
fi
codesign -d --entitlements - "$app/Contents/Resources/ketplus-browser/bin/node" 2>/dev/null |
    grep -q allow-jit || { echo "error: bundled node lost its JIT entitlement" >&2; exit 1; }

# Gatekeeper checks the app copied to /Applications, not the DMG it came from. Without its own
# stapled ticket the first launch waits on an online ticket lookup plus a scan of every file in
# the bundle, which on macOS 27 holds the process at _dyld_start long enough to look hung.
notarize_file() {
    local file="$1" result="$work/notary-$(basename "$1").json" status id
    xcrun notarytool submit "$file" --keychain-profile "$profile" --wait --output-format json \
        >"$result"
    status="$(plutil -extract status raw "$result" 2>/dev/null || true)"
    if [[ "$status" != "Accepted" ]]; then
        cat "$result" >&2
        id="$(plutil -extract id raw "$result" 2>/dev/null || true)"
        [[ -z "$id" ]] || xcrun notarytool log "$id" --keychain-profile "$profile" >&2 || true
        echo "error: notarization of $(basename "$file") is ${status:-unknown}" >&2
        exit 1
    fi
}

if [[ $notarize -eq 1 ]]; then
    echo "==> Notarizing the app (this usually takes a few minutes)"
    ditto -c -k --keepParent "$app" "$work/KetPlus.zip"
    notarize_file "$work/KetPlus.zip"
    xcrun stapler staple -q "$app"
    xcrun stapler validate -q "$app"
fi

if [[ -n "$source_app" ]]; then
    echo "==> Done: $output (Developer ID signed; notarized=$notarize)"
    exit 0
fi
hdiutil detach -quiet "$mount"
rmdir "$mount"

echo "==> Writing $output"
hdiutil convert -quiet "$work/rw.dmg" -format UDZO -imagekey zlib-level=9 -o "$output"
codesign --force --timestamp --sign "$identity" "$output"

if [[ $notarize -eq 1 ]]; then
    echo "==> Notarizing the DMG"
    notarize_file "$output"
    xcrun stapler staple -q "$output"
    spctl --assess --type open --context context:primary-signature --verbose "$output"
fi
echo "==> Done: $output"
