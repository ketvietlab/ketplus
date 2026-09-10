cmake_minimum_required(VERSION 3.24)

project(
    KetPlusCM
    VERSION 0.1.2
    DESCRIPTION "A fast, lightweight, cross-platform text editor"
    LANGUAGES C CXX
)

option(KETPLUS_CM_BUILD_APP "Build the KetPlus CM desktop application" ON)
option(KETPLUS_CM_BUILD_TESTS "Build KetPlus CM tests" ON)
option(KETPLUS_CM_BUILD_BENCHMARKS "Build KetPlus CM performance benchmarks" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Core5Compat Svg SvgWidgets)

include(cmake/CompilerCache.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Warnings.cmake)

set_property(GLOBAL PROPERTY
    KETPLUS_CM_INTER_FONT_SOURCE_DIR "${inter_font_SOURCE_DIR}"
)

function(ketplus_cm_add_font_resources target)
    get_property(font_source_dir GLOBAL PROPERTY KETPLUS_CM_INTER_FONT_SOURCE_DIR)
    qt_add_resources(${target} "ketplus_fonts"
        PREFIX "/fonts"
        BASE "${font_source_dir}"
        FILES "${font_source_dir}/InterVariable.ttf"
    )
    qt_add_resources(${target} "ketplus_font_licenses"
        PREFIX "/licenses/inter"
        BASE "${font_source_dir}"
        FILES "${font_source_dir}/LICENSE.txt"
    )
endfunction()

add_library(ketplus_ui STATIC
    src/ui/FindReplaceBar.cpp
    src/ui/FindReplaceBar.h
    src/ui/Theme.cpp
    src/ui/Theme.h
)
target_link_libraries(ketplus_ui PUBLIC Qt6::Widgets)
target_include_directories(ketplus_ui PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_ui)
add_library(KetPlusCM::ui ALIAS ketplus_ui)

add_library(ketplus_core STATIC
    src/core/Document.cpp
    src/core/Document.h
)
target_link_libraries(ketplus_core PUBLIC Qt6::Core)
target_include_directories(ketplus_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_core)
add_library(KetPlusCM::core ALIAS ketplus_core)

add_library(ketplus_git STATIC
    src/git/GitParser.cpp
    src/git/GitParser.h
    src/git/GitService.cpp
    src/git/GitService.h
    src/git/GitTypes.h
)
target_link_libraries(ketplus_git PUBLIC Qt6::Core)
target_include_directories(ketplus_git PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_git)
add_library(KetPlusCM::git ALIAS ketplus_git)

add_library(ketplus_workspace STATIC
    src/workspace/ExplorerPanel.cpp
    src/workspace/ExplorerPanel.h
)
target_link_libraries(ketplus_workspace PUBLIC Qt6::Widgets)
target_include_directories(ketplus_workspace PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_workspace)
add_library(KetPlusCM::workspace ALIAS ketplus_workspace)

add_library(ketplus_terminal STATIC
    src/terminal/PtyProcess.cpp
    src/terminal/PtyProcess.h
    src/terminal/TerminalPanel.cpp
    src/terminal/TerminalPanel.h
    src/terminal/TerminalView.cpp
    src/terminal/TerminalView.h
)
target_link_libraries(ketplus_terminal PUBLIC Qt6::Widgets ketplus_vterm)
if(UNIX AND NOT APPLE)
    target_link_libraries(ketplus_terminal PRIVATE util)
endif()
target_include_directories(ketplus_terminal PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_terminal)
add_library(KetPlusCM::terminal ALIAS ketplus_terminal)

add_library(ketplus_preview STATIC
    src/preview/MarkdownPreviewPane.cpp
    src/preview/MarkdownPreviewPane.h
    src/preview/MermaidDiagramDialog.cpp
    src/preview/MermaidDiagramDialog.h
    src/preview/MermaidRenderer.cpp
    src/preview/MermaidRenderer.h
)
target_link_libraries(ketplus_preview
    PUBLIC Qt6::Widgets Qt6::Svg Qt6::SvgWidgets ketplus_ui
)
target_include_directories(ketplus_preview PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_preview)
add_library(KetPlusCM::preview ALIAS ketplus_preview)

