cmake_minimum_required(VERSION 3.24)

# One source tree, two editions. Lite is KétPlus, the editor alone (core/). Pro is KétPlus
# Pro: the editor plus Agent Workspace, Atlas, the daemon and remote sessions. Pro on Linux
# and Windows is not built yet, so those platforms default to Lite.
if(APPLE)
    set(ketplus_default_edition "pro")
else()
    set(ketplus_default_edition "lite")
endif()
set(KETPLUS_EDITION "${ketplus_default_edition}" CACHE STRING
    "Edition to build: lite (KétPlus editor) or pro (KétPlus Pro)")
set_property(CACHE KETPLUS_EDITION PROPERTY STRINGS lite pro)
if(NOT KETPLUS_EDITION MATCHES "^(lite|pro)$")
    message(FATAL_ERROR "KETPLUS_EDITION must be lite or pro; got '${KETPLUS_EDITION}'")
endif()
message(STATUS "KetPlus edition: ${KETPLUS_EDITION}")

if(KETPLUS_EDITION STREQUAL "lite")
    project(KetPlusLite LANGUAGES C CXX)
    option(KETPLUS_BUILD_TESTS "Build the editor tests" ON)
    option(KETPLUS_BUILD_BENCHMARKS "Build the editor benchmarks" OFF)
    # core/ owns the Lite application. FORCE also undoes a Pro configuration of this build tree.
    set(KETPLUS_CM_BUILD_APP ON CACHE BOOL "" FORCE)
    set(KETPLUS_CM_BUILD_TESTS ${KETPLUS_BUILD_TESTS} CACHE BOOL "" FORCE)
    set(KETPLUS_CM_BUILD_BENCHMARKS ${KETPLUS_BUILD_BENCHMARKS} CACHE BOOL "" FORCE)
    if(KETPLUS_BUILD_TESTS)
        enable_testing()
    endif()
    add_subdirectory(core)
    return()
endif()

project(
    KetPlus
    VERSION 0.1.50
    DESCRIPTION "The full KetPlus desktop product"
    LANGUAGES C CXX
)
if(APPLE)
    enable_language(OBJCXX)
endif()

option(KETPLUS_BUILD_TESTS "Build KetPlus proprietary and integration tests" ON)
option(KETPLUS_BUILD_DESKTOP "Build the Desktop UI and public CM integration" ON)
option(KETPLUS_AUTO_UPGRADE_DAEMON "Replace an older idle daemon when an IPC client connects" ON)
option(KETPLUS_BUILD_BENCHMARKS "Build KetPlus performance benchmarks" OFF)
option(KETPLUS_ENABLE_REMOTE_DEMO "Enable Debug-only synthetic remote-control data" OFF)
# The relay (Control API) the daemon talks to: the standalone KétPlus runtime, which serves
# /v2/... at the root of its own host. Only a Debug build with the remote demo reads
# KETPLUS_CONTROL_URL from the environment, so a Release build meant for any other relay must
# name it here. `ketplus_daemon --build-info` and the bundle check report the one compiled in.
set(KETPLUS_CONTROL_URL "https://ketplus-staging.ketviet.vn" CACHE STRING
    "Relay (Control API) base URL compiled into the daemon")
if(NOT KETPLUS_CONTROL_URL MATCHES "^https://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._~-]+)*$" AND
   NOT KETPLUS_CONTROL_URL MATCHES
       "^http://(localhost|127\\.0\\.0\\.1)(:[0-9]+)?(/[A-Za-z0-9._~-]+)*$")
    message(FATAL_ERROR
        "KETPLUS_CONTROL_URL must be https://host[:port][/path], or http:// on loopback; "
        "got '${KETPLUS_CONTROL_URL}'")
endif()
message(STATUS "KetPlus daemon relay: ${KETPLUS_CONTROL_URL}")
if(KETPLUS_BUILD_DESKTOP)
    include(cmake/Sparkle.cmake)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

find_package(Qt6 6.11 REQUIRED COMPONENTS
    Concurrent Core Gui Network Sql WebSockets
)
if(KETPLUS_BUILD_DESKTOP)
    find_package(Qt6 6.11 REQUIRED COMPONENTS
        Core5Compat Qml Quick QuickControls2 Svg SvgWidgets WebView Widgets
    )
endif()
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
include(GNUInstallDirs)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Run a staged installation without the build-host Qt SDK on its search path.
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

include(cmake/Warnings.cmake)
include(cmake/OffscreenTests.cmake)

configure_file(
    cmake/KetPlusProductVersion.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/KetPlusProductVersion.h"
    @ONLY
)
add_library(ketplus_product_version INTERFACE)
target_include_directories(ketplus_product_version INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
)

add_subdirectory(modules/browser)
add_subdirectory(modules/computer)
add_subdirectory(modules/remote)
add_subdirectory(apps/daemon)

if(KETPLUS_BUILD_DESKTOP)
    set(KETPLUS_CM_BUILD_APP OFF CACHE BOOL "" FORCE)
    set(KETPLUS_CM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(KETPLUS_CM_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    add_subdirectory(core EXCLUDE_FROM_ALL)

    # Private chrome typography (KET Sans, then platform fallbacks). Defined before modules/ai
    # so Agent Workspace and the desktop shell resolve one family.
    add_library(ketplus_typography STATIC
        apps/desktop/src/ui/ChromeGeometry.cpp
        apps/desktop/src/ui/ChromeGeometry.h
        apps/desktop/src/ui/InterfaceFont.cpp
        apps/desktop/src/ui/InterfaceFont.h
        apps/desktop/src/ui/LucideIcon.cpp
        apps/desktop/src/ui/LucideIcon.h
    )
    target_link_libraries(ketplus_typography PUBLIC Qt6::Gui Qt6::Svg Qt6::Widgets)
    # ThemeManager's tooltipFontPixels(): the stylesheet keeps tooltips at the kit's size.
    target_link_libraries(ketplus_typography PRIVATE KetPlusCM::ui)
    target_include_directories(ketplus_typography PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/apps/desktop/src>"
    )
    ketplus_enable_warnings(ketplus_typography)
    # KÉT Sans and KÉT Sans Mono from core (core/cmake/KetFonts.cmake); InterfaceFont loads them.
    ketplus_cm_add_ket_fonts(ketplus_typography ketplus_ui_fonts)

    add_subdirectory(modules/ai)
    add_subdirectory(modules/atlas)
    add_subdirectory(apps/desktop)
endif()
add_subdirectory(packaging)

if(KETPLUS_BUILD_BENCHMARKS)
    if(NOT KETPLUS_BUILD_DESKTOP)
        message(FATAL_ERROR "Product benchmarks require KETPLUS_BUILD_DESKTOP=ON")
    endif()
    add_subdirectory(benchmarks)
endif()

if(KETPLUS_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 6.11 REQUIRED COMPONENTS Test)
    if(KETPLUS_BUILD_DESKTOP)
        add_subdirectory(tests)
    else()
        add_subdirectory(tests/daemon)
    endif()
endif()

if(NOT APPLE OR NOT KETPLUS_BUILD_DESKTOP)
    install(FILES LICENSE THIRD_PARTY_NOTICES.md
        DESTINATION share/licenses/ketplus
    )
    install(DIRECTORY third_party/licenses/
        DESTINATION share/licenses/ketplus
    )
endif()
