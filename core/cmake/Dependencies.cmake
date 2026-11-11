if(NOT DEFINED FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR
        "${CMAKE_SOURCE_DIR}/.cache/fetchcontent"
        CACHE PATH "Shared KetPlus FetchContent cache"
    )
endif()

include(FetchContent)

# Pin both projects so builds remain reproducible. They can be upgraded together: ketplus-syntax
# carries Lexilla and the lexer interface headers of this Scintilla release.
FetchContent_Declare(
    scintilla
    GIT_REPOSITORY https://github.com/mirror/scintilla.git
    GIT_TAG a1c86144eed9e3d2187e3a8b391d11ca909f00d2
)

# Which syntax a file gets, its Lexilla lexer and what each lexer style is; shared with the
# KetPlus phone app so code reads the same on both.
FetchContent_Declare(
    ketplus_syntax
    GIT_REPOSITORY https://github.com/ketvietlab/ketplus-syntax.git
    GIT_TAG cb7435c1e9237ee32d3459a58917b0e84b8b6025
    GIT_SUBMODULES third_party/lexilla
)
set(KETPLUS_SYNTAX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KETPLUS_SYNTAX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libvterm
    GIT_REPOSITORY https://github.com/neovim/libvterm.git
    GIT_TAG 9d6d2112335080312ef8c36667fa717ded4f7daf
)

# Két Design System tokens, Két Symbol icons and Qt Widgets components (ADR 0035). The kit is
# linked statically, so its tests, examples and install rules stay off; an app that needs the QML
# module sets KETQT_BUILD_QUICK before including core.
FetchContent_Declare(
    ketqt
    GIT_REPOSITORY https://github.com/ketvietlab/ket-qt.git
    GIT_TAG 9bdd51c200ddb6f84b7504951019bba4a1713c0c # ket-qt PR #46 merge (editable select: text once, inside its border)
    # Unlike the source-only dependencies above, the kit builds its own targets. Keep them in this
    # build tree: the shared cache is used by every build and edition at once.
    BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/ketqt-build"
)
set(KETQT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KETQT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(KETQT_INSTALL OFF CACHE BOOL "" FORCE)
if(NOT DEFINED KETQT_BUILD_QUICK)
    set(KETQT_BUILD_QUICK OFF CACHE BOOL "Build the Ket QML module (KetQt::Quick)")
endif()

FetchContent_MakeAvailable(scintilla ketplus_syntax libvterm ketqt)

add_library(ketplus_vterm STATIC
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
target_include_directories(ketplus_vterm
    PUBLIC "${libvterm_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/libvterm_generated"
        "${libvterm_SOURCE_DIR}/src"
)
set_target_properties(ketplus_vterm PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

set(scintilla_qt_dir "${scintilla_SOURCE_DIR}/qt/ScintillaEditBase")

# Scintilla's Qt port reports the viewport's position in its parent as the client rectangle,
# while its painting and hit testing assume a client origin of 0 (as the Win32 port does). A
# viewport moved down by setViewportMargins, as the editor breadcrumb does, then skips painting
# the lines under the margin. Compile a copy whose client rectangle starts at the origin; the
# pinned upstream file stays untouched in the shared dependency cache.
file(READ "${scintilla_qt_dir}/PlatQt.cpp" scintilla_platqt)
set(scintilla_client_upstream
    "PRectangle Window::GetClientPosition() const\n{\n\t// The client position is the window position\n\treturn GetPosition();\n}")
set(scintilla_client_origin
    "PRectangle Window::GetClientPosition() const\n{\n\t// KetPlus: client coordinates start at the origin (core/cmake/Dependencies.cmake).\n\treturn wid ? PRectFromQRect(window(wid)->rect()) : PRectangle(0, 0, 1000, 1000);\n}")
string(FIND "${scintilla_platqt}" "${scintilla_client_upstream}" scintilla_client_at)
if(scintilla_client_at EQUAL -1)
    message(FATAL_ERROR "Scintilla's Window::GetClientPosition changed; review the client-origin fix")
endif()
string(REPLACE "${scintilla_client_upstream}" "${scintilla_client_origin}" scintilla_platqt
               "${scintilla_platqt}")
set(scintilla_platqt_copy "${CMAKE_CURRENT_BINARY_DIR}/scintilla/PlatQt.cpp")
if(EXISTS "${scintilla_platqt_copy}")
    file(READ "${scintilla_platqt_copy}" scintilla_platqt_written)
endif()
if(NOT scintilla_platqt_written STREQUAL scintilla_platqt)
    file(WRITE "${scintilla_platqt_copy}" "${scintilla_platqt}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${scintilla_qt_dir}/PlatQt.cpp")

set(scintilla_sources
    "${scintilla_platqt_copy}"
    "${scintilla_qt_dir}/PlatQt.h"
    "${scintilla_qt_dir}/ScintillaEditBase.cpp"
    "${scintilla_qt_dir}/ScintillaEditBase.h"
    "${scintilla_qt_dir}/ScintillaQt.cpp"
    "${scintilla_qt_dir}/ScintillaQt.h"
)

set(scintilla_engine_sources
    AutoComplete.cxx
    CallTip.cxx
    CaseConvert.cxx
    CaseFolder.cxx
    CellBuffer.cxx
    ChangeHistory.cxx
    CharacterCategoryMap.cxx
    CharacterType.cxx
    CharClassify.cxx
    ContractionState.cxx
    DBCS.cxx
    Decoration.cxx
    Document.cxx
    EditModel.cxx
    Editor.cxx
    EditView.cxx
    Geometry.cxx
    Indicator.cxx
    KeyMap.cxx
    LineMarker.cxx
    MarginView.cxx
    PerLine.cxx
    PositionCache.cxx
    RESearch.cxx
    RunStyles.cxx
    ScintillaBase.cxx
    Selection.cxx
    Style.cxx
    UndoHistory.cxx
    UniConversion.cxx
    UniqueString.cxx
    ViewStyle.cxx
    XPM.cxx
)

list(TRANSFORM scintilla_engine_sources PREPEND "${scintilla_SOURCE_DIR}/src/")
list(APPEND scintilla_sources ${scintilla_engine_sources})

add_library(ketplus_scintilla STATIC ${scintilla_sources})
target_include_directories(ketplus_scintilla
    PUBLIC
        "${scintilla_qt_dir}"
        "${scintilla_SOURCE_DIR}/include"
        "${scintilla_SOURCE_DIR}/src"
)
target_compile_definitions(ketplus_scintilla
    PUBLIC EXPORT_IMPORT_API=
    PRIVATE SCINTILLA_QT=1
)
target_link_libraries(ketplus_scintilla PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Core5Compat)
set_target_properties(ketplus_scintilla PROPERTIES POSITION_INDEPENDENT_CODE ON)
