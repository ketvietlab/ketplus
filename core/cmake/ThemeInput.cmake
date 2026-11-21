# Source-only integration keeps upstream's examples, install rules and global
# compiler flags out of the application build. MIT notice ships with both editions.
include(FetchContent)
FetchContent_Declare(ketplus_miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 77d0dce8627735138c51770d1799a1ef48f2117d # 3.1.2
    SOURCE_SUBDIR ketplus-source-only
)
FetchContent_MakeAvailable(ketplus_miniz)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/miniz-include")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/miniz-include/miniz_export.h" "#pragma once\n#define MINIZ_EXPORT\n")
add_library(ketplus_miniz STATIC
    "${ketplus_miniz_SOURCE_DIR}/miniz.c"
    "${ketplus_miniz_SOURCE_DIR}/miniz_tdef.c"
    "${ketplus_miniz_SOURCE_DIR}/miniz_tinfl.c"
    "${ketplus_miniz_SOURCE_DIR}/miniz_zip.c"
)
target_include_directories(ketplus_miniz SYSTEM PUBLIC
    "${ketplus_miniz_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/miniz-include")
set_target_properties(ketplus_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(ketplus_theme_io STATIC
    src/ui/theme/io/ThemeInput.h
    src/ui/theme/io/Jsonc.cpp
    src/ui/theme/io/Xml.cpp
    src/ui/theme/io/ZipArchive.cpp
    src/ui/theme/io/IncludeResolver.cpp
)
target_link_libraries(ketplus_theme_io PUBLIC Qt6::Core PRIVATE ketplus_miniz)
target_include_directories(ketplus_theme_io PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
ketplus_enable_warnings(ketplus_theme_io)
add_library(KetPlusCM::theme_io ALIAS ketplus_theme_io)
