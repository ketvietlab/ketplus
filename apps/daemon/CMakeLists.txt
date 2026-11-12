include("${PROJECT_SOURCE_DIR}/cmake/WebRtc.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake")
add_library(ketplus_daemon_core STATIC ${KETPLUS_DAEMON_CORE_SOURCES}
    src/providers/ClaudeCredentials.cpp src/providers/ClaudeCredentials.h
    src/providers/ClaudeAccountUsage.cpp src/providers/ClaudeAccountUsage.h
    src/providers/ClaudeKeychain.cpp)
if(APPLE)
    target_link_libraries(ketplus_daemon_core PRIVATE "-framework Security" "-framework CoreFoundation")
endif()
target_link_libraries(ketplus_daemon_core PUBLIC
    Qt6::Concurrent
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
    OpenSSL::Crypto
    ketplus_remote
    ketplus_control_crypto
    ketplus_browser_tools
    ketplus_computer_tools
    ketplus_product_version
    LibDataChannel::LibDataChannelStatic
)
target_include_directories(ketplus_daemon_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/modules/atlas/src>"
)
target_include_directories(ketplus_daemon_core PRIVATE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/modules/ai/src>"
)
# CloudControlClient only honours a KETPLUS_CONTROL_URL environment variable behind this
# define, so a Debug daemon can reach the loopback Control API. Every other build uses the
# KETPLUS_CONTROL_URL cache entry compiled into KetPlusProductVersion.h.
target_compile_definitions(ketplus_daemon_core PUBLIC
    $<$<AND:$<BOOL:${KETPLUS_ENABLE_REMOTE_DEMO}>,$<CONFIG:Debug>>:KETPLUS_REMOTE_DEMO_AVAILABLE=1>
)
ketplus_enable_warnings(ketplus_daemon_core)
# Supported provider CLI versions (ADR 0015).
set_source_files_properties(src/providers/compat/provider-compatibility.json
    PROPERTIES QT_RESOURCE_ALIAS "provider-compatibility.json")
qt_add_resources(ketplus_daemon_core "ketplus_daemon_compat"
    PREFIX "/ketplus/daemon"
    FILES src/providers/compat/provider-compatibility.json
)

