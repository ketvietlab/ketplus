# Private product typography. The font license permits app embedding, not a
# standalone font distribution: fetch the official archive and compile its
# unmodified hinted TTFs into Qt resources. Never install loose font files.
include(FetchContent)

set(KETPLUS_KET_SANS_ARCHIVE "" CACHE FILEPATH
    "Local copy of the pinned official KET Sans 1.0.0 ZIP for offline builds")
if(KETPLUS_KET_SANS_ARCHIVE)
    set(ketplus_ket_sans_url "${KETPLUS_KET_SANS_ARCHIVE}")
else()
    set(ketplus_ket_sans_url
        "https://github.com/ketvietlab/ket-sans-font/releases/download/v1.0.0/KETSans-1.000.zip")
endif()
FetchContent_Declare(ketplus_ket_sans
    URL "${ketplus_ket_sans_url}"
    URL_HASH SHA256=c6e5eb6241559de86c9d65a9b0d77aa5fabd0a3e4f8bfb7190e95c5eb18dbebc
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake-project
)
FetchContent_MakeAvailable(ketplus_ket_sans)

qt_add_resources(ketplus_typography ketplus_ui_fonts
    PREFIX "/ketplus/fonts"
    BASE "${ketplus_ket_sans_SOURCE_DIR}"
    FILES
        "${ketplus_ket_sans_SOURCE_DIR}/TTF/KETSans-Light.ttf"
        "${ketplus_ket_sans_SOURCE_DIR}/TTF/KETSans-Regular.ttf"
        "${ketplus_ket_sans_SOURCE_DIR}/TTF/KETSans-Medium.ttf"
        "${ketplus_ket_sans_SOURCE_DIR}/TTF/KETSans-Bold.ttf"
        "${ketplus_ket_sans_SOURCE_DIR}/LICENSE.txt"
)
