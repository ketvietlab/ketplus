# Writes the commit a build came from, so a running daemon can say which source it is.
# Runs at every build rather than at configure time, which would go stale after a commit.
execute_process(
    COMMAND git -C "${SOURCE_DIR}" rev-parse --short=12 HEAD
    RESULT_VARIABLE revision_result
    OUTPUT_VARIABLE revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT revision_result EQUAL 0 OR revision STREQUAL "")
    set(revision "unknown")
else()
    execute_process(
        COMMAND git -C "${SOURCE_DIR}" status --porcelain --untracked-files=no
        OUTPUT_VARIABLE changes
        ERROR_QUIET
    )
    if(NOT changes STREQUAL "")
        string(APPEND revision "-dirty")
    endif()
endif()
file(WRITE "${OUTPUT}.tmp" "#pragma once\n\n#define KETPLUS_BUILD_REVISION \"${revision}\"\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
