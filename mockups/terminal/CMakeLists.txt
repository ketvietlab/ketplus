# KetPlus Terminal mock: the standalone terminal restyled with the Két Qt kit (ketvietlab/ket-qt).
# It compiles the real terminal from ../../core/src (PTY, libvterm session, view and tab), so
# it runs a real shell. It is a standalone project and is not part of the Lite or Pro build.
cmake_minimum_required(VERSION 3.24)
project(ketplus_terminal_mock VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg)
qt_standard_project_setup(REQUIRES 6.8)

include(FetchContent)

# The kit, the same pin as mockups/qt. FETCHCONTENT_SOURCE_DIR_KETQT points at a local checkout.
set(KETQT_BUILD_QUICK OFF CACHE BOOL "" FORCE)
set(KETQT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KETQT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ketqt
    GIT_REPOSITORY https://github.com/ketvietlab/ket-qt.git
    GIT_TAG bacf1e52cbf651149a56f64b9e62bd03651c487c # develop, styled tooltips and centred bubble text (PR #20)
)

# libvterm at the commit core/cmake/Dependencies.cmake pins.
FetchContent_Declare(libvterm
    GIT_REPOSITORY https://github.com/neovim/libvterm.git
    GIT_TAG 9d6d2112335080312ef8c36667fa717ded4f7daf
)

# KÉT Sans 1.1 (SIL OFL 1.1), the official release archive, as in mockups/qt.
set(KETPLUS_MOCK_KET_SANS_ARCHIVE "" CACHE FILEPATH "Local copy of KETSans-1.100.zip")
if(KETPLUS_MOCK_KET_SANS_ARCHIVE)
    set(ket_sans_url "${KETPLUS_MOCK_KET_SANS_ARCHIVE}")
else()
    set(ket_sans_url
        "https://github.com/ketvietlab/ket-sans-font/releases/download/v1.1.0/KETSans-1.100.zip")
endif()
FetchContent_Declare(ketplus_mock_ket_sans
    URL "${ket_sans_url}"
    URL_HASH SHA256=cc977aefc90a67bf6ba392b4845deae6750abbbb29a02097d4a848eedd25e34f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake-project
)
FetchContent_MakeAvailable(ketqt libvterm ketplus_mock_ket_sans)

set(core_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../core")

add_library(terminal_mock_vterm STATIC
    "${libvterm_SOURCE_DIR}/src/encoding.c"
    "${libvterm_SOURCE_DIR}/src/keyboard.c"
    "${libvterm_SOURCE_DIR}/src/mouse.c"
    "${libvterm_SOURCE_DIR}/src/parser.c"
    "${libvterm_SOURCE_DIR}/src/pen.c"
    "${libvterm_SOURCE_DIR}/src/screen.c"
    "${libvterm_SOURCE_DIR}/src/state.c"
    "${libvterm_SOURCE_DIR}/src/unicode.c"
    "${libvterm_SOURCE_DIR}/src/vterm.c"
)
target_include_directories(terminal_mock_vterm
    PUBLIC "${libvterm_SOURCE_DIR}/include"
    PRIVATE "${core_dir}/cmake/libvterm_generated" "${libvterm_SOURCE_DIR}/src"
)
set_target_properties(terminal_mock_vterm PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

# The terminal as KetPlus Terminal ships it; only the window around it belongs to the mock.
add_library(terminal_mock_core STATIC
    "${core_dir}/src/terminal/PtyProcess.cpp"
    "${core_dir}/src/terminal/PtyProcess.h"
    "${core_dir}/src/terminal/TerminalSession.cpp"
    "${core_dir}/src/terminal/TerminalSession.h"
    "${core_dir}/src/terminal/TerminalView.cpp"
    "${core_dir}/src/terminal/TerminalView.h"
    "${core_dir}/src/terminal_app/TerminalTab.cpp"
    "${core_dir}/src/terminal_app/TerminalTab.h"
)
target_include_directories(terminal_mock_core PUBLIC "${core_dir}/src")
target_link_libraries(terminal_mock_core PUBLIC Qt6::Widgets terminal_mock_vterm)
if(UNIX AND NOT APPLE)
    target_link_libraries(terminal_mock_core PRIVATE util)
endif()

qt_add_executable(ketplus_terminal_mock MACOSX_BUNDLE src/main.cpp src/TerminalMockWindow.cpp
                  src/TerminalMockWindow.h)
set_target_properties(ketplus_terminal_mock PROPERTIES OUTPUT_NAME "KetPlusTerminalMock")

qt_add_resources(ketplus_terminal_mock terminal_mock_fonts
    PREFIX "/fonts"
    BASE "${ketplus_mock_ket_sans_SOURCE_DIR}/TTF"
    FILES
        "${ketplus_mock_ket_sans_SOURCE_DIR}/TTF/KETSans-Regular.ttf"
        "${ketplus_mock_ket_sans_SOURCE_DIR}/TTF/KETSans-Medium.ttf"
)
# KÉT Sans Mono 0.10.4 Medium (SIL OFL 1.1), committed once under mockups/qt/fonts.
qt_add_resources(ketplus_terminal_mock terminal_mock_mono
    PREFIX "/fonts"
    BASE ../qt/fonts
    FILES ../qt/fonts/KETSansMono-Medium.ttf
)
qt_add_resources(ketplus_terminal_mock terminal_mock_assets
    PREFIX "/"
    BASE "${core_dir}/assets/brand"
    FILES "${core_dir}/assets/brand/ketplus-terminal-icon.png"
)
qt_add_resources(ketplus_terminal_mock terminal_mock_demo
    PREFIX "/"
    FILES demo/demo.sh
)

target_compile_definitions(ketplus_terminal_mock PRIVATE PROJECT_VERSION="${PROJECT_VERSION}")
target_link_libraries(ketplus_terminal_mock PRIVATE terminal_mock_core KetQt::Widgets Qt6::Svg)
