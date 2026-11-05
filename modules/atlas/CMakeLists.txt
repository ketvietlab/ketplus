add_library(ketplus_atlas STATIC
    src/atlas/AtlasProcess.cpp
    src/atlas/ManagedAtlasViewers.cpp
    src/atlas/ManagedAtlasViewers.h
    src/atlas/AtlasProcess.h
    src/atlas/AtlasThumbnailCache.cpp
    src/atlas/AtlasThumbnailCache.h
    src/atlas/AtlasTab.cpp
    src/atlas/AtlasTab.h
    src/workspace/AtlasIndex.cpp
    src/workspace/AtlasIndex.h
)
target_link_libraries(ketplus_atlas PUBLIC
    Qt6::Concurrent
    Qt6::Network
    Qt6::Svg
    Qt6::WebView
    Qt6::Widgets
    ketplus_typography
)
target_include_directories(ketplus_atlas PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_atlas)

set_source_files_properties(src/atlas/icons/external-link.svg
    PROPERTIES QT_RESOURCE_ALIAS "icons/external-link.svg"
)
foreach(icon plus rotate-ccw x)
    set_source_files_properties("${PROJECT_SOURCE_DIR}/assets/icons/lucide/${icon}.svg"
        PROPERTIES QT_RESOURCE_ALIAS "icons/${icon}.svg"
    )
endforeach()
qt_add_resources(ketplus_atlas "ketplus_atlas_resources"
    PREFIX "/atlas"
    FILES
        src/atlas/icons/external-link.svg
        "${PROJECT_SOURCE_DIR}/assets/icons/lucide/plus.svg"
        "${PROJECT_SOURCE_DIR}/assets/icons/lucide/x.svg"
        "${PROJECT_SOURCE_DIR}/assets/icons/lucide/rotate-ccw.svg"
)
