# SPDX-License-Identifier: MPL-2.0
option(KETPLUS_LINUX_DESKTOP_PACKAGE "Stage the complete Linux Pro desktop runtime" OFF)
if(NOT KETPLUS_LINUX_DESKTOP_PACKAGE)
    return()
endif()
if(NOT KETPLUS_PRODUCTION_RELEASE OR KETPLUS_ENABLE_REMOTE_DEMO)
    message(FATAL_ERROR "Linux desktop packages require production control with remote demo disabled")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
include("${PROJECT_SOURCE_DIR}/packaging/linux/RuntimePlugins.cmake")
find_program(ketplus_linux_node NAMES node REQUIRED)
set(ketplus_linux_browser_app "${PROJECT_SOURCE_DIR}/apps/browser")
set(ketplus_linux_browser_stage "${CMAKE_CURRENT_BINARY_DIR}/linux-browser-runtime")
file(GLOB_RECURSE ketplus_linux_browser_sources CONFIGURE_DEPENDS
    "${ketplus_linux_browser_app}/src/*"
    "${ketplus_linux_browser_app}/scripts/*"
    "${ketplus_linux_browser_app}/.claude/skills/ketplus-browser/*")
add_custom_command(OUTPUT "${ketplus_linux_browser_stage}/runtime.json"
    COMMAND "${ketplus_linux_node}" "${ketplus_linux_browser_app}/scripts/package-runtime.mjs"
        --out "${ketplus_linux_browser_stage}"
    DEPENDS ${ketplus_linux_browser_sources}
        "${ketplus_linux_browser_app}/package.json"
        "${ketplus_linux_browser_app}/package-lock.json"
        "${ketplus_linux_browser_app}/runtime/node-runtime.json"
    VERBATIM)
add_custom_target(ketplus_bundle_browser_runtime ALL
    DEPENDS "${ketplus_linux_browser_stage}/runtime.json")
add_dependencies(ketplus ketplus_bundle_browser_runtime)
install(CODE "
include(\"${PROJECT_SOURCE_DIR}/cmake/KetPlusBrowserRuntime.cmake\")
ketplus_browser_check_runtime(\"${ketplus_linux_browser_app}\" \"${ketplus_linux_browser_stage}\" FALSE)
" COMPONENT Runtime)
install(DIRECTORY "${ketplus_linux_browser_stage}/"
    DESTINATION lib/ketplus/ketplus-browser USE_SOURCE_PERMISSIONS COMPONENT Runtime)
install(PROGRAMS "${PROJECT_SOURCE_DIR}/packaging/linux/run-desktop.sh"
    DESTINATION . RENAME ketplus-pro COMPONENT Runtime)

# Agent/Atlas QML lives in resource collections, rather than an executable-owned module.
file(GLOB ketplus_linux_qml CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/modules/ai/src/agent/qml/*.qml")
list(APPEND ketplus_linux_qml "${PROJECT_SOURCE_DIR}/packaging/linux/RuntimeImports.qml")
set_property(TARGET ketplus PROPERTY QT_QML_MODULE_QML_FILES "${ketplus_linux_qml}")
set_property(TARGET ketplus PROPERTY QT_QML_IMPORT_SCANNER_EXTRA_ROOT_PATHS
    "${PROJECT_SOURCE_DIR}/modules/ai/src/agent/qml")

get_filename_component(ketplus_linux_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
if(NOT EXISTS "${ketplus_linux_qt_prefix}/libexec/QtWebEngineProcess" OR
   NOT EXISTS "${ketplus_linux_qt_prefix}/resources/qtwebengine_resources.pak")
    message(FATAL_ERROR "The Qt SDK must include the Linux Qt WebEngine runtime")
endif()
install(PROGRAMS "${ketplus_linux_qt_prefix}/libexec/QtWebEngineProcess"
    DESTINATION libexec COMPONENT Runtime)
install(DIRECTORY "${ketplus_linux_qt_prefix}/resources/"
    DESTINATION resources COMPONENT Runtime)
install(DIRECTORY "${ketplus_linux_qt_prefix}/translations/qtwebengine_locales"
    DESTINATION translations COMPONENT Runtime)
qt6_generate_deploy_script(TARGET ketplus NAME ketplus_linux_desktop
    OUTPUT_SCRIPT ketplus_linux_deploy_script CONTENT "
qt6_deploy_qml_imports(TARGET ketplus PLUGINS_FOUND plugins_found)
qt6_deploy_runtime_dependencies(
    EXECUTABLE \"$<TARGET_FILE:ketplus>\"
    ADDITIONAL_EXECUTABLES \"${ketplus_linux_qt_prefix}/libexec/QtWebEngineProcess\"
        \"$<TARGET_FILE:ketplus_daemon>\" \"$<TARGET_FILE:ketplusctl>\"
    ADDITIONAL_MODULES \${plugins_found}
    INCLUDE_PLUGIN_TYPES platforms webview imageformats iconengines sqldrivers tls
    INCLUDE_PLUGINS qoffscreen qxcb qsqlite
    EXCLUDE_PLUGINS ${ketplus_linux_excluded_runtime_plugins}
    GENERATE_QT_CONF NO_TRANSLATIONS)
file(WRITE \"\${QT_DEPLOY_PREFIX}/bin/qt.conf\"
    \"[Paths]\\nPrefix=..\\nLibraries=lib\\nLibraryExecutables=libexec\\nPlugins=plugins\\nQmlImports=qml\\nData=.\\nTranslations=translations\\n\")
file(COPY_FILE \"\${QT_DEPLOY_PREFIX}/bin/qt.conf\"
    \"\${QT_DEPLOY_PREFIX}/libexec/qt.conf\")
")
install(SCRIPT "${ketplus_linux_deploy_script}" COMPONENT Runtime)

set(KETPLUS_LINUX_PACKAGE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/linux-packages" CACHE PATH
    "New directory in which to create the Linux engineering artifacts")
add_custom_target(ketplus_linux_packages
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/packaging/linux/build-packages.py"
        --build-dir "${CMAKE_BINARY_DIR}" --out "${KETPLUS_LINUX_PACKAGE_OUTPUT_DIR}"
        --version "${PROJECT_VERSION}" --architecture "${CMAKE_SYSTEM_PROCESSOR}"
        --expected-control-url "${KETPLUS_CONTROL_URL}" --qt-version "${Qt6_VERSION}"
    DEPENDS ketplus ketplus_daemon ketplusctl ketplus_bundle_browser_runtime
    USES_TERMINAL VERBATIM)
