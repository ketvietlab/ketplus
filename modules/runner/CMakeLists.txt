# Worktree snapshots for runner jobs: create, pack and apply with the system git.
add_library(ketplus_runner_snapshot STATIC
    src/snapshot/ExcludeRules.cpp
    src/snapshot/ExcludeRules.h
    src/snapshot/RepositoryLayout.cpp
    src/snapshot/RepositoryLayout.h
    src/snapshot/SealedGit.cpp
    src/snapshot/SealedGit.h
    src/snapshot/SnapshotResult.h
    src/snapshot/WorkspaceApply.cpp
    src/snapshot/WorkspaceApply.h
    src/snapshot/WorktreeSnapshot.cpp
    src/snapshot/WorktreeSnapshot.h
)
target_link_libraries(ketplus_runner_snapshot PUBLIC Qt6::Core)
target_include_directories(ketplus_runner_snapshot PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_runner_snapshot)