add_library(ketplus_editor STATIC
    src/editor/EditorSettings.cpp
    src/editor/EditorSettings.h
    src/editor/EditorWidget.cpp
    src/editor/EditorWidget.h
    src/editor/SyntaxDefinition.cpp
    src/editor/SyntaxDefinition.h
)
target_link_libraries(ketplus_editor
    PUBLIC
        Qt6::Widgets
        ketplus_core
        ketplus_scintilla
        ketplus_ui
    PRIVATE
        ketplus_lexilla
)
target_include_directories(ketplus_editor PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_editor)
add_library(KetPlusCM::editor ALIAS ketplus_editor)

add_library(ketplus_git_ui STATIC
    src/git/GitChangesPanel.cpp
    src/git/GitChangesPanel.h
    src/git/GitDiffView.cpp
    src/git/GitDiffView.h
)
target_link_libraries(ketplus_git_ui
    PUBLIC Qt6::Widgets ketplus_editor ketplus_git ketplus_ui
)
target_include_directories(ketplus_git_ui PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_git_ui)
add_library(KetPlusCM::git_ui ALIAS ketplus_git_ui)

add_library(ketplus_settings_ui STATIC
    src/app/SettingsDialog.cpp
    src/app/SettingsDialog.h
)
target_link_libraries(ketplus_settings_ui PUBLIC Qt6::Widgets ketplus_editor ketplus_ui)
target_include_directories(ketplus_settings_ui PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_settings_ui)
add_library(KetPlusCM::settings_ui ALIAS ketplus_settings_ui)

