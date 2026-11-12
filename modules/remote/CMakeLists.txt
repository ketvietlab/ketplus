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
    src/SessionCatalogReader.cpp
    src/SessionCatalogReader.h
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
    src/control/ControlV2Transcript.cpp)
target_link_libraries(ketplus_remote_control PUBLIC ketplus_control_crypto Qt6::Network)
target_include_directories(ketplus_remote_control PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>")
ketplus_enable_warnings(ketplus_remote_control)

# SSH route to a Linux host's daemon through the system OpenSSH client (docs/ssh-transport.md).
add_library(ketplus_remote_ssh STATIC
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
