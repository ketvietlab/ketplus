#!/bin/bash
# ADR 0019: prepares a Sparkle update for an installed (signed, notarized) KetPlus.app.
#
# It archives the app, signs the archive with the EdDSA private key, and regenerates the appcast
# in an updates directory. It never uploads anything: publishing the directory to the host behind
# KETPLUS_UPDATE_FEED_URL is a separate, explicitly approved release step.
#
# The private key stays outside this repository. By default Sparkle's generate_appcast reads it
# from the login Keychain (created by `generate_keys`); --ed-key-file names an exported key file
# kept elsewhere, for example on CI from a secret mounted at run time.
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
usage: publish-update.sh --app KetPlus.app --updates-dir DIR --download-url-prefix URL
                         --sparkle-bin DIR [--release-notes FILE] [--ed-key-file FILE]

  --app                  Installed, signed KetPlus.app (cmake --install output).
  --updates-dir          Directory holding every published archive and appcast.xml.
  --download-url-prefix  HTTPS URL the archives are served from (ends with /).
  --sparkle-bin          Sparkle's bin directory (build/<preset>/_deps/ketplus_sparkle-src/bin).
  --release-notes        Optional HTML or Markdown release notes for this version.
  --ed-key-file          Optional exported EdDSA private key outside the repository, or - for
                         standard input. Without it the login Keychain key is used.
EOF
    exit 2
}

app=""
updates_dir=""
download_prefix=""
sparkle_bin=""
release_notes=""
key_file=""
while [[ $# -gt 0 ]]; do
    case "$1" in
    --app) app="${2:-}"; shift 2 ;;
    --updates-dir) updates_dir="${2:-}"; shift 2 ;;
    --download-url-prefix) download_prefix="${2:-}"; shift 2 ;;
    --sparkle-bin) sparkle_bin="${2:-}"; shift 2 ;;
    --release-notes) release_notes="${2:-}"; shift 2 ;;
    --ed-key-file) key_file="${2:-}"; shift 2 ;;
    *) usage ;;
    esac
done
[[ -n "$app" && -n "$updates_dir" && -n "$download_prefix" && -n "$sparkle_bin" ]] || usage

fail() {
    echo "publish-update: $*" >&2
    exit 1
}

[[ -d "$app/Contents" ]] || fail "not an application bundle: $app"
[[ -x "$sparkle_bin/generate_appcast" ]] || fail "generate_appcast not found in $sparkle_bin"
[[ "$download_prefix" == https://*/ ]] || fail "--download-url-prefix must be https:// and end with /"

plist="$app/Contents/Info.plist"
version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist")"
[[ "$version" =~ ^[0-9][A-Za-z0-9.+_-]*$ ]] ||
    fail "CFBundleShortVersionString must be a nonempty filename-safe version starting with a digit"
feed="$(/usr/libexec/PlistBuddy -c 'Print :SUFeedURL' "$plist" 2>/dev/null)" ||
    fail "$app was built without KETPLUS_UPDATE_FEED_URL; it could never install updates"
[[ -d "$app/Contents/Frameworks/Sparkle.framework" ]] || fail "$app does not embed Sparkle"

repo_root="$(cd "$(dirname "$0")/../.." && pwd -P)"
# "-" reads the key from standard input, which keeps it off the file system entirely.
if [[ -n "$key_file" && "$key_file" != "-" ]]; then
    [[ -f "$key_file" ]] || fail "key file not found: $key_file"
    key_real="$(cd "$(dirname "$key_file")" && pwd -P)/$(basename "$key_file")"
    [[ "$key_real" != "$repo_root"/* ]] || fail "the EdDSA private key must not live in the repository"
fi

/usr/bin/codesign --verify --deep --strict "$app" || fail "$app has an invalid code signature"
if ! /usr/bin/codesign -dv "$app" 2>&1 | grep -q '^Authority=Developer ID Application'; then
    echo "publish-update: warning: $app is not Developer ID signed; Gatekeeper will reject it" \
        "for users. Use this archive for testing only." >&2
fi

archive="$updates_dir/KetPlus-$version.zip"
[[ ! -e "$archive" && ! -L "$archive" ]] ||
    fail "$archive already exists; every release needs a new version"
notes_destination=""
if [[ -n "$release_notes" ]]; then
    [[ -f "$release_notes" && -r "$release_notes" ]] ||
        fail "release notes must be a readable file: $release_notes"
    case "$release_notes" in
    *.html) notes_destination="$updates_dir/KetPlus-$version.html" ;;
    *.md) notes_destination="$updates_dir/KetPlus-$version.md" ;;
    *) fail "release notes must be .html or .md" ;;
    esac
    [[ ! -e "$notes_destination" && ! -L "$notes_destination" ]] ||
        fail "$notes_destination already exists; refusing to overwrite release notes"
fi

# A run that fails before the new appcast.xml is in place removes what it created, so the same
# version can be prepared again, and leaves the published appcast.xml as it was. Once the new
# appcast is installed, the archive and notes it references stay even if a later step fails.
created=()
appcast_staging=""
remove_partial_release() {
    local status=$?
    if [[ $status -ne 0 ]]; then
        local path
        for path in ${created[@]+"${created[@]}"}; do
            rm -f -- "$path"
        done
    fi
    [[ -z "$appcast_staging" ]] || rm -rf -- "$appcast_staging"
    return $status
}
trap remove_partial_release EXIT

# Validate notes and destination collisions before creating any release artifact.
mkdir -p "$updates_dir"
created+=("$archive")
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$app" "$archive"
if [[ -n "$notes_destination" ]]; then
    created+=("$notes_destination")
    cp "$release_notes" "$notes_destination"
fi

key_arguments=()
if [[ -n "$key_file" ]]; then
    key_arguments=(--ed-key-file "$key_file")
fi
# generate_appcast reads an existing -o file as the feed to update, so the staged copy starts as
# the published feed (with its mode), or does not exist for a first release. The staging
# directory sits next to the updates directory, outside the archives Sparkle scans, and on the
# same filesystem, so installing the new feed is a rename.
updates_parent="$(cd "$updates_dir/.." && pwd -P)"
appcast_staging="$(mktemp -d "$updates_parent/.ketplus-appcast.XXXXXX")"
if [[ -e "$updates_dir/appcast.xml" ]]; then
    cp -p -- "$updates_dir/appcast.xml" "$appcast_staging/appcast.xml"
fi
"$sparkle_bin/generate_appcast" "${key_arguments[@]+"${key_arguments[@]}"}" \
    --download-url-prefix "$download_prefix" \
    --embed-release-notes \
    -o "$appcast_staging/appcast.xml" \
    "$updates_dir"
mv -f -- "$appcast_staging/appcast.xml" "$updates_dir/appcast.xml"
created=()

echo "Prepared KetPlus $version in $updates_dir"
echo "The installed app reads $feed; upload appcast.xml and the archive only after approval."
