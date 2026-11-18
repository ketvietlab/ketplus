# Worktree snapshots for runner jobs: create, pack and apply with the system git.
add_library(ketplus_runner_snapshot STATIC
    src/snapshot/UserGitRules.cpp
    src/snapshot/UserGitRules.h
    src/snapshot/RepositoryLayout.cpp
    src/snapshot/RepositoryLayout.h
    src/snapshot/SealedGit.cpp
    src/snapshot/SealedGit.h
    src/snapshot/SnapshotResult.h
    src/snapshot/SnapshotStore.cpp src/snapshot/SnapshotStore.h
    src/snapshot/WorkspaceApply.cpp
    src/snapshot/WorkspaceApply.h
    src/snapshot/WorkspaceStore.cpp
    src/snapshot/WorkspaceStore.h
    src/snapshot/WorktreeSnapshot.cpp
    src/snapshot/WorktreeSnapshot.h
)
target_link_libraries(ketplus_runner_snapshot PUBLIC Qt6::Core)
target_include_directories(ketplus_runner_snapshot PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_runner_snapshot)

# Runner pairing: token, fingerprints, pairing code, signed payloads and pin records, shared by
# the desktop daemon and the runner. Keys are passed in; nothing is stored.
add_library(ketplus_runner_pairing STATIC
    src/pairing/PairingIdentity.cpp
    src/pairing/PairingIdentity.h
    src/pairing/PairingPins.cpp
    src/pairing/PairingPins.h
    src/pairing/PairingResult.cpp
    src/pairing/PairingResult.h
    src/pairing/PairingSignatures.cpp
    src/pairing/PairingSignatures.h
    src/pairing/PairingToken.cpp
    src/pairing/PairingToken.h
)
target_link_libraries(ketplus_runner_pairing
    PUBLIC Qt6::Core
    PRIVATE ketplus_control_crypto_primitives OpenSSL::Crypto)
target_include_directories(ketplus_runner_pairing PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_runner_pairing)

# Runner peer protocol frames: types, JSON encode/decode and limit checks.
add_library(ketplus_runner_protocol STATIC
    src/protocol/Capabilities.h
    src/protocol/ErrorCodes.cpp
    src/protocol/ErrorCodes.h
    src/protocol/FrameDecode.cpp
    src/protocol/FrameEncode.cpp
    src/protocol/FrameJson.h
    src/protocol/FrameLimits.h
    src/protocol/FrameRoutes.cpp
    src/protocol/FrameRoutes.h
    src/protocol/FrameRules.h
    src/protocol/FrameValidation.cpp
    src/protocol/Frames.h
)
target_link_libraries(ketplus_runner_protocol PUBLIC Qt6::Core)
target_include_directories(ketplus_runner_protocol PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_runner_protocol)
