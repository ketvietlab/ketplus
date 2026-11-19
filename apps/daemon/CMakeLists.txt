include("${PROJECT_SOURCE_DIR}/cmake/WebRtc.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake")
add_library(ketplus_daemon_core STATIC ${KETPLUS_DAEMON_CORE_SOURCES}
    src/providers/ClaudeCredentials.cpp src/providers/ClaudeCredentials.h
    src/providers/ClaudeAccountUsage.cpp src/providers/ClaudeAccountUsage.h
    src/providers/ClaudeKeychain.cpp)
target_sources(ketplus_daemon_core PRIVATE
    src/teams/TeamRiskPolicy.cpp src/teams/TeamRiskPolicy.h
    src/teams/TeamCoordinatorRiskPolicy.cpp
    src/teams/TeamRoutingEvidence.cpp src/teams/TeamRoutingEvidence.h
    src/runtime/SessionHostTeamRouting.cpp)
if(APPLE)
    target_sources(ketplus_daemon_core PRIVATE src/service/DaemonApplicationMac.mm)
    target_link_libraries(ketplus_daemon_core PRIVATE "-framework Security" "-framework CoreFoundation" "-framework Foundation" "-framework IOKit" proc)
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
    ketplus_team_language
    ketplus_control_crypto
    ketplus_tool_locations
    ketplus_runner_pairing
    ketplus_browser_tools
    ketplus_computer_tools
    ketplus_product_version
    LibDataChannel::LibDataChannelStatic
    ketplus_peer_link
    ketplus_daemon_runner_peer
)
add_library(ketplus_daemon_runner_peer STATIC
    src/runner/peer/JobPeerSession.h src/runner/peer/JobPeerSessionJob.cpp
    src/runner/peer/JobPeerSessionLink.cpp src/runner/peer/JobPeerSessionOutput.cpp
    src/runner/peer/OutputIntake.cpp src/runner/peer/OutputIntake.h
    src/runner/peer/PeerLog.cpp src/runner/peer/PeerLog.h src/runner/peer/PeerPorts.h
    src/runner/peer/RemotePeerSessions.cpp src/runner/peer/RemotePeerSessions.h
    src/runner/peer/QtJobPeerClock.cpp src/runner/peer/QtJobPeerClock.h)