if(KETPLUS_CM_BUILD_APP)
    set(ketplus_platform_sources)
    if(APPLE)
        set(ketplus_macos_icon "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/KetPlus.icns")
        set_source_files_properties(
            "${ketplus_macos_icon}"
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
        )
        list(APPEND ketplus_platform_sources "${ketplus_macos_icon}")
    elseif(WIN32)
        list(APPEND ketplus_platform_sources assets/icons/KetPlus.rc)
    endif()

    qt_add_executable(ketplus_cm WIN32 MACOSX_BUNDLE
        src/app/MainWindow.cpp
        src/app/MainWindowActions.cpp
        src/app/MainWindowDocuments.cpp
        src/app/MainWindowGit.cpp
        src/app/MainWindow.h
        src/app/MainWindowPanels.cpp
        src/main.cpp
        ${ketplus_platform_sources}
    )
    target_link_libraries(ketplus_cm
        PRIVATE
            Qt6::Widgets
            KetPlusCM::core
            KetPlusCM::editor
            KetPlusCM::git
            KetPlusCM::git_ui
            KetPlusCM::preview
            KetPlusCM::settings_ui
            KetPlusCM::terminal
            KetPlusCM::ui
            KetPlusCM::workspace
    )
    target_compile_definitions(ketplus_cm PRIVATE
        KETPLUS_CM_VERSION="${PROJECT_VERSION}"
    )
    ketplus_enable_warnings(ketplus_cm)
    ketplus_cm_add_font_resources(ketplus_cm)

    qt_add_resources(ketplus_cm "ketplus_brand"
        PREFIX "/brand"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand"
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/ketplus-app-icon.png"
    )

    if(APPLE)
        set(ketplus_output_name "KetPlus CM")
    elseif(WIN32)
        set(ketplus_output_name "KetPlusCM")
    else()
        set(ketplus_output_name "ketplus-cm")
    endif()

    set_target_properties(ketplus_cm PROPERTIES
        OUTPUT_NAME "${ketplus_output_name}"
        MACOSX_BUNDLE_BUNDLE_NAME "KetPlus CM"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.ketsuite.ketplus.cm"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )

    if(APPLE)
        set_target_properties(ketplus_cm PROPERTIES MACOSX_BUNDLE_ICON_FILE "KetPlus.icns")
    endif()

    install(TARGETS ketplus_cm
        BUNDLE DESTINATION .
        RUNTIME DESTINATION bin
    )

    install(FILES LICENSE THIRD_PARTY_NOTICES.md
        DESTINATION share/licenses/ketplus-cm
    )
endif()

if(KETPLUS_CM_BUILD_BENCHMARKS)
    qt_add_executable(ketplus_editor_benchmark benchmarks/EditorBenchmark.cpp)
    target_link_libraries(ketplus_editor_benchmark PRIVATE Qt6::Widgets ketplus_editor)
    ketplus_enable_warnings(ketplus_editor_benchmark)
endif()

if(KETPLUS_CM_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

    function(ketplus_cm_add_test test_name target)
        if(WIN32)
            add_test(
                NAME ${test_name}
                COMMAND ${target} -o "${CMAKE_BINARY_DIR}/${test_name}.txt,txt"
            )
        else()
            add_test(NAME ${test_name} COMMAND ${target} -o -,txt)
        endif()
    endfunction()

    add_executable(ketplus_tests tests/DocumentTest.cpp)
    target_link_libraries(ketplus_tests PRIVATE Qt6::Test ketplus_core)
    ketplus_enable_warnings(ketplus_tests)

    ketplus_cm_add_test(ketplus_tests ketplus_tests)

    add_executable(ketplus_editor_tests tests/EditorWidgetTest.cpp)
    target_link_libraries(ketplus_editor_tests PRIVATE Qt6::Test ketplus_editor ketplus_lexilla)
    ketplus_enable_warnings(ketplus_editor_tests)

    ketplus_cm_add_test(ketplus_editor_tests ketplus_editor_tests)
    set_tests_properties(ketplus_editor_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(ketplus_settings_tests
        tests/SettingsDialogTest.cpp
    )
    target_link_libraries(ketplus_settings_tests
        PRIVATE Qt6::Test KetPlusCM::settings_ui
    )
    ketplus_enable_warnings(ketplus_settings_tests)

    ketplus_cm_add_test(ketplus_settings_tests ketplus_settings_tests)
    set_tests_properties(ketplus_settings_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(ketplus_workspace_tests tests/ExplorerPanelTest.cpp)
    target_link_libraries(ketplus_workspace_tests PRIVATE Qt6::Test ketplus_workspace)
    ketplus_enable_warnings(ketplus_workspace_tests)

    ketplus_cm_add_test(ketplus_workspace_tests ketplus_workspace_tests)
    set_tests_properties(ketplus_workspace_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(ketplus_terminal_tests tests/PtyProcessTest.cpp)
    target_link_libraries(ketplus_terminal_tests
        PRIVATE Qt6::Test ketplus_terminal ketplus_ui
    )
    ketplus_enable_warnings(ketplus_terminal_tests)

    ketplus_cm_add_test(ketplus_terminal_tests ketplus_terminal_tests)
    set_tests_properties(ketplus_terminal_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(ketplus_git_tests tests/GitParserTest.cpp)
    target_link_libraries(ketplus_git_tests PRIVATE Qt6::Test ketplus_git)
    ketplus_enable_warnings(ketplus_git_tests)

    ketplus_cm_add_test(ketplus_git_tests ketplus_git_tests)

    add_executable(ketplus_git_diff_view_tests
        tests/GitDiffViewTest.cpp
    )
    target_link_libraries(ketplus_git_diff_view_tests
        PRIVATE Qt6::Test KetPlusCM::git_ui
    )
    ketplus_enable_warnings(ketplus_git_diff_view_tests)

    ketplus_cm_add_test(ketplus_git_diff_view_tests ketplus_git_diff_view_tests)
    set_tests_properties(ketplus_git_diff_view_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(ketplus_preview_tests tests/MarkdownPreviewPaneTest.cpp)
    target_link_libraries(ketplus_preview_tests PRIVATE Qt6::Test ketplus_preview)
    ketplus_enable_warnings(ketplus_preview_tests)

    ketplus_cm_add_test(ketplus_preview_tests ketplus_preview_tests)
    set_tests_properties(ketplus_preview_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
