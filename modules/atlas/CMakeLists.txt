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
    KetQt::Widgets
)
target_include_directories(ketplus_atlas PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_atlas)