target_link_libraries(ketplus_daemon_runner_peer PUBLIC Qt6::Core ketplus_runner_protocol)
target_include_directories(ketplus_daemon_runner_peer PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
ketplus_enable_warnings(ketplus_daemon_runner_peer)
# Runner snapshot sync on the desktop: keys, preparation and the sender, on abstract ports.
add_library(ketplus_daemon_runner_sync STATIC
    src/runner/sync/PackCache.cpp src/runner/sync/PackCache.h
    src/runner/sync/SnapshotKeys.cpp src/runner/sync/SnapshotKeys.h
    src/runner/sync/SnapshotPreparation.cpp src/runner/sync/SnapshotPreparation.h
    src/runner/sync/SnapshotSender.cpp src/runner/sync/SnapshotSender.h
    src/runner/sync/SnapshotSyncPorts.h
    src/runner/sync/SnapshotWorker.cpp src/runner/sync/SnapshotWorker.h
    src/runner/sync/StoreRetention.cpp src/runner/sync/StoreRetention.h)
target_link_libraries(ketplus_daemon_runner_sync PUBLIC
    Qt6::Core OpenSSL::Crypto ketplus_control_crypto ketplus_control_crypto_primitives
    ketplus_runner_protocol ketplus_runner_snapshot)
target_include_directories(ketplus_daemon_runner_sync PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
ketplus_enable_warnings(ketplus_daemon_runner_sync)
# The output spool of remote jobs, on the task output store of ketplus_daemon_core.
add_library(ketplus_daemon_runner_output STATIC
    src/runner/output/TaskOutputSpool.cpp src/runner/output/TaskOutputSpool.h)
target_link_libraries(ketplus_daemon_runner_output
    PUBLIC Qt6::Core Qt6::Sql ketplus_daemon_runner_peer
    PRIVATE ketplus_daemon_core)
ketplus_enable_warnings(ketplus_daemon_runner_output)
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
target_sources(ketplus_daemon_core PRIVATE
    src/teams/TeamReviewPolicy.h src/teams/TeamReviewPolicy.cpp
    src/teams/TeamCoordinatorFindingResolutions.cpp)
ketplus_enable_warnings(ketplus_daemon_core)
# The desktop's peer links to runners: answers runner offers and carries the three channels.
add_library(ketplus_daemon_runner_link STATIC
    src/runner/link/LinkAdapters.cpp src/runner/link/LinkAdapters.h
    src/runner/link/LinkLog.cpp src/runner/link/LinkLog.h src/runner/link/LinkPorts.h
    src/runner/link/OfferCheck.cpp src/runner/link/OfferCheck.h
    src/runner/link/RunnerLinkState.h src/runner/link/RunnerLinks.cpp
    src/runner/link/RunnerLinks.h src/runner/link/RunnerLinksChannels.cpp)
target_link_libraries(ketplus_daemon_runner_link
    PUBLIC Qt6::Core ketplus_daemon_runner_peer ketplus_peer_link ketplus_runner_pairing
    ketplus_runner_protocol
    PRIVATE ketplus_control_crypto_primitives)
target_include_directories(ketplus_daemon_runner_link PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
ketplus_enable_warnings(ketplus_daemon_runner_link)
# Remote jobs on the running daemon: the records, peer sessions, runner links, snapshot sender
# and output spool on the real registry, Relay socket and pin store, and the entry of tasks
# started with runs_on.
target_sources(ketplus_daemon_core PRIVATE
    src/runner/remote/CoordinatorRecords.cpp src/runner/remote/CoordinatorRecords.h
    src/runner/remote/RelayCancels.cpp src/runner/remote/RelayCancels.h
    src/runner/remote/RelayMessages.cpp src/runner/remote/RelayMessages.h
    src/runner/remote/RemoteJobs.cpp src/runner/remote/RemoteJobs.h
    src/runner/remote/RemoteJobsLinks.cpp src/runner/remote/RemoteJobsRestore.cpp
    src/runner/remote/RemoteJobsService.cpp src/runner/remote/RemoteJobsService.h
    src/runner/remote/SnapshotPorts.cpp src/runner/remote/SnapshotPorts.h
    src/runner/remote/SnapshotStores.cpp src/runner/remote/SnapshotStores.h
    src/runner/remote/TaskSnapshots.cpp src/runner/remote/TaskSnapshots.h
    src/runner/entry/RemoteCleanup.h src/runner/entry/RemoteGuidance.h
    src/runner/entry/RemoteJobPort.h src/runner/entry/RemoteLiveTasks.h
    src/runner/entry/RemoteOutput.h
    src/runner/entry/RemoteJobEntry.cpp src/runner/entry/RemoteJobEntry.h
    src/runner/entry/RemoteSpec.cpp src/runner/entry/RemoteSpec.h
    src/runner/entry/RunsOnRequest.cpp src/runner/entry/RunsOnRequest.h)
target_link_libraries(ketplus_daemon_core PUBLIC
    ketplus_daemon_runner_sync ketplus_daemon_runner_link ketplus_daemon_runner_output)
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

if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_subdirectory(src/runtime/executor)
    add_dependencies(ketplus_daemon ketplus_job_executor)
    if(NOT APPLE OR NOT KETPLUS_BUILD_DESKTOP)
        install(TARGETS ketplus_job_executor RUNTIME DESTINATION bin COMPONENT Runtime)
    endif()
endif()

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
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/background-policy.mjs"
            "${KETPLUS_CLAUDE_SIDECAR_SOURCE}/process-barrier.mjs"
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
        DESTINATION lib/ketplus/claude-sidecar USE_SOURCE_PERMISSIONS COMPONENT Runtime)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/agent-skills"
        DESTINATION lib/ketplus COMPONENT Runtime)
endif()

add_executable(ketplusctl src/cli/main.cpp)
target_link_libraries(ketplusctl PRIVATE ketplus_remote)
ketplus_enable_warnings(ketplusctl)
install(TARGETS ketplusctl RUNTIME DESTINATION bin COMPONENT Runtime)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include("${PROJECT_SOURCE_DIR}/packaging/linux/RuntimePlugins.cmake")
    # Qt 6.11 adds the default QPA plugins even when their type is excluded.
    qt_generate_deploy_app_script(TARGET ketplus_daemon OUTPUT_SCRIPT ketplus_daemon_deploy
        NO_TRANSLATIONS EXCLUDE_PLUGIN_TYPES platforms platformthemes styles
        EXCLUDE_PLUGINS qxcb qoffscreen ${ketplus_linux_excluded_runtime_plugins})
    install(SCRIPT "${ketplus_daemon_deploy}" COMPONENT Runtime)
    qt_generate_deploy_app_script(TARGET ketplusctl OUTPUT_SCRIPT ketplus_ctl_deploy
        NO_TRANSLATIONS EXCLUDE_PLUGIN_TYPES platforms platformthemes styles
        EXCLUDE_PLUGINS qxcb qoffscreen ${ketplus_linux_excluded_runtime_plugins})
    install(SCRIPT "${ketplus_ctl_deploy}" COMPONENT Runtime)
    qt_generate_deploy_app_script(TARGET ketplus_job_executor OUTPUT_SCRIPT ketplus_job_deploy
        NO_TRANSLATIONS EXCLUDE_PLUGIN_TYPES platforms platformthemes styles
        EXCLUDE_PLUGINS qxcb qoffscreen ${ketplus_linux_excluded_runtime_plugins})
    install(SCRIPT "${ketplus_job_deploy}" COMPONENT Runtime)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/ketplus-daemon.service"
        DESTINATION share/systemd/user COMPONENT Runtime)
endif()
