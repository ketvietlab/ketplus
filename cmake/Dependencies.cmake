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
    inter_font
    URL https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
    URL_HASH SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
    libvterm
    GIT_REPOSITORY https://github.com/neovim/libvterm.git
    GIT_TAG 9d6d2112335080312ef8c36667fa717ded4f7daf
)

FetchContent_MakeAvailable(scintilla ketplus_syntax inter_font libvterm)

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

set(scintilla_sources
    "${scintilla_qt_dir}/PlatQt.cpp"
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
