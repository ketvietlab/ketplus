if(NOT DEFINED FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR
        "${CMAKE_SOURCE_DIR}/.cache/fetchcontent"
        CACHE PATH "Shared KetPlus FetchContent cache"
    )
endif()

include(FetchContent)

# Pin both projects so builds remain reproducible. They can be upgraded together.
FetchContent_Declare(
    scintilla
    GIT_REPOSITORY https://github.com/mirror/scintilla.git
    GIT_TAG a1c86144eed9e3d2187e3a8b391d11ca909f00d2
)

FetchContent_Declare(
    lexilla
    GIT_REPOSITORY https://github.com/ScintillaOrg/lexilla.git
    GIT_TAG 3e6f317eed854bc312b4c4453206189d381c7ae7
)

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

FetchContent_MakeAvailable(scintilla lexilla inter_font libvterm)

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

file(GLOB lexilla_library_sources CONFIGURE_DEPENDS
    "${lexilla_SOURCE_DIR}/lexlib/*.cxx"
    "${lexilla_SOURCE_DIR}/lexers/*.cxx"
)

add_library(ketplus_lexilla STATIC
    "${lexilla_SOURCE_DIR}/src/Lexilla.cxx"
    ${lexilla_library_sources}
)
target_include_directories(ketplus_lexilla
    PUBLIC "${lexilla_SOURCE_DIR}/include"
    PRIVATE
        "${lexilla_SOURCE_DIR}/lexlib"
        "${scintilla_SOURCE_DIR}/include"
)
target_compile_definitions(ketplus_lexilla PRIVATE _CRT_SECURE_NO_DEPRECATE=1)
set_target_properties(ketplus_lexilla PROPERTIES POSITION_INDEPENDENT_CODE ON)
