# KetPlus Qt mock: the interactive design reference for KétPlus Pro, built with the Két Qt kit
# (ketvietlab/ket-qt). It replaces ketplus.ketatlas (ADR 0033). It is a standalone project and
# is not part of the Lite or Pro build.
cmake_minimum_required(VERSION 3.24)
project(ketplus_qt_mock VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Svg Test Widgets)
qt_standard_project_setup(REQUIRES 6.8)

include(FetchContent)

# The kit, pinned to a merged develop commit. FETCHCONTENT_SOURCE_DIR_KETQT points at a
# local checkout instead. KetQt::Widgets is only for ket::printImage(), the image viewer's
# Print: the system print dialog is a widget, as in the app.
set(KETQT_BUILD_WIDGETS ON CACHE BOOL "" FORCE)
FetchContent_Declare(ketqt
    GIT_REPOSITORY https://github.com/ketvietlab/ket-qt.git
    GIT_TAG 5f310ce5a25677768c5e33646f68c7cfa45f5523 # develop, through PR #27
)
FetchContent_MakeAvailable(ketqt)

# KÉT Sans 1.1 (SIL OFL 1.1), the official release archive. KÉT Sans Mono 0.10.4 (SIL OFL
# 1.1) has no public release, so its Medium TTF is committed under fonts/ with its licence.
# Only Medium is bundled: every monospace text in the mock (numbers, code, diffs) is Medium.
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
FetchContent_MakeAvailable(ketplus_mock_ket_sans)

qt_add_executable(ketplus_qt_mock MACOSX_BUNDLE src/main.cpp src/MockController.h
                  src/MockController.cpp)
set_target_properties(ketplus_qt_mock PROPERTIES OUTPUT_NAME "KetPlusMock")

file(GLOB_RECURSE mock_qml RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS qml/*.qml)
set_source_files_properties(qml/Fixtures.qml qml/TranscriptFixtures.qml PROPERTIES
                            QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(ketplus_qt_mock
    URI KetPlusMock
    VERSION 1.0
    QML_FILES ${mock_qml}
    RESOURCES assets/ketplus-mark.svg assets/ketplus-mark-bare-dark.svg
              assets/ketplus-mark-bare-light.svg assets/screenshot-sidebar.png assets/screenshot-editor.png
              assets/screenshot-settings.png
              assets/mermaid/shared-clock-light@2x.png assets/mermaid/shared-clock-dark@2x.png
              assets/mermaid/agent-turn-light@2x.png assets/mermaid/agent-turn-dark@2x.png
              assets/connections/desktop-question-light.png assets/connections/desktop-question-dark.png
              assets/connections/mobile-question-light.png assets/connections/mobile-question-dark.png
)

qt_add_resources(ketplus_qt_mock mock_fonts
    PREFIX "/fonts"
    BASE "${ketplus_mock_ket_sans_SOURCE_DIR}/TTF"
    FILES
        "${ketplus_mock_ket_sans_SOURCE_DIR}/TTF/KETSans-Light.ttf"
        "${ketplus_mock_ket_sans_SOURCE_DIR}/TTF/KETSans-Regular.ttf"
        "${ketplus_mock_ket_sans_SOURCE_DIR}/TTF/KETSans-Medium.ttf"
        "${ketplus_mock_ket_sans_SOURCE_DIR}/TTF/KETSans-Bold.ttf"
)
qt_add_resources(ketplus_qt_mock mock_mono
    PREFIX "/fonts"
    BASE fonts
    FILES fonts/KETSansMono-Medium.ttf
)

# The Mermaid samples' sources, shown by the fixtures and rendered by tools/render_mermaid.py.
qt_add_resources(ketplus_qt_mock mock_mermaid
    PREFIX "/mermaid"
    BASE mermaid
    FILES mermaid/shared-clock.mmd mermaid/agent-turn.mmd
)

# The repository's licensing document, the long Markdown sample the editor previews.
qt_add_resources(ketplus_qt_mock mock_docs
    PREFIX "/docs"
    BASE ../../docs
    FILES ../../docs/licensing.md
)

target_link_libraries(ketplus_qt_mock PRIVATE KetQt::Quick KetQt::QuickPlugin KetQt::Widgets Qt6::Gui
                                              Qt6::Qml Qt6::Test Qt6::Widgets
                                              Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