set(KETPLUS_BUILD_IDENTITY_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/KetPlusBuildIdentity.h")
add_custom_target(ketplus_build_identity
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DOUTPUT=${KETPLUS_BUILD_IDENTITY_HEADER}"
        -P "${PROJECT_SOURCE_DIR}/cmake/GenerateBuildIdentity.cmake"
    BYPRODUCTS "${KETPLUS_BUILD_IDENTITY_HEADER}"
    COMMENT "Recording the daemon's source revision"
    VERBATIM
)
add_executable(ketplus_daemon src/main.cpp)
add_dependencies(ketplus_daemon ketplus_build_identity)
target_include_directories(ketplus_daemon PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(ketplus_daemon PRIVATE ketplus_daemon_core)
ketplus_enable_warnings(ketplus_daemon)

# Claude Agent SDK sidecar: JS + production node_modules. Node itself is not
# copied; the daemon runs this tree with ketplus-browser's bundled node
# (Contents/Resources/ketplus-browser/bin/node) or a build-host node on PATH.
# Always staged: without it every Claude session fails with "Claude Agent SDK
# sidecar was not found". Staging runs `npm ci`, which needs a build-host Node
# and the network, unless KETPLUS_CLAUDE_SIDECAR_DIR points at a staged tree.
set(KETPLUS_CLAUDE_SIDECAR_DIR "" CACHE PATH
    "Prebuilt staged Claude sidecar to copy instead of running package-sidecar.mjs")

set(KETPLUS_CLAUDE_SIDECAR_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/providers/claude-sidecar")

if(KETPLUS_CLAUDE_SIDECAR_DIR)
    if(NOT EXISTS "${KETPLUS_CLAUDE_SIDECAR_DIR}/runtime.json"
       OR NOT EXISTS "${KETPLUS_CLAUDE_SIDECAR_DIR}/index.mjs"
       OR NOT EXISTS "${KETPLUS_CLAUDE_SIDECAR_DIR}/authentication.mjs"
       OR NOT EXISTS "${KETPLUS_CLAUDE_SIDECAR_DIR}/transcript.mjs"
       OR NOT EXISTS "${KETPLUS_CLAUDE_SIDECAR_DIR}/input.mjs"
       OR NOT EXISTS "${KETPLUS_CLAUDE_SIDECAR_DIR}/tasks.mjs"
       OR NOT EXISTS "${KETPLUS_CLAUDE_SIDECAR_DIR}/resume.mjs"
       OR NOT EXISTS "${KETPLUS_CLAUDE_SIDECAR_DIR}/session-delete.mjs")
        message(FATAL_ERROR
            "KETPLUS_CLAUDE_SIDECAR_DIR is missing runtime.json, index.mjs, transcript.mjs, input.mjs, tasks.mjs, resume.mjs or session-delete.mjs: ${KETPLUS_CLAUDE_SIDECAR_DIR}")
    endif()
    set(KETPLUS_CLAUDE_SIDECAR_STAGE "${KETPLUS_CLAUDE_SIDECAR_DIR}" CACHE INTERNAL
        "Staged Claude Agent SDK sidecar directory")
    set(ketplus_claude_sidecar_stamp "${KETPLUS_CLAUDE_SIDECAR_DIR}/runtime.json")
else()
    find_program(KETPLUS_NODE_EXECUTABLE node
        HINTS /opt/homebrew/bin /usr/local/bin
        DOC "Build-host Node.js used to run package-sidecar.mjs and package-runtime.mjs")
    if(NOT KETPLUS_NODE_EXECUTABLE)
        message(FATAL_ERROR
            "The Claude sidecar needs a build-host node to stage it, or KETPLUS_CLAUDE_SIDECAR_DIR "
            "pointing at a tree staged with apps/daemon/providers/claude-sidecar/package-sidecar.mjs")
    endif()
    set(KETPLUS_CLAUDE_SIDECAR_STAGE "${CMAKE_CURRENT_BINARY_DIR}/claude-sidecar-stage" CACHE INTERNAL
        "Staged Claude Agent SDK sidecar directory")
    set(ketplus_claude_sidecar_stamp "${CMAKE_CURRENT_BINARY_DIR}/claude-sidecar.stamp")
    add_custom_command(
        OUTPUT "${ketplus_claude_sidecar_stamp}"
            "${KETPLUS_CLAUDE_SIDECAR_STAGE}/runtime.json"
        COMMAND "${KETPLUS_NODE_EXECUTABLE}"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/package-sidecar.mjs"
            --out "${KETPLUS_CLAUDE_SIDECAR_STAGE}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${ketplus_claude_sidecar_stamp}"
        DEPENDS
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/authentication.mjs"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/session-usage.mjs"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/package-sidecar.mjs"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/index.mjs"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/transcript.mjs"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/input.mjs"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/tasks.mjs"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/resume.mjs"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/review-guard.mjs"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/session-delete.mjs"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/package.json"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/package-lock.json"
        COMMENT "Staging Claude Agent SDK sidecar"
        VERBATIM
    )
endif()
add_custom_target(ketplus_stage_claude_sidecar ALL
    DEPENDS "${ketplus_claude_sidecar_stamp}")
add_dependencies(ketplus_daemon ketplus_stage_claude_sidecar)

if(APPLE)
    set(ketplus_copy_claude_sidecar
        COMMAND /bin/cp -R "${KETPLUS_CLAUDE_SIDECAR_STAGE}"
            "$<TARGET_FILE_DIR:ketplus_daemon>/claude-sidecar")
else()
    set(ketplus_copy_claude_sidecar
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${KETPLUS_CLAUDE_SIDECAR_STAGE}"
            "$<TARGET_FILE_DIR:ketplus_daemon>/claude-sidecar")
endif()
add_custom_command(TARGET ketplus_daemon POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
        "$<TARGET_FILE_DIR:ketplus_daemon>/claude-sidecar"
    ${ketplus_copy_claude_sidecar}
    COMMENT "Copying Claude sidecar next to ketplus_daemon"
    VERBATIM
)

if(NOT APPLE OR NOT KETPLUS_BUILD_DESKTOP)
    install(TARGETS ketplus_daemon RUNTIME DESTINATION bin COMPONENT Runtime)
    install(DIRECTORY "${KETPLUS_CLAUDE_SIDECAR_STAGE}/"
        DESTINATION lib/ketplus/claude-sidecar COMPONENT Runtime)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/agent-skills"
        DESTINATION lib/ketplus COMPONENT Runtime)
endif()

add_executable(ketplusctl src/cli/main.cpp)
target_link_libraries(ketplusctl PRIVATE ketplus_remote)
ketplus_enable_warnings(ketplusctl)
install(TARGETS ketplusctl RUNTIME DESTINATION bin COMPONENT Runtime)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Qt 6.11 adds the default QPA plugins even when their type is excluded.
    qt_generate_deploy_app_script(TARGET ketplus_daemon OUTPUT_SCRIPT ketplus_daemon_deploy
        NO_TRANSLATIONS EXCLUDE_PLUGIN_TYPES platforms platformthemes styles
        EXCLUDE_PLUGINS qxcb qoffscreen)
    install(SCRIPT "${ketplus_daemon_deploy}" COMPONENT Runtime)
    qt_generate_deploy_app_script(TARGET ketplusctl OUTPUT_SCRIPT ketplus_ctl_deploy
        NO_TRANSLATIONS EXCLUDE_PLUGIN_TYPES platforms platformthemes styles
        EXCLUDE_PLUGINS qxcb qoffscreen)
    install(SCRIPT "${ketplus_ctl_deploy}" COMPONENT Runtime)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/ketplus-daemon.service"
        DESTINATION share/systemd/user COMPONENT Runtime)
endif()
