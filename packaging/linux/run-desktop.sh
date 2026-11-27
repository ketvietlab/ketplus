#!/usr/bin/env sh
# SPDX-License-Identifier: MPL-2.0
set -eu
prefix=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
# Deployment supplies relative ELF RUNPATHs and qt.conf. Never export bundled
# loader/Qt paths: terminals, providers and Git inherit the user's environment.
case "${1-}" in
    --daemon) shift; exec "$prefix/bin/ketplus_daemon" "$@" ;;
    --build-info) exec "$prefix/bin/ketplus_daemon" --build-info ;;
esac
exec "$prefix/bin/KetPlus" "$@"
