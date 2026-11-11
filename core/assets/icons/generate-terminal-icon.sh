#!/bin/sh
# Compatibility entry point: the shared exporter also creates the Windows ICO.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/generate-app-icons.sh" terminal
