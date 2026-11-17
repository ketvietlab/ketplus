cmake_minimum_required(VERSION 3.24)
project(R03CloudPrivacy VERSION 0.1.1 LANGUAGES CXX)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "Use Release for production boundary characterization")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.11 REQUIRED COMPONENTS Concurrent Core Gui Network Sql Test WebSockets)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include("${REPO}/cmake/Warnings.cmake")
configure_file("${REPO}/cmake/KetPlusProductVersion.h.in" "${CMAKE_CURRENT_BINARY_DIR}/KetPlusProductVersion.h" @ONLY)
include("${REPO}/cmake/WebRtc.cmake")
set(DAEMON "${REPO}/apps/daemon/src")
# The daemon's own source lists, so this build cannot fall behind the daemon it characterizes.
include("${REPO}/apps/daemon/sources.cmake")
include("${REPO}/modules/browser/sources.cmake")
list(TRANSFORM KETPLUS_DAEMON_CORE_SOURCES PREPEND "${REPO}/apps/daemon/")
list(TRANSFORM KETPLUS_BROWSER_TOOLS_SOURCES PREPEND "${REPO}/modules/browser/")
# CredentialStore.cpp (OS key storage) is replaced by MemoryCredentials.cpp.
add_library(r03_privacy_core STATIC MemoryCredentials.cpp ${KETPLUS_DAEMON_CORE_SOURCES}
  ${KETPLUS_BROWSER_TOOLS_SOURCES} "${REPO}/modules/remote/src/DaemonPaths.cpp"
  "${REPO}/modules/remote/src/TeamLanguage.cpp"
  "${REPO}/modules/remote/src/TranscriptVisibility.cpp"
  "${REPO}/modules/remote/src/Schedule.cpp"
  "${REPO}/modules/remote/src/cloud/CloudCrypto.cpp" "${REPO}/modules/remote/src/cloud/BrowserSignIn.cpp"
  "${REPO}/modules/computer/src/ComputerAgentInstructions.cpp"
  "${REPO}/modules/computer/src/ComputerToolLaunch.cpp"
  "${REPO}/modules/remote/src/DaemonCredentialFile.cpp"
  "${DAEMON}/providers/ClaudeCredentials.cpp" "${DAEMON}/providers/ClaudeAccountUsage.cpp"
  "${DAEMON}/providers/ClaudeKeychain.cpp")
set_source_files_properties("${DAEMON}/providers/compat/provider-compatibility.json"
  PROPERTIES QT_RESOURCE_ALIAS "provider-compatibility.json")
qt_add_resources(r03_privacy_core "r03_daemon_compat" PREFIX "/ketplus/daemon"
  FILES "${DAEMON}/providers/compat/provider-compatibility.json")
# Only OS key storage is replaced; encryption and HTTP request serialization are production code.
target_include_directories(r03_privacy_core PUBLIC "${DAEMON}" "${REPO}/modules/ai/src" "${REPO}/modules/atlas/src" "${REPO}/modules/browser/src" "${REPO}/modules/remote/src"
  "${REPO}/modules/computer/src"
  "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(r03_privacy_core PUBLIC Qt6::Concurrent Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql Qt6::WebSockets
  OpenSSL::Crypto LibDataChannel::LibDataChannelStatic)
if(APPLE)
  # Linked like the daemon; the Claude login entries are not used by these tests.
  target_link_libraries(r03_privacy_core PRIVATE "-framework Security" "-framework CoreFoundation")
endif()
add_executable(r03_cloud_privacy_tests CloudPrivacyTest.cpp)
target_link_libraries(r03_cloud_privacy_tests PRIVATE r03_privacy_core Qt6::Test)
add_executable(r03_daemon_protocol_regression "${REPO}/tests/daemon/DaemonProtocolTest.cpp")
target_link_libraries(r03_daemon_protocol_regression PRIVATE r03_privacy_core Qt6::Test)
add_executable(r03_daemon_transcript_window_regression
  "${REPO}/tests/daemon/DaemonTranscriptWindowTest.cpp")
target_link_libraries(r03_daemon_transcript_window_regression PRIVATE r03_privacy_core Qt6::Test)
ketplus_enable_warnings(r03_privacy_core)
ketplus_enable_warnings(r03_cloud_privacy_tests)
ketplus_enable_warnings(r03_daemon_protocol_regression)
ketplus_enable_warnings(r03_daemon_transcript_window_regression)
enable_testing()
add_test(NAME r03_cloud_privacy_tests COMMAND r03_cloud_privacy_tests)
add_test(NAME r03_daemon_protocol_regression COMMAND r03_daemon_protocol_regression)
add_test(NAME r03_daemon_transcript_window_regression
  COMMAND r03_daemon_transcript_window_regression)

add_executable(r03_cloud_trust_lifecycle_tests CloudTrustLifecycleTest.cpp)
target_link_libraries(r03_cloud_trust_lifecycle_tests PRIVATE r03_privacy_core Qt6::Test)
ketplus_enable_warnings(r03_cloud_trust_lifecycle_tests)
add_test(NAME r03_cloud_trust_lifecycle_tests COMMAND r03_cloud_trust_lifecycle_tests)
add_executable(r03_sender_key_cache_tests SenderKeyCacheTest.cpp)
target_link_libraries(r03_sender_key_cache_tests PRIVATE r03_privacy_core Qt6::Test)
ketplus_enable_warnings(r03_sender_key_cache_tests)
add_test(NAME r03_sender_key_cache_tests COMMAND r03_sender_key_cache_tests)

# Match the routing sources added to the production daemon composition.
target_sources(r03_privacy_core PRIVATE
  "${DAEMON}/teams/TeamRiskPolicy.cpp"
  "${DAEMON}/teams/TeamCoordinatorRiskPolicy.cpp"
  "${DAEMON}/teams/TeamRoutingEvidence.cpp"
  "${DAEMON}/teams/TeamReviewPolicy.cpp"
  "${DAEMON}/teams/TeamCoordinatorFindingResolutions.cpp"
  "${DAEMON}/runtime/SessionHostTeamRouting.cpp")

# Remote host-pin regressions share the synthetic credential store, never the OS store.
add_executable(r03_remote_control_client_tests "${REPO}/tests/daemon/RemoteControlClientTest.cpp"
  "${REPO}/modules/remote/src/LargeEventReassembler.cpp"
  "${REPO}/modules/remote/src/control/ControlV2Client.cpp"
  "${REPO}/modules/remote/src/control/ControlV2Authentication.cpp"
  "${REPO}/modules/remote/src/control/ControlV2Sessions.cpp"
  "${REPO}/modules/remote/src/control/ControlV2Projection.cpp"
  "${REPO}/modules/remote/src/control/ControlV2Restore.cpp"
  "${REPO}/modules/remote/src/control/ControlV2Transcript.cpp"
  "${REPO}/modules/remote/src/control/ControlV2Worktrees.cpp")
target_link_libraries(r03_remote_control_client_tests PRIVATE r03_privacy_core Qt6::Test)
ketplus_enable_warnings(r03_remote_control_client_tests)
add_test(NAME r03_remote_control_client_tests COMMAND r03_remote_control_client_tests)

# Use the real store/locking/codec with a counting OS adapter, never a user keychain.
add_executable(r03_credential_batch_tests CredentialBatchTest.cpp
  "${REPO}/modules/remote/src/cloud/CredentialStore.cpp")
target_link_libraries(r03_credential_batch_tests PRIVATE r03_privacy_core Qt6::Test)
ketplus_enable_warnings(r03_credential_batch_tests)
add_test(NAME r03_credential_batch_tests COMMAND r03_credential_batch_tests)
