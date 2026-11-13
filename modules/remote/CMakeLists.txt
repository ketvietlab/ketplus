add_library(ketplus_remote STATIC
    src/DaemonCredentialFile.cpp
    src/DaemonCredentialFile.h
    src/DaemonPaths.cpp
    src/DaemonPaths.h
    src/DaemonClient.cpp
    src/DaemonClient.h
    src/DaemonMachineBackend.cpp
    src/DaemonMachineBackend.h
    src/MachineBackend.h
    src/MachineAttachmentUpload.cpp
    src/MachineAttachmentUpload.h
    src/MachineBackend.cpp
    src/MachineLargeEvents.cpp
    src/MachineLargeEvents.h
    src/MachineRows.cpp
    src/MachineRows.h
    src/SessionCatalogReader.cpp
    src/SessionCatalogReader.h
    src/LargeEventReassembler.cpp
    src/LargeEventReassembler.h
    src/TranscriptVisibility.cpp
    src/TranscriptVisibility.h
)
target_compile_definitions(ketplus_remote PRIVATE
    KETPLUS_AUTO_UPGRADE_DAEMON=$<BOOL:${KETPLUS_AUTO_UPGRADE_DAEMON}>)
target_link_libraries(ketplus_remote PUBLIC Qt6::Core Qt6::Network ketplus_product_version)
target_include_directories(ketplus_remote PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_remote)

# T-56: a team's language, and the team texts in each translated language. Shared by the
# daemon, which writes a team's Updates and decisions, and the desktop team page.
add_library(ketplus_team_language STATIC
    src/TeamLanguage.cpp
    src/TeamLanguage.h
)
target_link_libraries(ketplus_team_language PUBLIC Qt6::Core)
target_include_directories(ketplus_team_language PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_team_language)
# Only lrelease runs in the build. `cmake --build <dir> --target ketplus_team_language_lupdate`
# refreshes the .ts files from the team sources, in the one "Team" context the translator
# reads (ADR 0120, docs/development.md).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND AND TARGET Qt6::lupdate)
    add_custom_target(ketplus_team_language_lupdate
        COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/translations/update_team_translations.py"
            "$<TARGET_FILE:Qt6::lupdate>" "${CMAKE_CURRENT_SOURCE_DIR}/../.."
            "${CMAKE_CURRENT_SOURCE_DIR}/translations/ketplus_teams_vi.ts"
        VERBATIM)
endif()
qt_add_lrelease(TS_FILES translations/ketplus_teams_vi.ts
    QM_FILES_OUTPUT_VARIABLE ketplus_team_qm_files)
qt_add_resources(ketplus_team_language ketplus_team_translations
    PREFIX "/ketplus/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES ${ketplus_team_qm_files})

# Device keys, credential storage and browser sign-in, shared by the host daemon and a
# Desktop acting as a paired remote client.
add_library(ketplus_control_crypto STATIC
    src/cloud/CloudCrypto.cpp src/cloud/CloudCrypto.h
    src/cloud/CredentialStore.cpp src/cloud/CredentialStore.h
    src/cloud/BrowserSignIn.cpp src/cloud/BrowserSignIn.h
    src/cloud/ControlEndpoint.h src/cloud/HostPlatform.h)
target_link_libraries(ketplus_control_crypto PUBLIC Qt6::Core Qt6::Network OpenSSL::Crypto ketplus_remote)
target_include_directories(ketplus_control_crypto PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
ketplus_enable_warnings(ketplus_control_crypto)

# Independent backend for a Desktop acting as a paired remote client; no UI composition yet.
add_library(ketplus_remote_control STATIC
    src/control/ControlV2Client.h src/control/ControlV2Client.cpp
    src/control/ControlV2Authentication.cpp src/control/ControlV2Sessions.cpp
    src/control/ControlV2Content.h src/control/ControlV2Projection.cpp
    src/control/ControlV2Restore.cpp
    src/control/ControlV2Restore.h
    src/control/ControlV2Transcript.cpp
    src/control/ControlV2Worktrees.cpp
    src/control/CommandOutbox.h src/control/CommandOutbox.cpp
    src/control/RelayMachineBackend.h src/control/RelayMachineBackend.cpp
    src/control/RelayMachineCommands.cpp src/control/RelayMachinePreparation.cpp)
target_link_libraries(ketplus_remote_control PUBLIC ketplus_control_crypto Qt6::Network)
target_include_directories(ketplus_remote_control PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
ketplus_enable_warnings(ketplus_remote_control)

# SSH route to a Linux host's daemon through the system OpenSSH client (docs/ssh-transport.md).
add_library(ketplus_remote_ssh STATIC
    src/ssh/KnownHostsFile.cpp src/ssh/KnownHostsFile.h
    src/ssh/SshHostKeys.cpp src/ssh/SshHostKeys.h
    src/ssh/SshProcess.h src/ssh/SshTunnel.cpp src/ssh/SshTunnel.h
    src/ssh/SshTunnelBridge.cpp src/ssh/SshTunnelHostKey.cpp)
target_link_libraries(ketplus_remote_ssh PUBLIC Qt6::Core Qt6::Network)
target_include_directories(ketplus_remote_ssh PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
ketplus_enable_warnings(ketplus_remote_ssh)

# ADR 0119 machine backend for a Linux host over SshTunnel, with its persistent machine store.
add_library(ketplus_remote_ssh_backend STATIC
    src/ssh/SshMachineBackend.cpp src/ssh/SshMachineBackend.h
    src/ssh/SshMachineStore.cpp src/ssh/SshMachineStore.h)
target_link_libraries(ketplus_remote_ssh_backend PUBLIC ketplus_remote_ssh ketplus_remote)
target_include_directories(ketplus_remote_ssh_backend PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
ketplus_enable_warnings(ketplus_remote_ssh_backend)
