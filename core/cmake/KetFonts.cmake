# KÉT Sans 1.1 and KÉT Sans Mono 0.10.4, both SIL OFL 1.1 with the Reserved Font Names "KÉT" and
# "KET" (ADR 0033, #141). KÉT Sans comes from its official release archive, pinned by SHA-256.
# KÉT Sans Mono has no public release yet, so its Medium TTF and licence are committed under
# assets/fonts and shared with the mocks. Only Medium is shipped: every monospace text is Medium.
include(FetchContent)

set(KETPLUS_KET_SANS_ARCHIVE "" CACHE FILEPATH
    "Local copy of the pinned KETSans-1.100.zip for offline builds")
if(KETPLUS_KET_SANS_ARCHIVE)
    set(ketplus_ket_sans_url "${KETPLUS_KET_SANS_ARCHIVE}")
else()
    set(ketplus_ket_sans_url
        "https://github.com/ketvietlab/ket-sans-font/releases/download/v1.1.0/KETSans-1.100.zip")
endif()
FetchContent_Declare(ketplus_ket_sans
    URL "${ketplus_ket_sans_url}"
    URL_HASH SHA256=cc977aefc90a67bf6ba392b4845deae6750abbbb29a02097d4a848eedd25e34f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake-project
)
FetchContent_MakeAvailable(ketplus_ket_sans)

set_property(GLOBAL PROPERTY KETPLUS_CM_KET_SANS_SOURCE_DIR "${ketplus_ket_sans_SOURCE_DIR}")
set_property(GLOBAL PROPERTY KETPLUS_CM_KET_FONTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts")

# Compiles the four KÉT Sans faces and KÉT Sans Mono Medium, with their licences, into `target`
# under :/ketplus/fonts (TTF/KETSans-<Style>.ttf and KETSansMono-Medium.ttf). A static library
# must Q_INIT_RESOURCE(<resource_name>) and <resource_name>_mono before reading them.
function(ketplus_cm_add_ket_fonts target resource_name)
    get_property(ket_sans GLOBAL PROPERTY KETPLUS_CM_KET_SANS_SOURCE_DIR)
    get_property(ket_fonts GLOBAL PROPERTY KETPLUS_CM_KET_FONTS_DIR)
    qt_add_resources(${target} ${resource_name}
        PREFIX "/ketplus/fonts"
        BASE "${ket_sans}"
        FILES
            "${ket_sans}/TTF/KETSans-Light.ttf"
            "${ket_sans}/TTF/KETSans-Regular.ttf"
            "${ket_sans}/TTF/KETSans-Medium.ttf"
            "${ket_sans}/TTF/KETSans-Bold.ttf"
            "${ket_sans}/LICENSE.txt"
    )
    qt_add_resources(${target} ${resource_name}_mono
        PREFIX "/ketplus/fonts"
        BASE "${ket_fonts}"
        FILES
            "${ket_fonts}/KETSansMono-Medium.ttf"
            "${ket_fonts}/KET-Sans-Mono-OFL-1.1.txt"
    )
endfunction()
