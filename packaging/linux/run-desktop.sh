#!/usr/bin/env sh
# SPDX-License-Identifier: MPL-2.0
set -eu
prefix=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$prefix/plugins"
export QML_IMPORT_PATH="$prefix/qml"
export QML2_IMPORT_PATH="$prefix/qml"
export QTWEBENGINEPROCESS_PATH="$prefix/libexec/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="$prefix/resources"
export QTWEBENGINE_LOCALES_PATH="$prefix/translations/qtwebengine_locales"
case "${1-}" in
    --daemon) shift; exec "$prefix/bin/ketplus_daemon" "$@" ;;
    --build-info) exec "$prefix/bin/ketplus_daemon" --build-info ;;
esac
exec "$prefix/bin/KetPlus" "$@"
