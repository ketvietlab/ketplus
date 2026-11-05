add_library(ketplus_remote STATIC
    src/DaemonPaths.cpp
    src/DaemonPaths.h
    src/DaemonClient.cpp
    src/DaemonClient.h
    src/LargeEventReassembler.cpp
    src/LargeEventReassembler.h
)
target_compile_definitions(ketplus_remote PRIVATE
    KETPLUS_AUTO_UPGRADE_DAEMON=$<BOOL:${KETPLUS_AUTO_UPGRADE_DAEMON}>)
target_link_libraries(ketplus_remote PUBLIC Qt6::Core Qt6::Network ketplus_product_version)
target_include_directories(ketplus_remote PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_remote)

# Independent backend for a Desktop acting as a paired remote client; no UI composition yet.
# Reuse the private crypto/credential target shared with the host daemon.
add_library(ketplus_remote_control STATIC
    src/control/ControlV2Client.h src/control/ControlV2Client.cpp
    src/control/ControlV2Authentication.cpp src/control/ControlV2Sessions.cpp)
target_link_libraries(ketplus_remote_control PUBLIC ketplus_control_crypto Qt6::Network)
target_include_directories(ketplus_remote_control PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
ketplus_enable_warnings(ketplus_remote_control)
