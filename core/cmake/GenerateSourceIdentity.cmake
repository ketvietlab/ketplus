# SPDX-License-Identifier: MPL-2.0
# Generate at build time, so About cannot retain a stale configure-time revision.
set(revision "unknown")
execute_process(COMMAND git -C "${SOURCE_DIR}" rev-parse --verify HEAD
    OUTPUT_VARIABLE candidate OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    RESULT_VARIABLE result)
if(result EQUAL 0 AND candidate MATCHES "^[0-9a-f]+$")
    string(LENGTH "${candidate}" length)
    if(length EQUAL 40 OR length EQUAL 64)
        set(revision "${candidate}")
        execute_process(COMMAND git -c core.fsmonitor=false -C "${SOURCE_DIR}"
            status --porcelain --untracked-files=normal
            OUTPUT_VARIABLE changes ERROR_QUIET RESULT_VARIABLE status_result)
        if(NOT status_result EQUAL 0 OR NOT changes STREQUAL "")
            string(APPEND revision "-dirty")
        endif()
    endif()
endif()
file(WRITE "${OUTPUT}.tmp" "#pragma once\n#define KETPLUS_ABOUT_SOURCE_REVISION \"${revision}\"\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
